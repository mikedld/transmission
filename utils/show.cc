// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint> // int64_t
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <iterator> // std::back_inserter
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/core.h>

#include <libtransmission/transmission.h>

#include <libtransmission/error.h>
#include <libtransmission/log.h>
#include <libtransmission/quark.h>
#include <libtransmission/torrent-metainfo.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/tr-strbuf.h>
#include <libtransmission/utils.h>
#include <libtransmission/values.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>
#include <libtransmission/web.h>
#include <libtransmission/web-utils.h>

#include <utils/tools.h>

using namespace std::literals;
using namespace libtransmission::Values;

namespace
{

auto constexpr TimeoutSecs = std::chrono::seconds{ 30 };

char constexpr MyName[] = "transmission-show";
char constexpr Usage[] = "Usage: transmission-show [options] <torrent-file>";

auto options = std::array<tr_option, 14>{
    { { 'd', "header", "Show only header section", "d", false, nullptr },
      { 'i', "info", "Show only info section", "i", false, nullptr },
      { 't', "trackers", "Show only trackers section", "t", false, nullptr },
      { 'f', "files", "Show only file list", "f", false, nullptr },
      { 'D', "no-header", "Do not show header section", "D", false, nullptr },
      { 'I', "no-info", "Do not show info section", "I", false, nullptr },
      { 'T', "no-trackers", "Do not show trackers section", "T", false, nullptr },
      { 'F', "no-files", "Do not show files section", "F", false, nullptr },
      { 'b', "bytes", "Show file sizes in bytes", "b", false, nullptr },
      { 'm', "magnet", "Give a magnet link for the specified torrent", "m", false, nullptr },
      { 's', "scrape", "Ask the torrent's trackers how many peers are in the torrent's swarm", "s", false, nullptr },
      { 'u', "unsorted", "Do not sort files by name", "u", false, nullptr },
      { 'V', "version", "Show version number and exit", "V", false, nullptr },
      { 0, nullptr, nullptr, nullptr, false, nullptr } }
};

struct app_opts
{
    std::string_view filename;
    bool scrape = false;
    bool show_magnet = false;
    bool show_version = false;
    bool unsorted = false;
    bool print_header = true;
    bool print_info = true;
    bool print_trackers = true;
    bool print_files = true;
    bool show_bytesize = false;
};

int parseCommandLine(app_opts& opts, int argc, char const* const* argv)
{
    int c;
    char const* optarg;

    while ((c = tr_getopt(Usage, argc, argv, std::data(options), &optarg)) != TR_OPT_DONE)
    {
        switch (c)
        {
        case 'b':
            opts.show_bytesize = true;
            break;

        case 'f':
            opts.print_header = false;
            opts.print_info = false;
            opts.print_trackers = false;
            opts.print_files = true;
            break;

        case 'd':
            opts.print_header = true;
            opts.print_info = false;
            opts.print_trackers = false;
            opts.print_files = false;
            break;

        case 'i':
            opts.print_header = false;
            opts.print_info = true;
            opts.print_trackers = false;
            opts.print_files = false;
            break;

        case 't':
            opts.print_header = false;
            opts.print_info = false;
            opts.print_trackers = true;
            opts.print_files = false;
            break;

        case 'D':
            opts.print_header = false;
            break;

        case 'I':
            opts.print_info = false;
            break;

        case 'T':
            opts.print_trackers = false;
            break;

        case 'F':
            opts.print_files = false;
            break;

        case 'm':
            opts.show_magnet = true;
            break;

        case 's':
            opts.scrape = true;
            break;

        case 'u':
            opts.unsorted = true;
            break;

        case 'V':
            opts.show_version = true;
            break;

        case TR_OPT_UNK:
            opts.filename = optarg;
            break;

        default:
            return 1;
        }
    }

    return 0;
}

[[nodiscard]] auto toString(time_t now)
{
    return now == 0 ? "Unknown" : fmt::format("{:%a %b %d %T %Y}", fmt::localtime(now));
}

bool compareSecondField(std::string_view l, std::string_view r)
{
    auto const lpos = l.find(' ');
    if (lpos == std::string_view::npos)
    {
        return false;
    }

    auto const rpos = r.find(' ');
    if (rpos == std::string_view::npos)
    {
        return true;
    }

    return l.substr(lpos) <= r.substr(rpos);
}

void showInfo(app_opts const& opts, tr_torrent_metainfo const& metainfo)
{
    /**
    ***  General Info
    **/
    if (opts.print_info)
    {
        fmt::print("GENERAL\n\n");
        fmt::print("  Name: {:s}\n", metainfo.name());
        if (metainfo.has_v1_metadata())
        {
            fmt::print("  Hash v1: {:s}\n", metainfo.info_hash_string());
        }
        if (metainfo.has_v2_metadata())
        {
            fmt::print("  Hash v2: {:s}\n", metainfo.info_hash2_string());
        }
        fmt::print("  Created by: {:s}\n", std::empty(metainfo.creator()) ? "Unknown" : metainfo.creator());
        fmt::print("  Created on: {:s}\n\n", toString(metainfo.date_created()));

        if (!std::empty(metainfo.comment()))
        {
            fmt::print("  Comment: {:s}\n", metainfo.comment());
        }

        if (!std::empty(metainfo.source()))
        {
            fmt::print("  Source: {:s}\n", metainfo.source());
        }

        fmt::print("  Piece Count: {:d}\n", metainfo.piece_count());
        fmt::print("  Piece Size: {:s}\n", Memory{ metainfo.piece_size(), Memory::Units::Bytes }.to_string());
        fmt::print("  Total Size: {:s}\n", Storage{ metainfo.total_size(), Storage::Units::Bytes }.to_string());
        fmt::print("  Privacy: {:s}\n", metainfo.is_private() ? "Private torrent" : "Public torrent");
    }

    /**
    ***  Trackers
    **/

    if (opts.print_trackers)
    {
        fmt::print("\nTRACKERS\n");
        auto current_tier = std::optional<tr_tracker_tier_t>{};
        auto print_tier = size_t{ 1 };
        for (auto const& tracker : metainfo.announce_list())
        {
            if (!current_tier || current_tier != tracker.tier)
            {
                current_tier = tracker.tier;
                fmt::print("\n  Tier #{:d}\n", print_tier);
                ++print_tier;
            }

            fmt::print("  {:s}\n", tracker.announce.sv());
        }

        /**
        ***
        **/

        if (auto const n_webseeds = metainfo.webseed_count(); n_webseeds > 0)
        {
            fmt::print("\nWEBSEEDS\n\n");

            for (size_t i = 0; i < n_webseeds; ++i)
            {
                fmt::print("  {:s}\n", metainfo.webseed(i));
            }
        }
    }

    /**
    ***  Files
    **/

    if (opts.print_files)
    {
        if (!opts.show_bytesize)
        {
            fmt::print("\nFILES\n\n");
        }

        auto filenames = std::vector<std::string>{};
        for (tr_file_index_t i = 0, n = metainfo.file_count(); i < n; ++i)
        {
            std::string filename;
            if (opts.show_bytesize)
            {
                filename = std::to_string(metainfo.file_size(i));
                filename += " ";
                filename += metainfo.file_subpath(i);
            }
            else
            {
                filename = "  ";
                filename += metainfo.file_subpath(i);
                filename += " (";
                filename += Storage{ metainfo.file_size(i), Storage::Units::Bytes }.to_string();
                filename += ')';
            }
            filenames.emplace_back(filename);
        }

        if (!opts.unsorted)
        {
            if (opts.show_bytesize)
            {
                std::sort(std::begin(filenames), std::end(filenames), compareSecondField);
            }
            else
            {
                std::sort(std::begin(filenames), std::end(filenames));
            }
        }

        for (auto const& filename : filenames)
        {
            fmt::print("{:s}\n", filename);
        }
    }
}

void doScrape(tr_torrent_metainfo const& metainfo)
{
    class Mediator final : public tr_web::Mediator
    {
        [[nodiscard]] time_t now() const override
        {
            return time(nullptr);
        }
    };

    auto mediator = Mediator{};
    auto web = tr_web::create(mediator);

    for (auto const& tracker : metainfo.announce_list())
    {
        if (std::empty(tracker.scrape))
        {
            continue;
        }

        // build the full scrape URL
        auto scrape_url = tr_urlbuf{ tracker.scrape.sv() };
        auto delimiter = tr_strv_contains(scrape_url, '?') ? '&' : '?';
        scrape_url.append(delimiter, "info_hash=");
        tr_urlPercentEncode(std::back_inserter(scrape_url), metainfo.info_hash());
        fmt::print("{:s} ... ", scrape_url);
        fflush(stdout);

        // execute the http scrape
        auto response = tr_web::FetchResponse{};
        auto response_mutex = std::mutex{};
        auto response_cv = std::condition_variable{};
        auto lock = std::unique_lock(response_mutex);
        web->fetch({ scrape_url,
                     [&response, &response_cv](tr_web::FetchResponse const& resp)
                     {
                         response = resp;
                         response_cv.notify_one();
                     },
                     nullptr,
                     TimeoutSecs });
        response_cv.wait(lock);

        // check the response code
        if (auto const code = response.status; code != 200 /*HTTP OK*/)
        {
            fmt::print("error: unexpected response {:d} '{:s}'\n", code, tr_webGetResponseStr(code));
            continue;
        }

        // print it out
        auto otop = tr_variant_serde::benc().inplace().parse(response.body);
        if (!!otop)
        {
            fmt::print("error parsing scrape response\n");
            continue;
        }
        auto& top = *otop;

        bool matched = false;
        if (tr_variant* files = nullptr; tr_variantDictFindDict(&top, TR_KEY_files, &files))
        {
            size_t child_pos = 0;
            tr_quark key;
            tr_variant* val;

            auto hashsv = std::string_view{ reinterpret_cast<char const*>(std::data(metainfo.info_hash())),
                                            std::size(metainfo.info_hash()) };

            while (tr_variantDictChild(files, child_pos, &key, &val))
            {
                if (hashsv == tr_quark_get_string_view(key))
                {
                    auto i = int64_t{};
                    auto const seeders = tr_variantDictFindInt(val, TR_KEY_complete, &i) ? int(i) : -1;
                    auto const leechers = tr_variantDictFindInt(val, TR_KEY_incomplete, &i) ? int(i) : -1;
                    fmt::print("{:d} seeders, {:d} leechers\n", seeders, leechers);
                    matched = true;
                }

                ++child_pos;
            }
        }

        if (!matched)
        {
            fmt::print("no match\n");
        }
    }
}

} // namespace

static
int do_tr_show(int argc, char* argv[])
{
    tr_lib_init();

    tr_locale_set_global("");

    tr_logSetQueueEnabled(false);
    tr_logSetLevel(TR_LOG_ERROR);

    auto opts = app_opts{};
    if (parseCommandLine(opts, argc, (char const* const*)argv) != 0)
    {
        return EXIT_FAILURE;
    }

    if (opts.show_version)
    {
        fmt::print(stderr, "{:s} {:s}\n", MyName, LONG_VERSION_STRING);
        return EXIT_SUCCESS;
    }

    /* make sure the user specified a filename */
    if (std::empty(opts.filename))
    {
        fmt::print(stderr, "ERROR: No torrent file specified.\n");
        tr_getopt_usage(MyName, Usage, std::data(options));
        fmt::print(stderr, "\n");
        return EXIT_FAILURE;
    }

    /* try to parse the torrent file */
    auto metainfo = tr_torrent_metainfo{};
    auto error = tr_error{};
    auto const parsed = metainfo.parse_torrent_file(opts.filename, nullptr, &error);
    if (error)
    {
        fmt::print(stderr, "Error parsing torrent file '{:s}': {:s} ({:d})\n", opts.filename, error.message(), error.code());
    }
    if (!parsed)
    {
        return EXIT_FAILURE;
    }

    if (opts.show_magnet)
    {
        fmt::print("{:s}", metainfo.magnet());
    }
    else
    {
        if (opts.print_header)
        {
            fmt::print("Name: {:s}\n", metainfo.name());
            fmt::print("File: {:s}\n", opts.filename);
            fmt::print("\n");
            fflush(stdout);
        }

        if (opts.scrape)
        {
            doScrape(metainfo);
        }
        else
        {
            showInfo(opts, metainfo);
        }
    }

    /* cleanup */
    putc('\n', stdout);
    return EXIT_SUCCESS;
}

struct tr_cmd tr_show = {
	.name = "show",
	.cmd = do_tr_show,
};
