// This file Copyright © 2012-2022 Mnemosyne LLC.
// It may be used under GPLv2(SPDX : GPL - 2.0), GPLv3(SPDX : GPL - 3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

#include <curl/curl.h>

#include <event2/buffer.h>

#include <libtransmission/transmission.h>

#include <libtransmission/error.h>
#include <libtransmission/log.h>
#include <libtransmission/torrent-metainfo.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/tr-macros.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>
#include <libtransmission/web-utils.h>

#include "units.h"

using namespace std::literals;

namespace
{

auto constexpr TimeoutSecs = long{ 30 };

char constexpr MyName[] = "transmission-show";
char constexpr Usage[] = "Usage: transmission-show [options] <.torrent file>";
char constexpr UserAgent[] = "transmission-show/" LONG_VERSION_STRING;

auto options = std::array<tr_option, 5>{
    { { 'm', "magnet", "Give a magnet link for the specified torrent", "m", false, nullptr },
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
};

int parseCommandLine(app_opts& opts, int argc, char const* const* argv)
{
    int c;
    char const* optarg;

    while ((c = tr_getopt(Usage, argc, argv, std::data(options), &optarg)) != TR_OPT_DONE)
    {
        switch (c)
        {
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

auto toString(time_t timestamp)
{
    if (timestamp == 0)
    {
        return std::string{ "Unknown" };
    }

    struct tm tm;
    tr_localtime_r(&timestamp, &tm);
    auto buf = std::array<char, 64>{};
    strftime(std::data(buf), std::size(buf), "%a %b %d %T %Y%n", &tm); /* ctime equiv */
    return std::string{ std::data(buf) };
}

void showInfo(app_opts const& opts, tr_torrent_metainfo const& metainfo)
{
    /**
    ***  General Info
    **/

    printf("GENERAL\n\n");
    printf("  Name: %s\n", metainfo.name().c_str());
    printf("  Hash: %" TR_PRIsv "\n", TR_PRIsv_ARG(metainfo.infoHashString()));
    printf("  Created by: %s\n", std::empty(metainfo.creator()) ? "Unknown" : metainfo.creator().c_str());
    printf("  Created on: %s\n", toString(metainfo.dateCreated()).c_str());

    if (!std::empty(metainfo.comment()))
    {
        printf("  Comment: %s\n", metainfo.comment().c_str());
    }

    if (!std::empty(metainfo.source()))
    {
        printf("  Source: %s\n", metainfo.source().c_str());
    }

    printf("  Piece Count: %" PRIu64 "\n", metainfo.pieceCount());
    printf("  Piece Size: %s\n", tr_formatter_mem_B(metainfo.pieceSize()).c_str());
    printf("  Total Size: %s\n", tr_formatter_size_B(metainfo.totalSize()).c_str());
    printf("  Privacy: %s\n", metainfo.isPrivate() ? "Private torrent" : "Public torrent");

    /**
    ***  Trackers
    **/

    printf("\nTRACKERS\n");
    auto current_tier = std::optional<tr_tracker_tier_t>{};
    auto print_tier = size_t{ 1 };
    for (auto const& tracker : metainfo.announceList())
    {
        if (!current_tier || current_tier != tracker.tier)
        {
            current_tier = tracker.tier;
            printf("\n  Tier #%zu\n", print_tier);
            ++print_tier;
        }

        printf("  %" TR_PRIsv "\n", TR_PRIsv_ARG(tracker.announce.full));
    }

    /**
    ***
    **/

    if (auto const n_webseeds = metainfo.webseedCount(); n_webseeds > 0)
    {
        printf("\nWEBSEEDS\n\n");

        for (size_t i = 0; i < n_webseeds; ++i)
        {
            printf("  %s\n", metainfo.webseed(i).c_str());
        }
    }

    /**
    ***  Files
    **/

    printf("\nFILES\n\n");

    auto filenames = std::vector<std::string>{};
    for (tr_file_index_t i = 0, n = metainfo.fileCount(); i < n; ++i)
    {
        std::string filename = metainfo.fileSubpath(i);
        filename += " (";
        filename += tr_formatter_size_B(metainfo.fileSize(i));
        filename += ')';
        filenames.emplace_back(filename);
    }

    if (!opts.unsorted)
    {
        std::sort(std::begin(filenames), std::end(filenames));
    }

    for (auto const& filename : filenames)
    {
        printf("  %s\n", filename.c_str());
    }
}

size_t writeFunc(void* ptr, size_t size, size_t nmemb, void* vbuf)
{
    auto* buf = static_cast<evbuffer*>(vbuf);
    size_t const byteCount = size * nmemb;
    evbuffer_add(buf, ptr, byteCount);
    return byteCount;
}

CURL* tr_curl_easy_init(struct evbuffer* writebuf)
{
    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, writebuf);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, tr_env_key_exists("TR_CURL_VERBOSE"));
    curl_easy_setopt(curl, CURLOPT_ENCODING, "");
    return curl;
}

void doScrape(tr_torrent_metainfo const& metainfo)
{
    auto* const buf = evbuffer_new();
    auto* const curl = tr_curl_easy_init(buf);

    for (auto const& tracker : metainfo.announceList())
    {
        if (std::empty(tracker.scrape_str))
        {
            continue;
        }

        // build the full scrape URL
        auto escaped = std::array<char, TR_SHA1_DIGEST_LEN * 3 + 1>{};
        tr_http_escape_sha1(std::data(escaped), metainfo.infoHash());
        auto const scrape = tracker.scrape.full;
        auto const url = tr_strvJoin(
            scrape,
            (tr_strvContains(scrape, '?') ? "&"sv : "?"sv),
            "info_hash="sv,
            std::data(escaped));

        printf("%" TR_PRIsv " ... ", TR_PRIsv_ARG(url));
        fflush(stdout);

        // execute the http scrape
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, TimeoutSecs);
        if (auto const res = curl_easy_perform(curl); res != CURLE_OK)
        {
            printf("error: %s\n", curl_easy_strerror(res));
            continue;
        }

        // check the response code
        long response;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
        if (response != 200 /*HTTP OK*/)
        {
            printf("error: unexpected response %ld \"%s\"\n", response, tr_webGetResponseStr(response));
            continue;
        }

        // print it out
        tr_variant top;
        auto* const begin = (char const*)evbuffer_pullup(buf, -1);
        auto sv = std::string_view{ begin, evbuffer_get_length(buf) };
        if (!tr_variantFromBuf(&top, TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE, sv))
        {
            printf("error parsing scrape response\n");
            continue;
        }

        bool matched = false;
        if (tr_variant* files = nullptr; tr_variantDictFindDict(&top, TR_KEY_files, &files))
        {
            size_t child_pos = 0;
            tr_quark key;
            tr_variant* val;

            auto hashsv = std::string_view{ reinterpret_cast<char const*>(std::data(metainfo.infoHash())),
                                            std::size(metainfo.infoHash()) };

            while (tr_variantDictChild(files, child_pos, &key, &val))
            {
                if (hashsv == tr_quark_get_string_view(key))
                {
                    auto i = int64_t{};
                    auto const seeders = tr_variantDictFindInt(val, TR_KEY_complete, &i) ? int(i) : -1;
                    auto const leechers = tr_variantDictFindInt(val, TR_KEY_incomplete, &i) ? int(i) : -1;
                    printf("%d seeders, %d leechers\n", seeders, leechers);
                    matched = true;
                }

                ++child_pos;
            }
        }

        tr_variantFree(&top);

        if (!matched)
        {
            printf("no match\n");
        }
    }

    curl_easy_cleanup(curl);
    evbuffer_free(buf);
}

} // namespace

int tr_main(int argc, char* argv[])
{
    tr_logSetLevel(TR_LOG_ERROR);
    tr_formatter_mem_init(MemK, MemKStr, MemMStr, MemGStr, MemTStr);
    tr_formatter_size_init(DiskK, DiskKStr, DiskMStr, DiskGStr, DiskTStr);
    tr_formatter_speed_init(SpeedK, SpeedKStr, SpeedMStr, SpeedGStr, SpeedTStr);

    auto opts = app_opts{};
    if (parseCommandLine(opts, argc, (char const* const*)argv) != 0)
    {
        return EXIT_FAILURE;
    }

    if (opts.show_version)
    {
        fprintf(stderr, "%s %s\n", MyName, LONG_VERSION_STRING);
        return EXIT_SUCCESS;
    }

    /* make sure the user specified a filename */
    if (std::empty(opts.filename))
    {
        fprintf(stderr, "ERROR: No .torrent file specified.\n");
        tr_getopt_usage(MyName, Usage, std::data(options));
        fprintf(stderr, "\n");
        return EXIT_FAILURE;
    }

    /* try to parse the .torrent file */
    auto metainfo = tr_torrent_metainfo{};
    tr_error* error = nullptr;
    auto const parsed = metainfo.parseTorrentFile(opts.filename, nullptr, &error);
    if (error != nullptr)
    {
        fprintf(
            stderr,
            "Error parsing .torrent file \"%" TR_PRIsv "\": %s (%d)\n",
            TR_PRIsv_ARG(opts.filename),
            error->message,
            error->code);
        tr_error_clear(&error);
    }
    if (!parsed)
    {
        return EXIT_FAILURE;
    }

    if (opts.show_magnet)
    {
        printf("%s", metainfo.magnet().c_str());
    }
    else
    {
        printf("Name: %s\n", metainfo.name().c_str());
        printf("File: %" TR_PRIsv "\n", TR_PRIsv_ARG(opts.filename));
        printf("\n");
        fflush(stdout);

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
