// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstdio>
#include <cstdlib> // for strtoul()
#include <chrono>
#include <cstdint> // for uint32_t
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>

#include <libtransmission/transmission.h>

#include <libtransmission/announce-list.h>
#include <libtransmission/error.h>
#include <libtransmission/file.h>
#include <libtransmission/log.h>
#include <libtransmission/makemeta.h>
#include <libtransmission/torrent-files.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h>
#include <libtransmission/values.h>
#include <libtransmission/version.h>

#include <utils/tools.h>

using namespace std::literals;

using namespace libtransmission::Values;

namespace
{

char constexpr MyName[] = "transmission create";
char constexpr Usage[] = "Usage: transmission create [options] <file|directory>";

uint32_t constexpr KiB = 1024;

auto constexpr Options = std::array<tr_option, 10>{
    { { 'p', "private", "Allow this torrent to only be used with the specified tracker(s)", "p", false, nullptr },
      { 'r', "source", "Set the source for private trackers", "r", true, "<source>" },
      { 'o', "outfile", "Save the generated .torrent to this filename", "o", true, "<file>" },
      { 's', "piecesize", "Set the piece size in KiB, overriding the preferred default", "s", true, "<KiB>" },
      { 'c', "comment", "Add a comment", "c", true, "<comment>" },
      { 't', "tracker", "Add a tracker's announce URL", "t", true, "<url>" },
      { 'w', "webseed", "Add a webseed URL", "w", true, "<url>" },
      { 'x', "anonymize", R"(Omit "Creation date" and "Created by" info)", nullptr, false, nullptr },
      { 'V', "version", "Show version number and exit", "V", false, nullptr },
      { 0, nullptr, nullptr, nullptr, false, nullptr } }
};

struct app_options
{
    tr_announce_list trackers;
    std::vector<std::string> webseeds;
    std::string outfile;
    std::string_view comment;
    std::string_view infile;
    std::string_view source;
    uint32_t piece_size = 0;
    bool anonymize = false;
    bool is_private = false;
    bool show_version = false;
};

int parseCommandLine(app_options& options, int argc, char const* const* argv)
{
    int c;
    char const* optarg;

    while ((c = tr_getopt(Usage, argc, argv, std::data(Options), &optarg)) != TR_OPT_DONE)
    {
        switch (c)
        {
        case 'V':
            options.show_version = true;
            break;

        case 'p':
            options.is_private = true;
            break;

        case 'o':
            options.outfile = optarg;
            break;

        case 'c':
            options.comment = optarg;
            break;

        case 't':
            options.trackers.add(optarg, options.trackers.nextTier());
            break;

        case 'w':
            options.webseeds.emplace_back(optarg);
            break;

        case 's':
            if (optarg != nullptr)
            {
                char* endptr = nullptr;
                options.piece_size = strtoul(optarg, &endptr, 10) * KiB;
                if (endptr != nullptr && *endptr == 'M')
                {
                    options.piece_size *= KiB;
                }
            }
            break;

        case 'r':
            options.source = optarg;
            break;

        case 'x':
            options.anonymize = true;
            break;

        case TR_OPT_UNK:
            options.infile = optarg;
            break;

        default:
            return 1;
        }
    }

    return 0;
}
} // namespace

static
int do_tr_create(int argc, char* argv[])
{
    tr_locale_set_global("");

    tr_logSetLevel(TR_LOG_ERROR);

    auto options = app_options{};
    if (parseCommandLine(options, argc, (char const* const*)argv) != 0)
    {
        return EXIT_FAILURE;
    }

    if (options.show_version)
    {
        fprintf(stderr, "%s %s\n", MyName, LONG_VERSION_STRING);
        return EXIT_SUCCESS;
    }

    if (std::empty(options.infile))
    {
        fprintf(stderr, "ERROR: No input file or directory specified.\n");
        tr_getopt_usage(MyName, Usage, std::data(Options));
        fprintf(stderr, "\n");
        return EXIT_FAILURE;
    }

    if (std::empty(options.outfile))
    {
        auto error = tr_error{};
        auto const base = tr_sys_path_basename(options.infile, &error);
        if (error)
        {
            auto const errmsg = fmt::format(
                "Couldn't use '{path}': {error} ({error_code})",
                fmt::arg("path", options.infile),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code()));
            fmt::print(stderr, "{:s}\n", errmsg);
            return EXIT_FAILURE;
        }

        auto const cur = tr_sys_dir_get_current(&error);
        if (error)
        {
            auto const errmsg = fmt::format(
                "Couldn't create '{path}': {error} ({error_code})",
                fmt::arg("path", base),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code()));
            fmt::print(stderr, "{:s}\n", errmsg);
            return EXIT_FAILURE;
        }

        options.outfile = fmt::format("{:s}/{:s}.torrent"sv, cur, base);
    }

    if (std::empty(options.trackers))
    {
        if (options.is_private)
        {
            fprintf(stderr, "ERROR: no trackers specified for a private torrent\n");
            return EXIT_FAILURE;
        }
        else
        {
            printf("WARNING: no trackers specified\n");
        }
    }

    fmt::print("Creating torrent \"{:s}\"\n", options.outfile);

    auto builder = tr_metainfo_builder(options.infile);
    auto const n_files = builder.file_count();
    if (n_files == 0U)
    {
        fprintf(stderr, "ERROR: Cannot find specified input file or directory.\n");
        return EXIT_FAILURE;
    }

    for (tr_file_index_t i = 0; i < n_files; ++i)
    {
        auto const& path = builder.path(i);
        if (!tr_torrent_files::is_subpath_sanitized(path, false))
        {
            fmt::print(stderr, "WARNING\n");
            fmt::print(stderr, "filename \"{:s}\" may not be portable on all systems.\n", path);
            fmt::print(stderr, "consider \"{:s}\" instead.\n", tr_torrent_files::sanitize_subpath(path, false));
        }
    }

    if (options.piece_size != 0 && !builder.set_piece_size(options.piece_size))
    {
        fmt::print(stderr, "ERROR: piece size must be at least 16 KiB and must be a power of two.\n");
        return EXIT_FAILURE;
    }

    fmt::print(
        tr_ngettext("{file_count:L} file, {total_size}\n", "{file_count:L} files, {total_size}\n", builder.file_count()),
        fmt::arg("file_count", builder.file_count()),
        fmt::arg("total_size", Storage{ builder.total_size(), Storage::Units::Bytes }.to_string()));

    fmt::print(
        tr_ngettext(
            "{piece_count:L} piece, {piece_size}\n",
            "{piece_count:L} pieces, {piece_size} each\n",
            builder.piece_count()),
        fmt::arg("piece_count", builder.piece_count()),
        fmt::arg("piece_size", Memory{ builder.piece_size(), Memory::Units::Bytes }.to_string()));

    if (!std::empty(options.comment))
    {
        builder.set_comment(options.comment);
    }

    if (!std::empty(options.source))
    {
        builder.set_source(options.source);
    }

    builder.set_private(options.is_private);
    builder.set_anonymize(options.anonymize);
    builder.set_webseeds(std::move(options.webseeds));
    builder.set_announce_list(std::move(options.trackers));

    auto future = builder.make_checksums();
    auto last = std::optional<tr_piece_index_t>{};
    while (future.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready)
    {
        auto const [current, total] = builder.checksum_status();

        if (!last || current != *last)
        {
            fmt::print("\rPiece {:d}/{:d} ...", current, total);
            fflush(stdout);
            last = current;
        }
    }

    fmt::print(" ");

    if (auto error = future.get(); error)
    {
        fmt::print("ERROR: {:s} {:d}\n", error.message(), error.code());
        return EXIT_FAILURE;
    }

    if (auto error = tr_error{}; !builder.save(options.outfile, &error))
    {
        auto const errmsg = fmt::format(
            "Couldn't save '{path}': {error} ({error_code})",
            fmt::arg("path", options.outfile),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code()));
        fmt::print(stderr, "{:s}\n", errmsg);
        return EXIT_FAILURE;
    }

    fmt::print("done!\n");
    return EXIT_SUCCESS;
}

struct tr_cmd tr_create = {
	.name = "create",
	.cmd = do_tr_create,
};
