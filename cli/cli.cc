// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstdio> /* fprintf () */
#include <cstdlib> /* atoi () */
#include <string>
#include <string_view>
#include <thread>

#include <signal.h>

#include <fmt/core.h>

#include <libtransmission/transmission.h>

#include <libtransmission/error.h>
#include <libtransmission/file.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h> // _()
#include <libtransmission/variant.h>
#include <libtransmission/version.h>
#include <libtransmission/web-utils.h>
#include <libtransmission/web.h> // tr_sessionFetch()

using namespace std::chrono_literals;

/***
****
***/

static auto constexpr MemK = size_t{ 1024 };
static char constexpr MemKStr[] = "KiB";
static char constexpr MemMStr[] = "MiB";
static char constexpr MemGStr[] = "GiB";
static char constexpr MemTStr[] = "TiB";

static auto constexpr DiskK = size_t{ 1000 };
static char constexpr DiskKStr[] = "kB";
static char constexpr DiskMStr[] = "MB";
static char constexpr DiskGStr[] = "GB";
static char constexpr DiskTStr[] = "TB";

static auto constexpr SpeedK = size_t{ 1000 };
#define SPEED_K_STR "kB/s"
static char constexpr SpeedKStr[] = SPEED_K_STR;
static char constexpr SpeedMStr[] = "MB/s";
static char constexpr SpeedGStr[] = "GB/s";
static char constexpr SpeedTStr[] = "TB/s";

/***
****
***/

static auto constexpr LineWidth = int{ 80 };

static char constexpr MyConfigName[] = "transmission";
static char constexpr MyReadableName[] = "transmission-cli";
static char constexpr Usage
    [] = "A fast and easy BitTorrent client\n"
         "\n"
         "Usage: transmission-cli [options] <file|url|magnet>";

static bool showVersion = false;
static bool verify = false;
static sig_atomic_t gotsig = false;
static sig_atomic_t manualUpdate = false;

static char const* torrentPath = nullptr;

static auto constexpr Options = std::array<tr_option, 20>{
    { { 'b', "blocklist", "Enable peer blocklists", "b", false, nullptr },
      { 'B', "no-blocklist", "Disable peer blocklists", "B", false, nullptr },
      { 'd', "downlimit", "Set max download speed in " SPEED_K_STR, "d", true, "<speed>" },
      { 'D', "no-downlimit", "Don't limit the download speed", "D", false, nullptr },
      { 910, "encryption-required", "Encrypt all peer connections", "er", false, nullptr },
      { 911, "encryption-preferred", "Prefer encrypted peer connections", "ep", false, nullptr },
      { 912, "encryption-tolerated", "Prefer unencrypted peer connections", "et", false, nullptr },
      { 'f', "finish", "Run a script when the torrent finishes", "f", true, "<script>" },
      { 'g', "config-dir", "Where to find configuration files", "g", true, "<path>" },
      { 'm', "portmap", "Enable portmapping via NAT-PMP or UPnP", "m", false, nullptr },
      { 'M', "no-portmap", "Disable portmapping", "M", false, nullptr },
      { 'p', "port", "Port for incoming peers (Default: " TR_DEFAULT_PEER_PORT_STR ")", "p", true, "<port>" },
      { 't',
        "tos",
        "Peer socket DSCP / ToS setting (number, or a DSCP string, e.g. 'af11' or 'cs0', default=" TR_DEFAULT_PEER_SOCKET_TOS_STR
        ")",
        "t",
        true,
        "<dscp-or-tos>" },
      { 'u', "uplimit", "Set max upload speed in " SPEED_K_STR, "u", true, "<speed>" },
      { 'U', "no-uplimit", "Don't limit the upload speed", "U", false, nullptr },
      { 'v', "verify", "Verify the specified torrent", "v", false, nullptr },
      { 'V', "version", "Show version number and exit", "V", false, nullptr },
      { 'w', "download-dir", "Where to save downloaded data", "w", true, "<path>" },
      { 500, "sequential-download", "Download pieces sequentially", "seq", false, nullptr },

      { 0, nullptr, nullptr, nullptr, false, nullptr } }
};

static int parseCommandLine(tr_variant*, int argc, char const** argv);

static void sigHandler(int signal);

static std::string tr_strlratio(double ratio)
{
    if (static_cast<int>(ratio) == TR_RATIO_NA)
    {
        return _("None");
    }

    if (static_cast<int>(ratio) == TR_RATIO_INF)
    {
        return _("Inf");
    }

    if (ratio < 10.0)
    {
        return fmt::format(FMT_STRING("{:.2f}"), ratio);
    }

    if (ratio < 100.0)
    {
        return fmt::format(FMT_STRING("{:.1f}"), ratio);
    }

    return fmt::format(FMT_STRING("{:.0f}"), ratio);
}

static bool waitingOnWeb;

static void onTorrentFileDownloaded(tr_web::FetchResponse const& response)
{
    auto* ctor = static_cast<tr_ctor*>(response.user_data);
    tr_ctorSetMetainfo(ctor, std::data(response.body), std::size(response.body), nullptr);
    waitingOnWeb = false;
}

static std::string getStatusStr(tr_stat const* st)
{
    if (st->activity == TR_STATUS_CHECK_WAIT)
    {
        return "Waiting to verify local files";
    }

    if (st->activity == TR_STATUS_CHECK)
    {
        return fmt::format(
            FMT_STRING("Verifying local files ({:.2f}%, {:.2f}% valid)"),
            tr_truncd(100 * st->recheckProgress, 2),
            tr_truncd(100 * st->percentDone, 2));
    }

    if (st->activity == TR_STATUS_DOWNLOAD)
    {
        return fmt::format(
            FMT_STRING("Progress: {:.1f}%, dl from {:d} of {:d} peers ({:s}), ul to {:d} ({:s}) [{:s}]"),
            tr_truncd(100 * st->percentDone, 1),
            st->peersSendingToUs,
            st->peersConnected,
            tr_formatter_speed_KBps(st->pieceDownloadSpeed_KBps),
            st->peersGettingFromUs,
            tr_formatter_speed_KBps(st->pieceUploadSpeed_KBps),
            tr_strlratio(st->ratio));
    }

    if (st->activity == TR_STATUS_SEED)
    {
        return fmt::format(
            FMT_STRING("Seeding, uploading to {:d} of {:d} peer(s), {:s} [{:s}]"),
            st->peersGettingFromUs,
            st->peersConnected,
            tr_formatter_speed_KBps(st->pieceUploadSpeed_KBps),
            tr_strlratio(st->ratio));
    }

    return "";
}

static std::string getConfigDir(int argc, char const** argv)
{
    int c;
    char const* my_optarg;
    int const ind = tr_optind;

    while ((c = tr_getopt(Usage, argc, argv, std::data(Options), &my_optarg)) != TR_OPT_DONE)
    {
        if (c == 'g')
        {
            return my_optarg;
            break;
        }
    }

    tr_optind = ind;

    return tr_getDefaultConfigDir(MyConfigName);
}

int tr_main(int argc, char* argv[])
{
    auto const init_mgr = tr_lib_init();

    tr_locale_set_global("");

    tr_formatter_mem_init(MemK, MemKStr, MemMStr, MemGStr, MemTStr);
    tr_formatter_size_init(DiskK, DiskKStr, DiskMStr, DiskGStr, DiskTStr);
    tr_formatter_speed_init(SpeedK, SpeedKStr, SpeedMStr, SpeedGStr, SpeedTStr);

    printf("%s %s\n", MyReadableName, LONG_VERSION_STRING);

    /* user needs to pass in at least one argument */
    if (argc < 2)
    {
        tr_getopt_usage(MyReadableName, Usage, std::data(Options));
        return EXIT_FAILURE;
    }

    /* load the defaults from config file + libtransmission defaults */
    auto const config_dir = getConfigDir(argc, (char const**)argv);
    auto settings = tr_variant::make_map();
    tr_sessionLoadSettings(&settings, config_dir.c_str(), MyConfigName);

    /* the command line overrides defaults */
    if (parseCommandLine(&settings, argc, (char const**)argv) != 0)
    {
        return EXIT_FAILURE;
    }

    if (showVersion)
    {
        return EXIT_SUCCESS;
    }

    /* Check the options for validity */
    if (torrentPath == nullptr)
    {
        fprintf(stderr, "No torrent specified!\n");
        return EXIT_FAILURE;
    }

    if (auto sv = std::string_view{}; tr_variantDictFindStrView(&settings, TR_KEY_download_dir, &sv))
    {
        auto const sz_download_dir = std::string{ sv };

        if (!tr_sys_path_exists(sz_download_dir))
        {
            tr_error* error = nullptr;

            if (!tr_sys_dir_create(sz_download_dir, TR_SYS_DIR_CREATE_PARENTS, 0700, &error))
            {
                fprintf(stderr, "Unable to create download directory \"%s\": %s\n", sz_download_dir.c_str(), error->message);
                tr_error_free(error);
                return EXIT_FAILURE;
            }
        }
    }

    auto* const h = tr_sessionInit(config_dir.c_str(), false, &settings);
    auto* const ctor = tr_ctorNew(h);

    tr_ctorSetPaused(ctor, TR_FORCE, false);

    if (tr_sys_path_exists(torrentPath) ? tr_ctorSetMetainfoFromFile(ctor, torrentPath, nullptr) :
                                          tr_ctorSetMetainfoFromMagnetLink(ctor, torrentPath, nullptr))
    {
        // all good
    }
    else if (tr_urlIsValid(torrentPath))
    {
        // fetch it
        tr_sessionFetch(h, { torrentPath, onTorrentFileDownloaded, ctor });
        waitingOnWeb = true;
        while (waitingOnWeb)
        {
            std::this_thread::sleep_for(1s);
        }
    }
    else
    {
        fprintf(stderr, "ERROR: Unrecognized torrent \"%s\".\n", torrentPath);
        fprintf(stderr, " * If you're trying to create a torrent, use transmission-create.\n");
        fprintf(stderr, " * If you're trying to see a torrent's info, use transmission-show.\n");
        tr_sessionClose(h);
        return EXIT_FAILURE;
    }

    tr_torrent* tor = tr_torrentNew(ctor, nullptr);
    tr_ctorFree(ctor);
    if (tor == nullptr)
    {
        fprintf(stderr, "Failed opening torrent file `%s'\n", torrentPath);
        tr_sessionClose(h);
        return EXIT_FAILURE;
    }

    signal(SIGINT, sigHandler);
#ifndef _WIN32
    signal(SIGHUP, sigHandler);
#endif
    tr_torrentStart(tor);

    if (verify)
    {
        verify = false;
        tr_torrentVerify(tor);
    }

    for (;;)
    {
        static auto constexpr messageName = std::array<char const*, 4>{
            nullptr,
            "Tracker gave a warning:",
            "Tracker gave an error:",
            "Error:",
        };

        std::this_thread::sleep_for(200ms);

        if (gotsig)
        {
            gotsig = false;
            printf("\nStopping torrent...\n");
            tr_torrentStop(tor);
        }

        if (manualUpdate)
        {
            manualUpdate = false;

            if (!tr_torrentCanManualUpdate(tor))
            {
                fprintf(stderr, "\nReceived SIGHUP, but can't send a manual update now\n");
            }
            else
            {
                fprintf(stderr, "\nReceived SIGHUP: manual update scheduled\n");
                tr_torrentManualUpdate(tor);
            }
        }

        auto const* const st = tr_torrentStat(tor);
        if (st->activity == TR_STATUS_STOPPED)
        {
            break;
        }

        auto const status_str = getStatusStr(st);
        printf("\r%-*s", TR_ARG_TUPLE(LineWidth, status_str.c_str()));

        if (messageName[st->error])
        {
            fprintf(stderr, "\n%s: %s\n", messageName[st->error], st->errorString);
        }
    }

    tr_sessionSaveSettings(h, config_dir.c_str(), &settings);

    printf("\n");
    tr_sessionClose(h);
    return EXIT_SUCCESS;
}

/***
****
****
***/

static int parseCommandLine(tr_variant* d, int argc, char const** argv)
{
    int c;
    char const* my_optarg;

    while ((c = tr_getopt(Usage, argc, argv, std::data(Options), &my_optarg)) != TR_OPT_DONE)
    {
        switch (c)
        {
        case 'b':
            tr_variantDictAddBool(d, TR_KEY_blocklist_enabled, true);
            break;

        case 'B':
            tr_variantDictAddBool(d, TR_KEY_blocklist_enabled, false);
            break;

        case 'd':
            tr_variantDictAddInt(d, TR_KEY_speed_limit_down, atoi(my_optarg));
            tr_variantDictAddBool(d, TR_KEY_speed_limit_down_enabled, true);
            break;

        case 'D':
            tr_variantDictAddBool(d, TR_KEY_speed_limit_down_enabled, false);
            break;

        case 'f':
            tr_variantDictAddStr(d, TR_KEY_script_torrent_done_filename, my_optarg);
            tr_variantDictAddBool(d, TR_KEY_script_torrent_done_enabled, true);
            break;

        case 'g': /* handled above */
            break;

        case 'm':
            tr_variantDictAddBool(d, TR_KEY_port_forwarding_enabled, true);
            break;

        case 'M':
            tr_variantDictAddBool(d, TR_KEY_port_forwarding_enabled, false);
            break;

        case 'p':
            tr_variantDictAddInt(d, TR_KEY_peer_port, atoi(my_optarg));
            break;

        case 't':
            tr_variantDictAddStr(d, TR_KEY_peer_socket_tos, my_optarg);
            break;

        case 'u':
            tr_variantDictAddInt(d, TR_KEY_speed_limit_up, atoi(my_optarg));
            tr_variantDictAddBool(d, TR_KEY_speed_limit_up_enabled, true);
            break;

        case 'U':
            tr_variantDictAddBool(d, TR_KEY_speed_limit_up_enabled, false);
            break;

        case 'v':
            verify = true;
            break;

        case 'V':
            showVersion = true;
            break;

        case 'w':
            tr_variantDictAddStr(d, TR_KEY_download_dir, my_optarg);
            break;

        case 910:
            tr_variantDictAddInt(d, TR_KEY_encryption, TR_ENCRYPTION_REQUIRED);
            break;

        case 911:
            tr_variantDictAddInt(d, TR_KEY_encryption, TR_ENCRYPTION_PREFERRED);
            break;

        case 912:
            tr_variantDictAddInt(d, TR_KEY_encryption, TR_CLEAR_PREFERRED);
            break;

        case 500:
            tr_variantDictAddBool(d, TR_KEY_sequentialDownload, true);
            break;

        case TR_OPT_UNK:
            if (torrentPath == nullptr)
            {
                torrentPath = my_optarg;
            }

            break;

        default:
            return 1;
        }
    }

    return 0;
}

static void sigHandler(int signal)
{
    switch (signal)
    {
    case SIGINT:
        gotsig = true;
        break;

#ifndef _WIN32

    case SIGHUP:
        manualUpdate = true;
        break;

#endif

    default:
        break;
    }
}
