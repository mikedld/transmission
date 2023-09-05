// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype> /* isspace */
#include <cerrno>
#include <cmath> // floor
#include <cstdint> // int64_t
#include <cstdio>
#include <cstdlib>
#include <cstring> /* strcmp */
#include <ctime>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <curl/curl.h>

#include <event2/buffer.h>

#include <fmt/chrono.h>
#include <fmt/core.h>

#include <libtransmission/transmission.h>
#include <libtransmission/crypto-utils.h>
#include <libtransmission/file.h>
#include <libtransmission/log.h>
#include <libtransmission/quark.h>
#include <libtransmission/rpcimpl.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>

using namespace std::literals;

#define SPEED_K_STR "kB/s"
#define MEM_M_STR "MiB"

static auto constexpr DefaultPort = int{ TR_DEFAULT_RPC_PORT };
static char constexpr DefaultHost[] = "localhost";
static char constexpr DefaultUrl[] = TR_DEFAULT_RPC_URL_STR "rpc/";

static char constexpr MyName[] = "transmission-remote";
static char constexpr Usage[] = "transmission-remote " LONG_VERSION_STRING
                                "\n"
                                "A fast and easy BitTorrent client\n"
                                "https://transmissionbt.com/\n"
                                "\n"
                                "Usage: transmission-remote [host] [options]\n"
                                "       transmission-remote [port] [options]\n"
                                "       transmission-remote [host:port] [options]\n"
                                "       transmission-remote [http(s?)://host:port/transmission/] [options]\n"
                                "\n"
                                "See the man page for detailed explanations and many examples.";

static auto constexpr Arguments = TR_KEY_arguments;

static auto constexpr MemK = size_t{ 1024 };
static char constexpr MemKStr[] = "KiB";
static char constexpr MemMStr[] = MEM_M_STR;
static char constexpr MemGStr[] = "GiB";
static char constexpr MemTStr[] = "TiB";

static auto constexpr DiskK = size_t{ 1000 };
static char constexpr DiskKStr[] = "kB";
static char constexpr DiskMStr[] = "MB";
static char constexpr DiskGStr[] = "GB";
static char constexpr DiskTStr[] = "TB";

static auto constexpr SpeedK = size_t{ 1000 };
static auto constexpr SpeedKStr = SPEED_K_STR;
static char constexpr SpeedMStr[] = "MB/s";
static char constexpr SpeedGStr[] = "GB/s";
static char constexpr SpeedTStr[] = "TB/s";

struct Config
{
    std::string auth;
    std::string filter;
    std::string netrc;
    std::string session_id;
    std::string torrent_ids;
    std::string unix_socket_path;

    bool debug = false;
    bool json = false;
    bool use_ssl = false;
};

/***
****
****  Display Utilities
****
***/

static std::string etaToString(int64_t eta)
{
    if (eta < 0)
    {
        return "Unknown";
    }

    if (eta < 60)
    {
        return fmt::format(FMT_STRING("{:d} sec"), eta);
    }

    if (eta < (60 * 60))
    {
        return fmt::format(FMT_STRING("{:d} min"), eta / 60);
    }

    if (eta < (60 * 60 * 24))
    {
        return fmt::format(FMT_STRING("{:d} hrs"), eta / (60 * 60));
    }

    if (eta < (60 * 60 * 24 * 30))
    {
        return fmt::format(FMT_STRING("{:d} days"), eta / (60 * 60 * 24));
    }

    if (eta < (60 * 60 * 24 * 30 * 12))
    {
        return fmt::format(FMT_STRING("{:d} months"), eta / (60 * 60 * 24 * 30));
    }

    if (eta < (60 * 60 * 24 * 365 * 1000LL)) // up to 999 years
    {
        return fmt::format(FMT_STRING("{:d} years"), eta / (60 * 60 * 24 * 365));
    }

    return "∞";
}

static std::string tr_strltime(time_t seconds)
{
    if (seconds < 0)
    {
        seconds = 0;
    }

    auto const total_seconds = seconds;
    auto const days = seconds / 86400;
    auto const hours = (seconds % 86400) / 3600;
    auto const minutes = (seconds % 3600) / 60;
    seconds = (seconds % 3600) % 60;

    auto tmpstr = std::string{};

    auto const hstr = fmt::format(FMT_STRING("{:d} {:s}"), hours, tr_ngettext("hour", "hours", hours));
    auto const mstr = fmt::format(FMT_STRING("{:d} {:s}"), minutes, tr_ngettext("minute", "minutes", minutes));
    auto const sstr = fmt::format(FMT_STRING("{:d} {:s}"), seconds, tr_ngettext("seconds", "seconds", seconds));

    if (days > 0)
    {
        auto const dstr = fmt::format(FMT_STRING("{:d} {:s}"), days, tr_ngettext("day", "days", days));
        tmpstr = days >= 4 || hours == 0 ? dstr : fmt::format(FMT_STRING("{:s}, {:s}"), dstr, hstr);
    }
    else if (hours > 0)
    {
        tmpstr = hours >= 4 || minutes == 0 ? hstr : fmt::format(FMT_STRING("{:s}, {:s}"), hstr, mstr);
    }
    else if (minutes > 0)
    {
        tmpstr = minutes >= 4 || seconds == 0 ? mstr : fmt::format(FMT_STRING("{:s}, {:s}"), mstr, sstr);
    }
    else
    {
        tmpstr = sstr;
    }

    auto const totstr = fmt::format(FMT_STRING("{:d} {:s}"), total_seconds, tr_ngettext("seconds", "seconds", total_seconds));
    return fmt::format(FMT_STRING("{:s} ({:s})"), tmpstr, totstr);
}

static std::string strlpercent(double x)
{
    return tr_strpercent(x);
}

static std::string strlratio2(double ratio)
{
    return tr_strratio(ratio, "Inf");
}

static std::string strlratio(int64_t numerator, int64_t denominator)
{
    return strlratio2(tr_getRatio(numerator, denominator));
}

static std::string strlmem(int64_t bytes)
{
    return bytes == 0 ? "None"s : tr_formatter_mem_B(bytes);
}

static std::string strlsize(int64_t bytes)
{
    if (bytes < 0)
    {
        return "Unknown"s;
    }

    if (bytes == 0)
    {
        return "None"s;
    }

    return tr_formatter_size_B(bytes);
}

enum
{
    TAG_SESSION,
    TAG_STATS,
    TAG_DETAILS,
    TAG_FILES,
    TAG_FILTER,
    TAG_GROUPS,
    TAG_LIST,
    TAG_PEERS,
    TAG_PIECES,
    TAG_PORTTEST,
    TAG_TORRENT_ADD,
    TAG_TRACKERS
};

/***
****
****  Command-Line Arguments
****
***/

static auto constexpr Options = std::array<tr_option, 98>{
    { { 'a', "add", "Add torrent files by filename or URL", "a", false, nullptr },
      { 970, "alt-speed", "Use the alternate Limits", "as", false, nullptr },
      { 971, "no-alt-speed", "Don't use the alternate Limits", "AS", false, nullptr },
      { 972, "alt-speed-downlimit", "max alternate download speed (in " SPEED_K_STR ")", "asd", true, "<speed>" },
      { 973, "alt-speed-uplimit", "max alternate upload speed (in " SPEED_K_STR ")", "asu", true, "<speed>" },
      { 974, "alt-speed-scheduler", "Use the scheduled on/off times", "asc", false, nullptr },
      { 975, "no-alt-speed-scheduler", "Don't use the scheduled on/off times", "ASC", false, nullptr },
      { 976, "alt-speed-time-begin", "Time to start using the alt speed limits (in hhmm)", nullptr, true, "<time>" },
      { 977, "alt-speed-time-end", "Time to stop using the alt speed limits (in hhmm)", nullptr, true, "<time>" },
      { 978, "alt-speed-days", "Numbers for any/all days of the week - eg. \"1-7\"", nullptr, true, "<days>" },
      { 963, "blocklist-update", "Blocklist update", nullptr, false, nullptr },
      { 'c', "incomplete-dir", "Where to store new torrents until they're complete", "c", true, "<dir>" },
      { 'C', "no-incomplete-dir", "Don't store incomplete torrents in a different location", "C", false, nullptr },
      { 'b', "debug", "Print debugging information", "b", false, nullptr },
      { 730, "bandwidth-group", "Set the current torrents' bandwidth group", "bwg", true, "<group>" },
      { 731, "no-bandwidth-group", "Reset the current torrents' bandwidth group", "nwg", false, nullptr },
      { 732, "list-groups", "Show bandwidth groups with their parameters", "lg", false, nullptr },
      { 'd',
        "downlimit",
        "Set the max download speed in " SPEED_K_STR " for the current torrent(s) or globally",
        "d",
        true,
        "<speed>" },
      { 'D', "no-downlimit", "Disable max download speed for the current torrent(s) or globally", "D", false, nullptr },
      { 'e', "cache", "Set the maximum size of the session's memory cache (in " MEM_M_STR ")", "e", true, "<size>" },
      { 910, "encryption-required", "Encrypt all peer connections", "er", false, nullptr },
      { 911, "encryption-preferred", "Prefer encrypted peer connections", "ep", false, nullptr },
      { 912, "encryption-tolerated", "Prefer unencrypted peer connections", "et", false, nullptr },
      { 850, "exit", "Tell the transmission session to shut down", nullptr, false, nullptr },
      { 940, "files", "List the current torrent(s)' files", "f", false, nullptr },
      { 'F', "filter", "Filter the current torrent(s)", "F", true, "criterion" },
      { 'g', "get", "Mark files for download", "g", true, "<files>" },
      { 'G', "no-get", "Mark files for not downloading", "G", true, "<files>" },
      { 'i', "info", "Show the current torrent(s)' details", "i", false, nullptr },
      { 944, "print-ids", "Print the current torrent(s)' ids", "ids", false, nullptr },
      { 940, "info-files", "List the current torrent(s)' files", "if", false, nullptr },
      { 941, "info-peers", "List the current torrent(s)' peers", "ip", false, nullptr },
      { 942, "info-pieces", "List the current torrent(s)' pieces", "ic", false, nullptr },
      { 943, "info-trackers", "List the current torrent(s)' trackers", "it", false, nullptr },
      { 'j', "json", "Return RPC response as a JSON string", "j", false, nullptr },
      { 920, "session-info", "Show the session's details", "si", false, nullptr },
      { 921, "session-stats", "Show the session's statistics", "st", false, nullptr },
      { 'l', "list", "List all torrents", "l", false, nullptr },
      { 'L', "labels", "Set the current torrents' labels", "L", true, "<label[,label...]>" },
      { 960, "move", "Move current torrent's data to a new folder", nullptr, true, "<path>" },
      { 968, "unix-socket", "Use a Unix domain socket", nullptr, true, "<path>" },
      { 961, "find", "Tell Transmission where to find a torrent's data", nullptr, true, "<path>" },
      { 964, "rename", "Rename torrents root folder or a file", nullptr, true, "<name>" },
      { 965, "path", "Provide path for rename functions", nullptr, true, "<path>" },
      { 'm', "portmap", "Enable portmapping via NAT-PMP or UPnP", "m", false, nullptr },
      { 'M', "no-portmap", "Disable portmapping", "M", false, nullptr },
      { 'n', "auth", "Set username and password", "n", true, "<user:pw>" },
      { 810, "authenv", "Set authentication info from the TR_AUTH environment variable (user:pw)", "ne", false, nullptr },
      { 'N', "netrc", "Set authentication info from a .netrc file", "N", true, "<file>" },
      { 820, "ssl", "Use SSL when talking to daemon", nullptr, false, nullptr },
      { 'o', "dht", "Enable distributed hash tables (DHT)", "o", false, nullptr },
      { 'O', "no-dht", "Disable distributed hash tables (DHT)", "O", false, nullptr },
      { 'p', "port", "Port for incoming peers (Default: " TR_DEFAULT_PEER_PORT_STR ")", "p", true, "<port>" },
      { 962, "port-test", "Port testing", "pt", false, nullptr },
      { 'P', "random-port", "Random port for incoming peers", "P", false, nullptr },
      { 900, "priority-high", "Try to download these file(s) first", "ph", true, "<files>" },
      { 901, "priority-normal", "Try to download these file(s) normally", "pn", true, "<files>" },
      { 902, "priority-low", "Try to download these file(s) last", "pl", true, "<files>" },
      { 700, "bandwidth-high", "Give this torrent first chance at available bandwidth", "Bh", false, nullptr },
      { 701, "bandwidth-normal", "Give this torrent bandwidth left over by high priority torrents", "Bn", false, nullptr },
      { 702,
        "bandwidth-low",
        "Give this torrent bandwidth left over by high and normal priority torrents",
        "Bl",
        false,
        nullptr },
      { 600, "reannounce", "Reannounce the current torrent(s)", nullptr, false, nullptr },
      { 'r', "remove", "Remove the current torrent(s)", "r", false, nullptr },
      { 930, "peers", "Set the maximum number of peers for the current torrent(s) or globally", "pr", true, "<max>" },
      { 840, "remove-and-delete", "Remove the current torrent(s) and delete local data", "rad", false, nullptr },
      { 800, "torrent-done-script", "A script to run when a torrent finishes downloading", nullptr, true, "<file>" },
      { 801, "no-torrent-done-script", "Don't run the done-downloading script", nullptr, false, nullptr },
      { 802, "torrent-done-seeding-script", "A script to run when a torrent finishes seeding", nullptr, true, "<file>" },
      { 803, "no-torrent-done-seeding-script", "Don't run the done-seeding script", nullptr, false, nullptr },
      { 950, "seedratio", "Let the current torrent(s) seed until a specific ratio", "sr", true, "ratio" },
      { 951, "seedratio-default", "Let the current torrent(s) use the global seedratio settings", "srd", false, nullptr },
      { 952, "no-seedratio", "Let the current torrent(s) seed regardless of ratio", "SR", false, nullptr },
      { 953,
        "global-seedratio",
        "All torrents, unless overridden by a per-torrent setting, should seed until a specific ratio",
        "gsr",
        true,
        "ratio" },
      { 954,
        "no-global-seedratio",
        "All torrents, unless overridden by a per-torrent setting, should seed regardless of ratio",
        "GSR",
        false,
        nullptr },
      { 710, "tracker-add", "Add a tracker to a torrent", "td", true, "<tracker>" },
      { 712, "tracker-remove", "Remove a tracker from a torrent", "tr", true, "<trackerId>" },
      { 's', "start", "Start the current torrent(s)", "s", false, nullptr },
      { 'S', "stop", "Stop the current torrent(s)", "S", false, nullptr },
      { 't', "torrent", "Set the current torrent(s)", "t", true, "<torrent>" },
      { 990, "start-paused", "Start added torrents paused", nullptr, false, nullptr },
      { 991, "no-start-paused", "Start added torrents unpaused", nullptr, false, nullptr },
      { 992, "trash-torrent", "Delete torrents after adding", nullptr, false, nullptr },
      { 993, "no-trash-torrent", "Do not delete torrents after adding", nullptr, false, nullptr },
      { 984, "honor-session", "Make the current torrent(s) honor the session limits", "hl", false, nullptr },
      { 985, "no-honor-session", "Make the current torrent(s) not honor the session limits", "HL", false, nullptr },
      { 'u',
        "uplimit",
        "Set the max upload speed in " SPEED_K_STR " for the current torrent(s) or globally",
        "u",
        true,
        "<speed>" },
      { 'U', "no-uplimit", "Disable max upload speed for the current torrent(s) or globally", "U", false, nullptr },
      { 830, "utp", "Enable µTP for peer connections", nullptr, false, nullptr },
      { 831, "no-utp", "Disable µTP for peer connections", nullptr, false, nullptr },
      { 'v', "verify", "Verify the current torrent(s)", "v", false, nullptr },
      { 'V', "version", "Show version number and exit", "V", false, nullptr },
      { 'w',
        "download-dir",
        "When used in conjunction with --add, set the new torrent's download folder. "
        "Otherwise, set the default download folder",
        "w",
        true,
        "<path>" },
      { 'x', "pex", "Enable peer exchange (PEX)", "x", false, nullptr },
      { 'X', "no-pex", "Disable peer exchange (PEX)", "X", false, nullptr },
      { 'y', "lpd", "Enable local peer discovery (LPD)", "y", false, nullptr },
      { 'Y', "no-lpd", "Disable local peer discovery (LPD)", "Y", false, nullptr },
      { 941, "peer-info", "List the current torrent(s)' peers", "pi", false, nullptr },
      { 0, nullptr, nullptr, nullptr, false, nullptr } }
};

static void showUsage(void)
{
    tr_getopt_usage(MyName, Usage, std::data(Options));
}

static long numarg(char const* arg)
{
    char* end = nullptr;
    long const num = strtol(arg, &end, 10);

    if (*end != '\0')
    {
        fmt::print(stderr, FMT_STRING("Not a number: '{:s}'\n"), arg);
        showUsage();
        exit(EXIT_FAILURE);
    }

    return num;
}

enum
{
    MODE_TORRENT_START = (1 << 0),
    MODE_TORRENT_STOP = (1 << 1),
    MODE_TORRENT_VERIFY = (1 << 2),
    MODE_TORRENT_REANNOUNCE = (1 << 3),
    MODE_TORRENT_SET = (1 << 4),
    MODE_TORRENT_GET = (1 << 5),
    MODE_TORRENT_ADD = (1 << 6),
    MODE_TORRENT_REMOVE = (1 << 7),
    MODE_TORRENT_SET_LOCATION = (1 << 8),
    MODE_SESSION_SET = (1 << 9),
    MODE_SESSION_GET = (1 << 10),
    MODE_SESSION_STATS = (1 << 11),
    MODE_SESSION_CLOSE = (1 << 12),
    MODE_BLOCKLIST_UPDATE = (1 << 13),
    MODE_PORT_TEST = (1 << 14),
    MODE_GROUP_GET = (1 << 15)
};

static int getOptMode(int val)
{
    switch (val)
    {
    case TR_OPT_ERR:
    case TR_OPT_UNK:
    case 'a': /* add torrent */
    case 'b': /* debug */
    case 'n': /* auth */
    case 968: /* Unix domain socket */
    case 'j': /* JSON */
    case 810: /* authenv */
    case 'N': /* netrc */
    case 820: /* UseSSL */
    case 't': /* set current torrent */
    case 'V': /* show version number */
    case 944: /* print selected torrents' ids */
        return 0;

    case 'c': /* incomplete-dir */
    case 'C': /* no-incomplete-dir */
    case 'e': /* cache */
    case 'm': /* portmap */
    case 'M': /* "no-portmap */
    case 'o': /* dht */
    case 'O': /* no-dht */
    case 'p': /* incoming peer port */
    case 'P': /* random incoming peer port */
    case 'x': /* pex */
    case 'X': /* no-pex */
    case 'y': /* lpd */
    case 'Y': /* no-lpd */
    case 800: /* torrent-done-script */
    case 801: /* no-torrent-done-script */
    case 802: /* torrent-done-seeding-script */
    case 803: /* no-torrent-done-seeding-script */
    case 830: /* utp */
    case 831: /* no-utp */
    case 970: /* alt-speed */
    case 971: /* no-alt-speed */
    case 972: /* alt-speed-downlimit */
    case 973: /* alt-speed-uplimit */
    case 974: /* alt-speed-scheduler */
    case 975: /* no-alt-speed-scheduler */
    case 976: /* alt-speed-time-begin */
    case 977: /* alt-speed-time-end */
    case 978: /* alt-speed-days */
    case 910: /* encryption-required */
    case 911: /* encryption-preferred */
    case 912: /* encryption-tolerated */
    case 953: /* global-seedratio */
    case 954: /* no-global-seedratio */
    case 990: /* start-paused */
    case 991: /* no-start-paused */
    case 992: /* trash-torrent */
    case 993: /* no-trash-torrent */
        return MODE_SESSION_SET;

    case 712: /* tracker-remove */
    case 950: /* seedratio */
    case 951: /* seedratio-default */
    case 952: /* no-seedratio */
    case 984: /* honor-session */
    case 985: /* no-honor-session */
        return MODE_TORRENT_SET;

    case 920: /* session-info */
        return MODE_SESSION_GET;

    case 'g': /* get */
    case 'G': /* no-get */
    case 'L': /* labels */
    case 700: /* torrent priority-high */
    case 701: /* torrent priority-normal */
    case 702: /* torrent priority-low */
    case 710: /* tracker-add */
    case 900: /* file priority-high */
    case 901: /* file priority-normal */
    case 902: /* file priority-low */
    case 730: /* set bandwidth group */
    case 731: /* reset bandwidth group */
        return MODE_TORRENT_SET | MODE_TORRENT_ADD;

    case 961: /* find */
        return MODE_TORRENT_SET_LOCATION | MODE_TORRENT_ADD;

    case 'i': /* info */
    case 'l': /* list all torrents */
    case 940: /* info-files */
    case 941: /* info-peer */
    case 942: /* info-pieces */
    case 943: /* info-tracker */
    case 'F': /* filter torrents */
        return MODE_TORRENT_GET;

    case 'd': /* download speed limit */
    case 'D': /* no download speed limit */
    case 'u': /* upload speed limit */
    case 'U': /* no upload speed limit */
    case 930: /* peers */
        return MODE_SESSION_SET | MODE_TORRENT_SET;

    case 's': /* start */
        return MODE_TORRENT_START | MODE_TORRENT_ADD;

    case 'S': /* stop */
        return MODE_TORRENT_STOP | MODE_TORRENT_ADD;

    case 'w': /* download-dir */
        return MODE_SESSION_SET | MODE_TORRENT_ADD;

    case 850: /* session-close */
        return MODE_SESSION_CLOSE;

    case 963: /* blocklist-update */
        return MODE_BLOCKLIST_UPDATE;

    case 921: /* session-stats */
        return MODE_SESSION_STATS;

    case 'v': /* verify */
        return MODE_TORRENT_VERIFY;

    case 600: /* reannounce */
        return MODE_TORRENT_REANNOUNCE;

    case 962: /* port-test */
        return MODE_PORT_TEST;

    case 'r': /* remove */
    case 840: /* remove and delete */
        return MODE_TORRENT_REMOVE;

    case 960: /* move */
        return MODE_TORRENT_SET_LOCATION;

    case 964: /* rename */
        return MODE_TORRENT_SET_LOCATION | MODE_TORRENT_SET;

    case 965: /* path */
        return MODE_TORRENT_SET_LOCATION | MODE_TORRENT_SET;

    case 732: /* List groups */
        return MODE_GROUP_GET;

    default:
        fmt::print(stderr, FMT_STRING("unrecognized argument {:d}\n"), val);
        assert("unrecognized argument" && 0);
        return 0;
    }
}

static std::string getEncodedMetainfo(char const* filename)
{
    if (auto contents = std::vector<char>{}; tr_sys_path_exists(filename) && tr_file_read(filename, contents))
    {
        return tr_base64_encode({ std::data(contents), std::size(contents) });
    }

    return {};
}

static void addIdArg(tr_variant* args, std::string_view id_str, std::string_view fallback = "")
{
    if (std::empty(id_str))
    {
        id_str = fallback;
    }

    if (std::empty(id_str))
    {
        fmt::print(stderr, "No torrent specified!  Please use the -t option first.\n");
        id_str = "-1"sv; /* no torrent will have this ID, so will act as a no-op */
    }

    static auto constexpr IdActive = "active"sv;
    static auto constexpr IdAll = "all"sv;

    if (IdActive == id_str)
    {
        tr_variantDictAddStrView(args, TR_KEY_ids, "recently-active"sv);
    }
    else if (IdAll != id_str)
    {
        bool const is_list = id_str.find_first_of(",-") != std::string_view::npos;
        bool is_num = true;

        for (auto const& ch : id_str)
        {
            is_num = is_num && isdigit(ch);
        }

        if (is_num || is_list)
        {
            tr_rpc_parse_list_str(tr_variantDictAdd(args, TR_KEY_ids), id_str);
        }
        else
        {
            tr_variantDictAddStr(args, TR_KEY_ids, id_str); /* it's a torrent sha hash */
        }
    }
}

static void addIdArg(tr_variant* args, Config const& config, std::string_view fallback = "")
{
    return addIdArg(args, config.torrent_ids, fallback);
}

static void addTime(tr_variant* args, tr_quark const key, char const* arg)
{
    int time = 0;
    bool success = false;

    if (arg != nullptr && strlen(arg) == 4)
    {
        char const hh[3] = { arg[0], arg[1], '\0' };
        char const mm[3] = { arg[2], arg[3], '\0' };
        int const hour = atoi(hh);
        int const min = atoi(mm);

        if (0 <= hour && hour < 24 && 0 <= min && min < 60)
        {
            time = min + (hour * 60);
            success = true;
        }
    }

    if (success)
    {
        tr_variantDictAddInt(args, key, time);
    }
    else
    {
        fmt::print(stderr, "Please specify the time of day in 'hhmm' format.\n");
    }
}

static void addDays(tr_variant* args, tr_quark const key, char const* arg)
{
    int days = 0;

    if (arg != nullptr)
    {
        for (int& day : tr_num_parse_range(arg))
        {
            if (day < 0 || day > 7)
            {
                continue;
            }

            if (day == 7)
            {
                day = 0;
            }

            days |= 1 << day;
        }
    }

    if (days != 0)
    {
        tr_variantDictAddInt(args, key, days);
    }
    else
    {
        fmt::print(stderr, "Please specify the days of the week in '1-3,4,7' format.\n");
    }
}

static void addLabels(tr_variant* args, std::string_view comma_delimited_labels)
{
    tr_variant* labels;
    if (!tr_variantDictFindList(args, TR_KEY_labels, &labels))
    {
        labels = tr_variantDictAddList(args, TR_KEY_labels, 10);
    }

    auto label = std::string_view{};
    while (tr_strv_sep(&comma_delimited_labels, &label, ','))
    {
        tr_variantListAddStr(labels, label);
    }
}

static void setGroup(tr_variant* args, std::string_view group)
{
    tr_variantDictAddStrView(args, TR_KEY_group, group);
}

static void addFiles(tr_variant* args, tr_quark const key, char const* arg)
{
    tr_variant* files = tr_variantDictAddList(args, key, 100);

    if (tr_str_is_empty(arg))
    {
        fmt::print(stderr, "No files specified!\n");
        arg = "-1"; /* no file will have this index, so should be a no-op */
    }

    if (strcmp(arg, "all") != 0)
    {
        for (auto const& idx : tr_num_parse_range(arg))
        {
            tr_variantListAddInt(files, idx);
        }
    }
}

// clang-format off

static auto constexpr FilesKeys = std::array<tr_quark, 4>{
    TR_KEY_files,
    TR_KEY_name,
    TR_KEY_priorities,
    TR_KEY_wanted,
};

static auto constexpr DetailsKeys = std::array<tr_quark, 52>{
    TR_KEY_activityDate,
    TR_KEY_addedDate,
    TR_KEY_bandwidthPriority,
    TR_KEY_comment,
    TR_KEY_corruptEver,
    TR_KEY_creator,
    TR_KEY_dateCreated,
    TR_KEY_desiredAvailable,
    TR_KEY_doneDate,
    TR_KEY_downloadDir,
    TR_KEY_downloadedEver,
    TR_KEY_downloadLimit,
    TR_KEY_downloadLimited,
    TR_KEY_error,
    TR_KEY_errorString,
    TR_KEY_eta,
    TR_KEY_group,
    TR_KEY_hashString,
    TR_KEY_haveUnchecked,
    TR_KEY_haveValid,
    TR_KEY_honorsSessionLimits,
    TR_KEY_id,
    TR_KEY_isFinished,
    TR_KEY_isPrivate,
    TR_KEY_labels,
    TR_KEY_leftUntilDone,
    TR_KEY_magnetLink,
    TR_KEY_name,
    TR_KEY_peersConnected,
    TR_KEY_peersGettingFromUs,
    TR_KEY_peersSendingToUs,
    TR_KEY_peer_limit,
    TR_KEY_pieceCount,
    TR_KEY_pieceSize,
    TR_KEY_rateDownload,
    TR_KEY_rateUpload,
    TR_KEY_recheckProgress,
    TR_KEY_secondsDownloading,
    TR_KEY_secondsSeeding,
    TR_KEY_seedRatioMode,
    TR_KEY_seedRatioLimit,
    TR_KEY_sizeWhenDone,
    TR_KEY_source,
    TR_KEY_startDate,
    TR_KEY_status,
    TR_KEY_totalSize,
    TR_KEY_uploadedEver,
    TR_KEY_uploadLimit,
    TR_KEY_uploadLimited,
    TR_KEY_uploadRatio,
    TR_KEY_webseeds,
    TR_KEY_webseedsSendingToUs
};

static auto constexpr ListKeys = std::array<tr_quark, 15>{
    TR_KEY_addedDate,
    TR_KEY_error,
    TR_KEY_errorString,
    TR_KEY_eta,
    TR_KEY_id,
    TR_KEY_isFinished,
    TR_KEY_leftUntilDone,
    TR_KEY_name,
    TR_KEY_peersGettingFromUs,
    TR_KEY_peersSendingToUs,
    TR_KEY_rateDownload,
    TR_KEY_rateUpload,
    TR_KEY_sizeWhenDone,
    TR_KEY_status,
    TR_KEY_uploadRatio
};

// clang-format on

static size_t writeFunc(void* ptr, size_t size, size_t nmemb, void* vbuf)
{
    auto* const buf = static_cast<evbuffer*>(vbuf);
    size_t const byteCount = size * nmemb;
    evbuffer_add(buf, ptr, byteCount);
    return byteCount;
}

/* look for a session id in the header in case the server gives back a 409 */
static size_t parseResponseHeader(void* ptr, size_t size, size_t nmemb, void* vconfig)
{
    auto& config = *static_cast<Config*>(vconfig);
    auto const* const line = static_cast<char const*>(ptr);
    size_t const line_len = size * nmemb;
    char const* key = TR_RPC_SESSION_ID_HEADER ": ";
    size_t const key_len = strlen(key);

    if (line_len >= key_len && evutil_ascii_strncasecmp(line, key, key_len) == 0)
    {
        char const* begin = line + key_len;
        char const* end = begin;

        while (!isspace(*end))
        {
            ++end;
        }

        config.session_id.assign(begin, end - begin);
    }

    return line_len;
}

static long getTimeoutSecs(std::string_view req)
{
    if (req.find("\"method\":\"blocklist-update\""sv) != std::string_view::npos)
    {
        return 300L;
    }

    return 60L; /* default value */
}

static std::string getStatusString(tr_variant* t)
{
    auto from_us = int64_t{};
    auto status = int64_t{};
    auto to_us = int64_t{};

    if (!tr_variantDictFindInt(t, TR_KEY_status, &status))
    {
        return "";
    }

    switch (status)
    {
    case TR_STATUS_DOWNLOAD_WAIT:
    case TR_STATUS_SEED_WAIT:
        return "Queued";

    case TR_STATUS_STOPPED:
        if (auto flag = bool{}; tr_variantDictFindBool(t, TR_KEY_isFinished, &flag) && flag)
        {
            return "Finished";
        }
        return "Stopped";

    case TR_STATUS_CHECK_WAIT:
        if (auto percent = double{}; tr_variantDictFindReal(t, TR_KEY_recheckProgress, &percent))
        {
            return fmt::format(FMT_STRING("Will Verify ({:.0f}%)"), floor(percent * 100.0));
        }
        return "Will Verify";

    case TR_STATUS_CHECK:
        if (auto percent = double{}; tr_variantDictFindReal(t, TR_KEY_recheckProgress, &percent))
        {
            return fmt::format(FMT_STRING("Verifying ({:.0f}%)"), floor(percent * 100.0));
        }
        return "Verifying";

    case TR_STATUS_DOWNLOAD:
    case TR_STATUS_SEED:
        tr_variantDictFindInt(t, TR_KEY_peersGettingFromUs, &from_us);
        tr_variantDictFindInt(t, TR_KEY_peersSendingToUs, &to_us);
        if (from_us != 0 && to_us != 0)
        {
            return "Up & Down";
        }
        if (to_us != 0)
        {
            return "Downloading";
        }
        if (from_us == 0)
        {
            return "Idle";
        }
        if (auto left_until_done = int64_t{};
            tr_variantDictFindInt(t, TR_KEY_leftUntilDone, &left_until_done) && left_until_done > 0)
        {
            return "Uploading";
        }
        return "Seeding";

    default:
        return "Unknown";
    }
}

static auto constexpr bandwidth_priority_names = std::array<std::string_view, 4>{
    "Low"sv,
    "Normal"sv,
    "High"sv,
    "Invalid"sv,
};

static char* format_date(char* buf, size_t buflen, time_t now)
{
    *fmt::format_to_n(buf, buflen - 1, "{:%a %b %d %T %Y}", fmt::localtime(now)).out = '\0';
    return buf;
}

static void printDetails(tr_variant* top)
{
    tr_variant* args;
    tr_variant* torrents;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args) && tr_variantDictFindList(args, TR_KEY_torrents, &torrents))
    {
        for (size_t ti = 0, tCount = tr_variantListSize(torrents); ti < tCount; ++ti)
        {
            tr_variant* t = tr_variantListChild(torrents, ti);
            tr_variant* l;
            char buf[512];
            int64_t i;
            int64_t j;
            int64_t k;
            bool boolVal;
            double d;
            auto sv = std::string_view{};

            fmt::print("NAME\n");

            if (tr_variantDictFindInt(t, TR_KEY_id, &i))
            {
                fmt::print("  Id: {:d}\n", i);
            }

            if (tr_variantDictFindStrView(t, TR_KEY_name, &sv))
            {
                fmt::print("  Name: {:s}\n", sv);
            }

            if (tr_variantDictFindStrView(t, TR_KEY_hashString, &sv))
            {
                fmt::print("  Hash: {:s}\n", sv);
            }

            if (tr_variantDictFindStrView(t, TR_KEY_magnetLink, &sv))
            {
                fmt::print("  Magnet: {:s}\n", sv);
            }

            if (tr_variantDictFindList(t, TR_KEY_labels, &l))
            {
                fmt::print("  Labels: ");

                for (size_t child_idx = 0, n_children = tr_variantListSize(l); child_idx < n_children; ++child_idx)
                {
                    if (tr_variantGetStrView(tr_variantListChild(l, child_idx), &sv))
                    {
                        fmt::print(child_idx == 0 ? "{:s}" : ", {:s}", sv);
                    }
                }

                fmt::print("\n");
            }

            if (tr_variantDictFindStrView(t, TR_KEY_group, &sv) && !sv.empty())
            {
                fmt::print("  Bandwidth group: {:s}\n", sv);
            }

            fmt::print("\n");

            fmt::print("TRANSFER\n");
            fmt::print("  State: {:s}\n", getStatusString(t));

            if (tr_variantDictFindStrView(t, TR_KEY_downloadDir, &sv))
            {
                fmt::print("  Location: {:s}\n", sv);
            }

            if (tr_variantDictFindBool(t, TR_KEY_sequentialDownload, &boolVal))
            {
                fmt::print("  Sequential Download: {:s}\n", (boolVal ? "Yes" : "No"));
            }

            if (tr_variantDictFindInt(t, TR_KEY_sizeWhenDone, &i) && tr_variantDictFindInt(t, TR_KEY_leftUntilDone, &j))
            {
                fmt::print("  Percent Done: {:s}%\n", strlpercent(100.0 * (i - j) / i));
            }

            if (tr_variantDictFindInt(t, TR_KEY_eta, &i))
            {
                fmt::print("  ETA: {:s}\n", tr_strltime(i));
            }

            if (tr_variantDictFindInt(t, TR_KEY_rateDownload, &i))
            {
                fmt::print("  Download Speed: {:s}\n", tr_formatter_speed_KBps(i / (double)tr_speed_K));
            }

            if (tr_variantDictFindInt(t, TR_KEY_rateUpload, &i))
            {
                fmt::print("  Upload Speed: {:s}\n", tr_formatter_speed_KBps(i / (double)tr_speed_K));
            }

            if (tr_variantDictFindInt(t, TR_KEY_haveUnchecked, &i) && tr_variantDictFindInt(t, TR_KEY_haveValid, &j))
            {
                fmt::print("  Have: {:s} ({:s} verified)\n", strlsize(i + j), strlsize(j));
            }

            if (tr_variantDictFindInt(t, TR_KEY_sizeWhenDone, &i))
            {
                if (i < 1)
                {
                    fmt::print("  Availability: None\n");
                }

                if (tr_variantDictFindInt(t, TR_KEY_desiredAvailable, &j) && tr_variantDictFindInt(t, TR_KEY_leftUntilDone, &k))
                {
                    j += i - k;
                    fmt::print("  Availability: {:s}%\n", strlpercent(100.0 * j / i));
                }

                if (tr_variantDictFindInt(t, TR_KEY_totalSize, &j))
                {
                    fmt::print("  Total size: {:s} ({:s} wanted)\n", strlsize(j), strlsize(i));
                }
            }

            if (tr_variantDictFindInt(t, TR_KEY_downloadedEver, &i))
            {
                if (auto corrupt = int64_t{}; tr_variantDictFindInt(t, TR_KEY_corruptEver, &corrupt) && corrupt != 0)
                {
                    fmt::print("  Downloaded: {:s} (+{:s} discarded after failed checksum)\n", strlsize(i), strlsize(corrupt));
                }
                else
                {
                    fmt::print("  Downloaded: {:s}\n", strlsize(i));
                }
            }

            if (tr_variantDictFindInt(t, TR_KEY_uploadedEver, &i))
            {
                fmt::print("  Uploaded: {:s}\n", strlsize(i));

                if (tr_variantDictFindInt(t, TR_KEY_sizeWhenDone, &j))
                {
                    fmt::print("  Ratio: {:s}\n", strlratio(i, j));
                }
            }

            if (tr_variantDictFindStrView(t, TR_KEY_errorString, &sv) && !std::empty(sv) &&
                tr_variantDictFindInt(t, TR_KEY_error, &i) && i != 0)
            {
                switch (i)
                {
                case TR_STAT_TRACKER_WARNING:
                    fmt::print("  Tracker gave a warning: {:s}\n", sv);
                    break;

                case TR_STAT_TRACKER_ERROR:
                    fmt::print("  Tracker gave an error: {:s}\n", sv);
                    break;

                case TR_STAT_LOCAL_ERROR:
                    fmt::print("  Error: {:s}\n", sv);
                    break;

                default:
                    break; /* no error */
                }
            }

            if (tr_variantDictFindInt(t, TR_KEY_peersConnected, &i) &&
                tr_variantDictFindInt(t, TR_KEY_peersGettingFromUs, &j) &&
                tr_variantDictFindInt(t, TR_KEY_peersSendingToUs, &k))
            {
                fmt::print("  Peers: connected to {:d}, uploading to {:d}, downloading from {:d}\n", i, j, k);
            }

            if (tr_variantDictFindList(t, TR_KEY_webseeds, &l) && tr_variantDictFindInt(t, TR_KEY_webseedsSendingToUs, &i))
            {
                int64_t const n = tr_variantListSize(l);

                if (n > 0)
                {
                    fmt::print("  Web Seeds: downloading from {:d} of {:d} web seeds\n", i, n);
                }
            }

            fmt::print("\n");

            fmt::print("HISTORY\n");

            if (tr_variantDictFindInt(t, TR_KEY_addedDate, &i) && i != 0)
            {
                fmt::print("  Date added:       {:s}\n", format_date(buf, sizeof(buf), i));
            }

            if (tr_variantDictFindInt(t, TR_KEY_doneDate, &i) && i != 0)
            {
                fmt::print("  Date finished:    {:s}\n", format_date(buf, sizeof(buf), i));
            }

            if (tr_variantDictFindInt(t, TR_KEY_startDate, &i) && i != 0)
            {
                fmt::print("  Date started:     {:s}\n", format_date(buf, sizeof(buf), i));
            }

            if (tr_variantDictFindInt(t, TR_KEY_activityDate, &i) && i != 0)
            {
                fmt::print("  Latest activity:  {:s}\n", format_date(buf, sizeof(buf), i));
            }

            if (tr_variantDictFindInt(t, TR_KEY_secondsDownloading, &i) && i > 0)
            {
                fmt::print("  Downloading Time: {:s}\n", tr_strltime(i));
            }

            if (tr_variantDictFindInt(t, TR_KEY_secondsSeeding, &i) && i > 0)
            {
                fmt::print("  Seeding Time:     {:s}\n", tr_strltime(i));
            }

            fmt::print("\n");

            fmt::print("ORIGINS\n");

            if (tr_variantDictFindInt(t, TR_KEY_dateCreated, &i) && i != 0)
            {
                fmt::print("  Date created: {:s}\n", format_date(buf, sizeof(buf), i));
            }

            if (tr_variantDictFindBool(t, TR_KEY_isPrivate, &boolVal))
            {
                fmt::print("  Public torrent: {:s}\n", (boolVal ? "No" : "Yes"));
            }

            if (tr_variantDictFindStrView(t, TR_KEY_comment, &sv) && !std::empty(sv))
            {
                fmt::print("  Comment: {:s}\n", sv);
            }

            if (tr_variantDictFindStrView(t, TR_KEY_creator, &sv) && !std::empty(sv))
            {
                fmt::print("  Creator: {:s}\n", sv);
            }

            if (tr_variantDictFindStrView(t, TR_KEY_source, &sv) && !std::empty(sv))
            {
                fmt::print("  Source: {:s}\n", sv);
            }

            if (tr_variantDictFindInt(t, TR_KEY_pieceCount, &i))
            {
                fmt::print("  Piece Count: {:d}\n", i);
            }

            if (tr_variantDictFindInt(t, TR_KEY_pieceSize, &i))
            {
                fmt::print("  Piece Size: {:s}\n", strlmem(i));
            }

            fmt::print("\n");

            fmt::print("LIMITS & BANDWIDTH\n");

            if (tr_variantDictFindBool(t, TR_KEY_downloadLimited, &boolVal) &&
                tr_variantDictFindInt(t, TR_KEY_downloadLimit, &i))
            {
                fmt::print("  Download Limit: ");

                if (boolVal)
                {
                    fmt::print("{:s}\n", tr_formatter_speed_KBps(i));
                }
                else
                {
                    fmt::print("Unlimited\n");
                }
            }

            if (tr_variantDictFindBool(t, TR_KEY_uploadLimited, &boolVal) && tr_variantDictFindInt(t, TR_KEY_uploadLimit, &i))
            {
                fmt::print("  Upload Limit: ");

                if (boolVal)
                {
                    fmt::print("{:s}\n", tr_formatter_speed_KBps(i));
                }
                else
                {
                    fmt::print("Unlimited\n");
                }
            }

            if (tr_variantDictFindInt(t, TR_KEY_seedRatioMode, &i))
            {
                switch (i)
                {
                case TR_RATIOLIMIT_GLOBAL:
                    fmt::print("  Ratio Limit: Default\n");
                    break;

                case TR_RATIOLIMIT_SINGLE:
                    if (tr_variantDictFindReal(t, TR_KEY_seedRatioLimit, &d))
                    {
                        fmt::print("  Ratio Limit: {:s}\n", strlratio2(d));
                    }

                    break;

                case TR_RATIOLIMIT_UNLIMITED:
                    fmt::print("  Ratio Limit: Unlimited\n");
                    break;

                default:
                    break;
                }
            }

            if (tr_variantDictFindBool(t, TR_KEY_honorsSessionLimits, &boolVal))
            {
                fmt::print("  Honors Session Limits: {:s}\n", boolVal ? "Yes" : "No");
            }

            if (tr_variantDictFindInt(t, TR_KEY_peer_limit, &i))
            {
                fmt::print("  Peer limit: {:d}\n", i);
            }

            if (tr_variantDictFindInt(t, TR_KEY_bandwidthPriority, &i))
            {
                fmt::print("  Bandwidth Priority: {:s}\n", bandwidth_priority_names[(i + 1) & 3]);
            }

            fmt::print("\n");
        }
    }
}

static void printFileList(tr_variant* top)
{
    tr_variant* args;
    tr_variant* torrents;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args) && tr_variantDictFindList(args, TR_KEY_torrents, &torrents))
    {
        for (size_t i = 0, in = tr_variantListSize(torrents); i < in; ++i)
        {
            tr_variant* d = tr_variantListChild(torrents, i);
            tr_variant* files;
            tr_variant* priorities;
            tr_variant* wanteds;
            auto name = std::string_view{};

            if (tr_variantDictFindStrView(d, TR_KEY_name, &name) && tr_variantDictFindList(d, TR_KEY_files, &files) &&
                tr_variantDictFindList(d, TR_KEY_priorities, &priorities) && tr_variantDictFindList(d, TR_KEY_wanted, &wanteds))
            {
                auto const jn = tr_variantListSize(files);
                fmt::print("{:s} ({:d} files):\n", name, jn);
                printf("%3s  %4s %8s %3s %9s  %s\n", "#", "Done", "Priority", "Get", "Size", "Name");

                for (size_t j = 0; j < jn; ++j)
                {
                    int64_t have;
                    int64_t length;
                    int64_t priority;
                    bool wanted;
                    auto filename = std::string_view{};
                    tr_variant* file = tr_variantListChild(files, j);

                    if (tr_variantDictFindInt(file, TR_KEY_length, &length) &&
                        tr_variantDictFindStrView(file, TR_KEY_name, &filename) &&
                        tr_variantDictFindInt(file, TR_KEY_bytesCompleted, &have) &&
                        tr_variantGetInt(tr_variantListChild(priorities, j), &priority) &&
                        tr_variantGetBool(tr_variantListChild(wanteds, j), &wanted))
                    {
                        double percent = (double)have / length;
                        char const* pristr;

                        switch (priority)
                        {
                        case TR_PRI_LOW:
                            pristr = "Low";
                            break;

                        case TR_PRI_HIGH:
                            pristr = "High";
                            break;

                        default:
                            pristr = "Normal";
                            break;
                        }

                        fmt::print(
                            FMT_STRING("{:3d}: {:3.0f}% {:<8s} {:<3s} {:9s}  {:s}\n"),
                            j,
                            floor(100.0 * percent),
                            pristr,
                            wanted ? "Yes" : "No",
                            strlsize(length),
                            filename);
                    }
                }
            }
        }
    }
}

static void printPeersImpl(tr_variant* peers)
{
    printf("%-40s  %-12s  %-5s %-6s  %-6s  %s\n", "Address", "Flags", "Done", "Down", "Up", "Client");

    for (size_t i = 0, n = tr_variantListSize(peers); i < n; ++i)
    {
        auto address = std::string_view{};
        auto client = std::string_view{};
        auto flagstr = std::string_view{};
        auto progress = double{};
        auto rateToClient = int64_t{};
        auto rateToPeer = int64_t{};

        tr_variant* d = tr_variantListChild(peers, i);

        if (tr_variantDictFindStrView(d, TR_KEY_address, &address) &&
            tr_variantDictFindStrView(d, TR_KEY_clientName, &client) && tr_variantDictFindReal(d, TR_KEY_progress, &progress) &&
            tr_variantDictFindStrView(d, TR_KEY_flagStr, &flagstr) &&
            tr_variantDictFindInt(d, TR_KEY_rateToClient, &rateToClient) &&
            tr_variantDictFindInt(d, TR_KEY_rateToPeer, &rateToPeer))
        {
            fmt::print(
                FMT_STRING("{:<40s}  {:<12s}  {:<5.1f} {:6.1f}  {:6.1f}  {:s}\n"),
                address,
                flagstr,
                progress * 100.0,
                rateToClient / static_cast<double>(tr_speed_K),
                rateToPeer / static_cast<double>(tr_speed_K),
                client);
        }
    }
}

static void printPeers(tr_variant* top)
{
    tr_variant* args;
    tr_variant* torrents;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args) && tr_variantDictFindList(args, TR_KEY_torrents, &torrents))
    {
        for (size_t i = 0, n = tr_variantListSize(torrents); i < n; ++i)
        {
            tr_variant* peers;
            tr_variant* torrent = tr_variantListChild(torrents, i);

            if (tr_variantDictFindList(torrent, TR_KEY_peers, &peers))
            {
                printPeersImpl(peers);

                if (i + 1 < n)
                {
                    fmt::print("\n");
                }
            }
        }
    }
}

static void printPiecesImpl(std::string_view raw, size_t piece_count)
{
    auto const str = tr_base64_decode(raw);
    fmt::print("  ");

    size_t piece = 0;
    size_t const col_width = 64;
    for (auto const ch : str)
    {
        for (int bit = 0; piece < piece_count && bit < 8; ++bit, ++piece)
        {
            printf("%c", (ch & (1 << (7 - bit))) != 0 ? '1' : '0');
        }

        fmt::print(" ");

        if (piece % col_width == 0)
        {
            fmt::print("\n  ");
        }
    }

    fmt::print("\n");
}

static void printPieces(tr_variant* top)
{
    tr_variant* args;
    tr_variant* torrents;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args) && tr_variantDictFindList(args, TR_KEY_torrents, &torrents))
    {
        for (size_t i = 0, n = tr_variantListSize(torrents); i < n; ++i)
        {
            int64_t j;
            auto raw = std::string_view{};
            tr_variant* torrent = tr_variantListChild(torrents, i);

            if (tr_variantDictFindStrView(torrent, TR_KEY_pieces, &raw) &&
                tr_variantDictFindInt(torrent, TR_KEY_pieceCount, &j))
            {
                assert(j >= 0);
                printPiecesImpl(raw, (size_t)j);

                if (i + 1 < n)
                {
                    fmt::print("\n");
                }
            }
        }
    }
}

static void printPortTest(tr_variant* top)
{
    tr_variant* args;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args))
    {
        bool boolVal;

        if (tr_variantDictFindBool(args, TR_KEY_port_is_open, &boolVal))
        {
            fmt::print("Port is open: {:s}\n", boolVal ? "Yes" : "No");
        }
    }
}

static void printTorrentList(tr_variant* top)
{
    tr_variant* args;
    tr_variant* list;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args) && tr_variantDictFindList(args, TR_KEY_torrents, &list))
    {
        int64_t total_size = 0;
        double total_up = 0;
        double total_down = 0;

        printf(
            "%6s   %-4s  %9s  %-9s  %6s  %6s  %-5s  %-11s  %s\n",
            "ID",
            "Done",
            "Have",
            "ETA",
            "Up",
            "Down",
            "Ratio",
            "Status",
            "Name");

        size_t num_torrents = tr_variantListSize(list);

        std::vector<tr_variant*> tptrs;
        tptrs.reserve(num_torrents);

        for (size_t i = 0; i < num_torrents; ++i)
        {
            tr_variant* d = tr_variantListChild(list, i);
            if (tr_variantDictFindInt(d, TR_KEY_id, nullptr))
                tptrs.push_back(d);
        }

        std::sort(
            tptrs.begin(),
            tptrs.end(),
            [](tr_variant* f, tr_variant* s)
            {
                int64_t f_time = INT64_MIN, s_time = INT64_MIN;
                (void)tr_variantDictFindInt(f, TR_KEY_addedDate, &f_time);
                (void)tr_variantDictFindInt(s, TR_KEY_addedDate, &s_time);
                return f_time < s_time;
            });

        for (auto const& d : tptrs)
        {
            int64_t torId;
            int64_t eta;
            int64_t status;
            int64_t up;
            int64_t down;
            int64_t sizeWhenDone;
            int64_t leftUntilDone;
            double ratio;
            auto name = std::string_view{};

            if (tr_variantDictFindInt(d, TR_KEY_eta, &eta) && tr_variantDictFindInt(d, TR_KEY_id, &torId) &&
                tr_variantDictFindInt(d, TR_KEY_leftUntilDone, &leftUntilDone) &&
                tr_variantDictFindStrView(d, TR_KEY_name, &name) && tr_variantDictFindInt(d, TR_KEY_rateDownload, &down) &&
                tr_variantDictFindInt(d, TR_KEY_rateUpload, &up) &&
                tr_variantDictFindInt(d, TR_KEY_sizeWhenDone, &sizeWhenDone) &&
                tr_variantDictFindInt(d, TR_KEY_status, &status) && tr_variantDictFindReal(d, TR_KEY_uploadRatio, &ratio))
            {
                int64_t error;

                auto const eta_str = leftUntilDone != 0 || eta != -1 ? etaToString(eta) : "Done";
                auto const error_mark = tr_variantDictFindInt(d, TR_KEY_error, &error) && error ? '*' : ' ';
                auto const done_str = sizeWhenDone != 0 ?
                    fmt::format(FMT_STRING("{:.0f}%"), (100.0 * (sizeWhenDone - leftUntilDone) / sizeWhenDone)) :
                    std::string{ "n/a" };

                fmt::print(
                    FMT_STRING("{:6d}{:c}  {:>4s}  {:>9s}  {:<9s}  {:6.1f}  {:6.1f}  {:>5s}  {:<11s}  {:s}\n"),
                    torId,
                    error_mark,
                    done_str,
                    strlsize(sizeWhenDone - leftUntilDone),
                    eta_str,
                    up / static_cast<double>(tr_speed_K),
                    down / static_cast<double>(tr_speed_K),
                    strlratio2(ratio),
                    getStatusString(d),
                    name);

                total_up += up;
                total_down += down;
                total_size += sizeWhenDone - leftUntilDone;
            }
        }

        fmt::print(
            FMT_STRING("Sum:           {:>9s}             {:6.1f}  {:6.1f}\n"),
            strlsize(total_size).c_str(),
            total_up / static_cast<double>(tr_speed_K),
            total_down / static_cast<double>(tr_speed_K));
    }
}

static void printTrackersImpl(tr_variant* trackerStats)
{
    for (size_t i = 0, n = tr_variantListSize(trackerStats); i < n; ++i)
    {
        tr_variant* const t = tr_variantListChild(trackerStats, i);

        auto announceState = int64_t{};
        auto downloadCount = int64_t{};
        auto hasAnnounced = bool{};
        auto hasScraped = bool{};
        auto host = std::string_view{};
        auto isBackup = bool{};
        auto lastAnnouncePeerCount = int64_t{};
        auto lastAnnounceResult = std::string_view{};
        auto lastAnnounceStartTime = int64_t{};
        auto lastAnnounceTime = int64_t{};
        auto lastScrapeResult = std::string_view{};
        auto lastScrapeStartTime = int64_t{};
        auto lastScrapeSucceeded = bool{};
        auto lastScrapeTime = int64_t{};
        auto lastScrapeTimedOut = bool{};
        auto leecherCount = int64_t{};
        auto nextAnnounceTime = int64_t{};
        auto nextScrapeTime = int64_t{};
        auto scrapeState = int64_t{};
        auto seederCount = int64_t{};
        auto tier = int64_t{};
        auto trackerId = int64_t{};
        bool lastAnnounceSucceeded;
        bool lastAnnounceTimedOut;

        if (tr_variantDictFindInt(t, TR_KEY_downloadCount, &downloadCount) &&
            tr_variantDictFindBool(t, TR_KEY_hasAnnounced, &hasAnnounced) &&
            tr_variantDictFindBool(t, TR_KEY_hasScraped, &hasScraped) && tr_variantDictFindStrView(t, TR_KEY_host, &host) &&
            tr_variantDictFindInt(t, TR_KEY_id, &trackerId) && tr_variantDictFindBool(t, TR_KEY_isBackup, &isBackup) &&
            tr_variantDictFindInt(t, TR_KEY_announceState, &announceState) &&
            tr_variantDictFindInt(t, TR_KEY_scrapeState, &scrapeState) &&
            tr_variantDictFindInt(t, TR_KEY_lastAnnouncePeerCount, &lastAnnouncePeerCount) &&
            tr_variantDictFindStrView(t, TR_KEY_lastAnnounceResult, &lastAnnounceResult) &&
            tr_variantDictFindInt(t, TR_KEY_lastAnnounceStartTime, &lastAnnounceStartTime) &&
            tr_variantDictFindBool(t, TR_KEY_lastAnnounceSucceeded, &lastAnnounceSucceeded) &&
            tr_variantDictFindInt(t, TR_KEY_lastAnnounceTime, &lastAnnounceTime) &&
            tr_variantDictFindBool(t, TR_KEY_lastAnnounceTimedOut, &lastAnnounceTimedOut) &&
            tr_variantDictFindStrView(t, TR_KEY_lastScrapeResult, &lastScrapeResult) &&
            tr_variantDictFindInt(t, TR_KEY_lastScrapeStartTime, &lastScrapeStartTime) &&
            tr_variantDictFindBool(t, TR_KEY_lastScrapeSucceeded, &lastScrapeSucceeded) &&
            tr_variantDictFindInt(t, TR_KEY_lastScrapeTime, &lastScrapeTime) &&
            tr_variantDictFindBool(t, TR_KEY_lastScrapeTimedOut, &lastScrapeTimedOut) &&
            tr_variantDictFindInt(t, TR_KEY_leecherCount, &leecherCount) &&
            tr_variantDictFindInt(t, TR_KEY_nextAnnounceTime, &nextAnnounceTime) &&
            tr_variantDictFindInt(t, TR_KEY_nextScrapeTime, &nextScrapeTime) &&
            tr_variantDictFindInt(t, TR_KEY_seederCount, &seederCount) && tr_variantDictFindInt(t, TR_KEY_tier, &tier))
        {
            time_t const now = time(nullptr);

            fmt::print("\n");
            fmt::print("  Tracker {:d}: {:s}\n", trackerId, host);

            if (isBackup)
            {
                fmt::print("  Backup on tier {:d}\n", tier);
            }
            else
            {
                fmt::print("  Active in tier {:d}\n", tier);
            }

            if (!isBackup)
            {
                if (hasAnnounced && announceState != TR_TRACKER_INACTIVE)
                {
                    auto const timestr = tr_strltime(now - lastAnnounceTime);

                    if (lastAnnounceSucceeded)
                    {
                        fmt::print("  Got a list of {:d} peers {:s} ago\n", lastAnnouncePeerCount, timestr);
                    }
                    else if (lastAnnounceTimedOut)
                    {
                        fmt::print("  Peer list request timed out; will retry\n");
                    }
                    else
                    {
                        fmt::print("  Got an error '{:s}' {:s} ago\n", lastAnnounceResult, timestr);
                    }
                }

                switch (announceState)
                {
                case TR_TRACKER_INACTIVE:
                    fmt::print("  No updates scheduled\n");
                    break;

                case TR_TRACKER_WAITING:
                    fmt::print("  Asking for more peers in {:s}\n", tr_strltime(nextAnnounceTime - now));
                    break;

                case TR_TRACKER_QUEUED:
                    fmt::print("  Queued to ask for more peers\n");
                    break;

                case TR_TRACKER_ACTIVE:
                    fmt::print("  Asking for more peers now... {:s}\n", tr_strltime(now - lastAnnounceStartTime));
                    break;
                }

                if (hasScraped)
                {
                    auto const timestr = tr_strltime(now - lastScrapeTime);

                    if (lastScrapeSucceeded)
                    {
                        fmt::print(
                            "  Tracker had {:d} seeders and {:d} leechers {:s} ago\n",
                            seederCount,
                            leecherCount,
                            timestr);
                    }
                    else if (lastScrapeTimedOut)
                    {
                        fmt::print("  Tracker scrape timed out; will retry\n");
                    }
                    else
                    {
                        fmt::print("  Got a scrape error '{:s}' {:s} ago\n", lastScrapeResult, timestr);
                    }
                }

                switch (scrapeState)
                {
                case TR_TRACKER_INACTIVE:
                    break;

                case TR_TRACKER_WAITING:
                    fmt::print("  Asking for peer counts in {:s}\n", tr_strltime(nextScrapeTime - now));
                    break;

                case TR_TRACKER_QUEUED:
                    fmt::print("  Queued to ask for peer counts\n");
                    break;

                case TR_TRACKER_ACTIVE:
                    fmt::print("  Asking for peer counts now... {:s}\n", tr_strltime(now - lastScrapeStartTime));
                    break;
                }
            }
        }
    }
}

static void printTrackers(tr_variant* top)
{
    tr_variant* args;
    tr_variant* torrents;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args) && tr_variantDictFindList(args, TR_KEY_torrents, &torrents))
    {
        for (size_t i = 0, n = tr_variantListSize(torrents); i < n; ++i)
        {
            tr_variant* trackerStats;
            tr_variant* torrent = tr_variantListChild(torrents, i);

            if (tr_variantDictFindList(torrent, TR_KEY_trackerStats, &trackerStats))
            {
                printTrackersImpl(trackerStats);

                if (i + 1 < n)
                {
                    fmt::print("\n");
                }
            }
        }
    }
}

static void printSession(tr_variant* top)
{
    tr_variant* args;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args))
    {
        int64_t i;
        bool boolVal;
        auto sv = std::string_view{};

        fmt::print("VERSION\n");

        if (tr_variantDictFindStrView(args, TR_KEY_version, &sv))
        {
            fmt::print("  Daemon version: {:s}\n", sv);
        }

        if (tr_variantDictFindInt(args, TR_KEY_rpc_version, &i))
        {
            fmt::print("  RPC version: {:d}\n", i);
        }

        if (tr_variantDictFindInt(args, TR_KEY_rpc_version_minimum, &i))
        {
            fmt::print("  RPC minimum version: {:d}\n", i);
        }

        fmt::print("\n");

        fmt::print("CONFIG\n");

        if (tr_variantDictFindStrView(args, TR_KEY_config_dir, &sv))
        {
            fmt::print("  Configuration directory: {:s}\n", sv);
        }

        if (tr_variantDictFindStrView(args, TR_KEY_download_dir, &sv))
        {
            fmt::print("  Download directory: {:s}\n", sv);
        }

        if (tr_variantDictFindInt(args, TR_KEY_peer_port, &i))
        {
            fmt::print("  Listenport: {:d}\n", i);
        }

        if (tr_variantDictFindBool(args, TR_KEY_port_forwarding_enabled, &boolVal))
        {
            fmt::print("  Portforwarding enabled: {:s}\n", boolVal ? "Yes" : "No");
        }

        if (tr_variantDictFindBool(args, TR_KEY_utp_enabled, &boolVal))
        {
            fmt::print("  µTP enabled: {:s}\n", boolVal ? "Yes" : "No");
        }

        if (tr_variantDictFindBool(args, TR_KEY_dht_enabled, &boolVal))
        {
            fmt::print("  Distributed hash table enabled: {:s}\n", boolVal ? "Yes" : "No");
        }

        if (tr_variantDictFindBool(args, TR_KEY_lpd_enabled, &boolVal))
        {
            fmt::print("  Local peer discovery enabled: {:s}\n", boolVal ? "Yes" : "No");
        }

        if (tr_variantDictFindBool(args, TR_KEY_pex_enabled, &boolVal))
        {
            fmt::print("  Peer exchange allowed: {:s}\n", boolVal ? "Yes" : "No");
        }

        if (tr_variantDictFindStrView(args, TR_KEY_encryption, &sv))
        {
            fmt::print("  Encryption: {:s}\n", sv);
        }

        if (tr_variantDictFindInt(args, TR_KEY_cache_size_mb, &i))
        {
            fmt::print("  Maximum memory cache size: {:s}\n", tr_formatter_mem_MB(i));
        }

        fmt::print("\n");

        {
            bool altEnabled;
            bool altTimeEnabled;
            bool upEnabled;
            bool downEnabled;
            bool seedRatioLimited;
            int64_t altDown;
            int64_t altUp;
            int64_t altBegin;
            int64_t altEnd;
            int64_t altDay;
            int64_t upLimit;
            int64_t downLimit;
            int64_t peerLimit;
            double seedRatioLimit;

            if (tr_variantDictFindInt(args, TR_KEY_alt_speed_down, &altDown) &&
                tr_variantDictFindBool(args, TR_KEY_alt_speed_enabled, &altEnabled) &&
                tr_variantDictFindInt(args, TR_KEY_alt_speed_time_begin, &altBegin) &&
                tr_variantDictFindBool(args, TR_KEY_alt_speed_time_enabled, &altTimeEnabled) &&
                tr_variantDictFindInt(args, TR_KEY_alt_speed_time_end, &altEnd) &&
                tr_variantDictFindInt(args, TR_KEY_alt_speed_time_day, &altDay) &&
                tr_variantDictFindInt(args, TR_KEY_alt_speed_up, &altUp) &&
                tr_variantDictFindInt(args, TR_KEY_peer_limit_global, &peerLimit) &&
                tr_variantDictFindInt(args, TR_KEY_speed_limit_down, &downLimit) &&
                tr_variantDictFindBool(args, TR_KEY_speed_limit_down_enabled, &downEnabled) &&
                tr_variantDictFindInt(args, TR_KEY_speed_limit_up, &upLimit) &&
                tr_variantDictFindBool(args, TR_KEY_speed_limit_up_enabled, &upEnabled) &&
                tr_variantDictFindReal(args, TR_KEY_seedRatioLimit, &seedRatioLimit) &&
                tr_variantDictFindBool(args, TR_KEY_seedRatioLimited, &seedRatioLimited))
            {
                fmt::print("LIMITS\n");
                fmt::print("  Peer limit: {:d}\n", peerLimit);

                fmt::print("  Default seed ratio limit: {:s}\n", seedRatioLimited ? strlratio2(seedRatioLimit) : "Unlimited");

                std::string effective_up_limit;

                if (altEnabled)
                {
                    effective_up_limit = tr_formatter_speed_KBps(altUp);
                }
                else if (upEnabled)
                {
                    effective_up_limit = tr_formatter_speed_KBps(upLimit);
                }
                else
                {
                    effective_up_limit = "Unlimited"s;
                }

                fmt::print(
                    FMT_STRING("  Upload speed limit: {:s} ({:s} limit: {:s}; {:s} turtle limit: {:s})\n"),
                    effective_up_limit,
                    upEnabled ? "Enabled" : "Disabled",
                    tr_formatter_speed_KBps(upLimit),
                    altEnabled ? "Enabled" : "Disabled",
                    tr_formatter_speed_KBps(altUp));

                std::string effective_down_limit;

                if (altEnabled)
                {
                    effective_down_limit = tr_formatter_speed_KBps(altDown);
                }
                else if (downEnabled)
                {
                    effective_down_limit = tr_formatter_speed_KBps(downLimit);
                }
                else
                {
                    effective_down_limit = "Unlimited"s;
                }

                fmt::print(
                    FMT_STRING("  Download speed limit: {:s} ({:s} limit: {:s}; {:s} turtle limit: {:s})\n"),
                    effective_down_limit,
                    downEnabled ? "Enabled" : "Disabled",
                    tr_formatter_speed_KBps(downLimit),
                    altEnabled ? "Enabled" : "Disabled",
                    tr_formatter_speed_KBps(altDown));

                if (altTimeEnabled)
                {
                    printf(
                        "  Turtle schedule: %02d:%02d - %02d:%02d  ",
                        (int)(altBegin / 60),
                        (int)(altBegin % 60),
                        (int)(altEnd / 60),
                        (int)(altEnd % 60));

                    if ((altDay & TR_SCHED_SUN) != 0)
                    {
                        fmt::print("Sun ");
                    }

                    if ((altDay & TR_SCHED_MON) != 0)
                    {
                        fmt::print("Mon ");
                    }

                    if ((altDay & TR_SCHED_TUES) != 0)
                    {
                        fmt::print("Tue ");
                    }

                    if ((altDay & TR_SCHED_WED) != 0)
                    {
                        fmt::print("Wed ");
                    }

                    if ((altDay & TR_SCHED_THURS) != 0)
                    {
                        fmt::print("Thu ");
                    }

                    if ((altDay & TR_SCHED_FRI) != 0)
                    {
                        fmt::print("Fri ");
                    }

                    if ((altDay & TR_SCHED_SAT) != 0)
                    {
                        fmt::print("Sat ");
                    }

                    fmt::print("\n");
                }
            }
        }

        fmt::print("\n");

        fmt::print("MISC\n");

        if (tr_variantDictFindBool(args, TR_KEY_start_added_torrents, &boolVal))
        {
            fmt::print("  Autostart added torrents: {:s}\n", boolVal ? "Yes" : "No");
        }

        if (tr_variantDictFindBool(args, TR_KEY_trash_original_torrent_files, &boolVal))
        {
            fmt::print("  Delete automatically added torrents: {:s}\n", boolVal ? "Yes" : "No");
        }
    }
}

static void printSessionStats(tr_variant* top)
{
    tr_variant* args;
    tr_variant* d;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args))
    {
        int64_t up;
        int64_t down;
        int64_t secs;
        int64_t sessions;

        if (tr_variantDictFindDict(args, TR_KEY_current_stats, &d) && tr_variantDictFindInt(d, TR_KEY_uploadedBytes, &up) &&
            tr_variantDictFindInt(d, TR_KEY_downloadedBytes, &down) && tr_variantDictFindInt(d, TR_KEY_secondsActive, &secs))
        {
            fmt::print("\nCURRENT SESSION\n");
            fmt::print("  Uploaded:   {:s}\n", strlsize(up));
            fmt::print("  Downloaded: {:s}\n", strlsize(down));
            fmt::print("  Ratio:      {:s}\n", strlratio(up, down));
            fmt::print("  Duration:   {:s}\n", tr_strltime(secs));
        }

        if (tr_variantDictFindDict(args, TR_KEY_cumulative_stats, &d) &&
            tr_variantDictFindInt(d, TR_KEY_sessionCount, &sessions) && tr_variantDictFindInt(d, TR_KEY_uploadedBytes, &up) &&
            tr_variantDictFindInt(d, TR_KEY_downloadedBytes, &down) && tr_variantDictFindInt(d, TR_KEY_secondsActive, &secs))
        {
            fmt::print("\nTOTAL\n");
            fmt::print("  Started {:d} times\n", sessions);
            fmt::print("  Uploaded:   {:s}\n", strlsize(up));
            fmt::print("  Downloaded: {:s}\n", strlsize(down));
            fmt::print("  Ratio:      {:s}\n", strlratio(up, down));
            fmt::print("  Duration:   {:s}\n", tr_strltime(secs));
        }
    }
}

static void printGroups(tr_variant* top)
{
    tr_variant* args;
    tr_variant* groups;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args) && tr_variantDictFindList(args, TR_KEY_group, &groups))
    {
        for (size_t i = 0, n = tr_variantListSize(groups); i < n; ++i)
        {
            tr_variant* group = tr_variantListChild(groups, i);
            std::string_view name;
            bool upEnabled;
            bool downEnabled;
            int64_t upLimit;
            int64_t downLimit;
            bool honors;
            if (tr_variantDictFindStrView(group, TR_KEY_name, &name) &&
                tr_variantDictFindInt(group, TR_KEY_downloadLimit, &downLimit) &&
                tr_variantDictFindBool(group, TR_KEY_downloadLimited, &downEnabled) &&
                tr_variantDictFindInt(group, TR_KEY_uploadLimit, &upLimit) &&
                tr_variantDictFindBool(group, TR_KEY_uploadLimited, &upEnabled) &&
                tr_variantDictFindBool(group, TR_KEY_honorsSessionLimits, &honors))
            {
                fmt::print("{:s}: ", name);
                fmt::print(
                    FMT_STRING("Upload speed limit: {:s}, Download speed limit: {:s}, {:s} session bandwidth limits\n"),
                    upEnabled ? tr_formatter_speed_KBps(upLimit).c_str() : "unlimited",
                    downEnabled ? tr_formatter_speed_KBps(downLimit).c_str() : "unlimited",
                    honors ? "honors" : "does not honor");
            }
        }
    }
}

static void filterIds(tr_variant* top, Config& config)
{
    tr_variant* args;
    tr_variant* list;

    std::set<int> ids;

    if (tr_variantDictFindDict(top, TR_KEY_arguments, &args) && tr_variantDictFindList(args, TR_KEY_torrents, &list))
    {
        size_t pos = 0;
        bool negate = false;
        std::string_view arg;

        if (config.filter[pos] == '~')
        {
            ++pos;
            negate = true;
        }
        if (std::size(config.filter) > pos + 1 && config.filter[pos + 1] == ':')
        {
            arg = &config.filter[pos + 2];
        }

        for (size_t i = 0, n = tr_variantListSize(list); i < n; ++i)
        {
            tr_variant* d = tr_variantListChild(list, i);
            int64_t torId;
            if (!tr_variantDictFindInt(d, TR_KEY_id, &torId) || torId < 0)
            {
                continue;
            }
            bool include = negate;
            auto const status = getStatusString(d);
            switch (config.filter[pos])
            {
            case 'i': // Status = Idle
                if (status == "Idle")
                {
                    include = !include;
                }
                break;
            case 'd': // Downloading (Status is Downloading or Up&Down)
                if (status.find("Down") != std::string::npos)
                {
                    include = !include;
                }
                break;
            case 'u': // Uploading (Status is Uploading, Up&Down or Seeding
                if ((status.find("Up") != std::string::npos) || (status == "Seeding"))
                {
                    include = !include;
                }
                break;
            case 'l': // label
                if (tr_variant * l; tr_variantDictFindList(d, TR_KEY_labels, &l))
                {
                    for (size_t child_idx = 0, n_children = tr_variantListSize(l); child_idx < n_children; ++child_idx)
                    {
                        if (auto sv = std::string_view{};
                            tr_variantGetStrView(tr_variantListChild(l, child_idx), &sv) && arg == sv)
                        {
                            include = !include;
                            break;
                        }
                    }
                }
                break;
            case 'n': // Torrent name substring
                if (std::string_view name; !tr_variantDictFindStrView(d, TR_KEY_name, &name))
                {
                    continue;
                }
                else if (name.find(arg) != std::string::npos)
                {
                    include = !include;
                }
                break;
            case 'r': // Minimal ratio
                if (double ratio; !tr_variantDictFindReal(d, TR_KEY_uploadRatio, &ratio))
                {
                    continue;
                }
                else if (ratio >= std::stof(std::string(arg)))
                {
                    include = !include;
                }
                break;
            case 'w': // Not all torrent wanted
                if (int64_t totalSize; !tr_variantDictFindInt(d, TR_KEY_totalSize, &totalSize) || totalSize < 0)
                {
                    continue;
                }
                else if (int64_t sizeWhenDone;
                         !tr_variantDictFindInt(d, TR_KEY_sizeWhenDone, &sizeWhenDone) || sizeWhenDone < 0)
                {
                    continue;
                }
                else if (totalSize > sizeWhenDone)
                {
                    include = !include;
                }
                break;
            }
            if (include)
            {
                ids.insert(torId);
            }
        }

        auto& res = config.torrent_ids;
        res.clear();
        for (auto const& i : ids)
        {
            res += std::to_string(i) + ",";
        }
        if (res.empty())
        {
            res = ","; // no selected torrents
        }
    }
}
static int processResponse(char const* rpcurl, std::string_view response, Config& config)
{
    auto status = int{ EXIT_SUCCESS };

    if (config.debug)
    {
        fmt::print(stderr, "got response (len {:d}):\n--------\n{:s}\n--------\n", std::size(response), response);
    }

    if (config.json)
    {
        fmt::print("{:s}\n", response);
        return status;
    }

    if (auto otop = tr_variant_serde::json().inplace().parse(response); !otop)
    {
        tr_logAddWarn(fmt::format("Unable to parse response '{}'", response));
        status |= EXIT_FAILURE;
    }
    else
    {
        auto& top = *otop;

        if (auto sv = std::string_view{}; tr_variantDictFindStrView(&top, TR_KEY_result, &sv))
        {
            if (sv != "success"sv)
            {
                fmt::print("Error: {:s}\n", sv);
                status |= EXIT_FAILURE;
            }
            else
            {
                int64_t tag = -1;
                (void)tr_variantDictFindInt(&top, TR_KEY_tag, &tag);

                switch (tag)
                {
                case TAG_SESSION:
                    printSession(&top);
                    break;

                case TAG_STATS:
                    printSessionStats(&top);
                    break;

                case TAG_DETAILS:
                    printDetails(&top);
                    break;

                case TAG_FILES:
                    printFileList(&top);
                    break;

                case TAG_LIST:
                    printTorrentList(&top);
                    break;

                case TAG_PEERS:
                    printPeers(&top);
                    break;

                case TAG_PIECES:
                    printPieces(&top);
                    break;

                case TAG_PORTTEST:
                    printPortTest(&top);
                    break;

                case TAG_TRACKERS:
                    printTrackers(&top);
                    break;

                case TAG_GROUPS:
                    printGroups(&top);
                    break;

                case TAG_FILTER:
                    filterIds(&top, config);
                    break;

                case TAG_TORRENT_ADD:
                    {
                        int64_t i;
                        tr_variant* b = &top;

                        if (tr_variantDictFindDict(&top, Arguments, &b) &&
                            tr_variantDictFindDict(b, TR_KEY_torrent_added, &b) && tr_variantDictFindInt(b, TR_KEY_id, &i))
                        {
                            config.torrent_ids = std::to_string(i);
                        }
                        [[fallthrough]];
                    }

                default:
                    if (!tr_variantDictFindStrView(&top, TR_KEY_result, &sv))
                    {
                        status |= EXIT_FAILURE;
                    }
                    else
                    {
                        fmt::print("{:s} responded: {:s}\n", rpcurl, sv);

                        if (sv != "success"sv)
                        {
                            status |= EXIT_FAILURE;
                        }
                    }
                }
            }
        }
        else
        {
            status |= EXIT_FAILURE;
        }
    }

    return status;
}

static CURL* tr_curl_easy_init(struct evbuffer* writebuf, Config& config)
{
    CURL* curl = curl_easy_init();
    (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, fmt::format(FMT_STRING("{:s}/{:s}"), MyName, LONG_VERSION_STRING).c_str());
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunc);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, writebuf);
    (void)curl_easy_setopt(curl, CURLOPT_HEADERDATA, &config);
    (void)curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, parseResponseHeader);
    (void)curl_easy_setopt(curl, CURLOPT_POST, 1);
    (void)curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
    (void)curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    (void)curl_easy_setopt(curl, CURLOPT_VERBOSE, config.debug);
    (void)curl_easy_setopt(
        curl,
        CURLOPT_ENCODING,
        ""); /* "" tells curl to fill in the blanks with what it was compiled to support */

    if (auto const& str = config.unix_socket_path; !std::empty(str))
    {
        (void)curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, str.c_str());
    }

    if (auto const& str = config.netrc; !std::empty(str))
    {
        (void)curl_easy_setopt(curl, CURLOPT_NETRC_FILE, str.c_str());
    }

    if (auto const& str = config.auth; !std::empty(str))
    {
        (void)curl_easy_setopt(curl, CURLOPT_USERPWD, str.c_str());
    }

    if (config.use_ssl)
    {
        // do not verify subject/hostname
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);

        // since most certs will be self-signed, do not verify against CA
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
    }

    if (auto const& str = config.session_id; !std::empty(str))
    {
        auto const h = fmt::format(FMT_STRING("{:s}: {:s}"), TR_RPC_SESSION_ID_HEADER, str);
        auto* const custom_headers = curl_slist_append(nullptr, h.c_str());

        (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, custom_headers);
        (void)curl_easy_setopt(curl, CURLOPT_PRIVATE, custom_headers);
    }

    return curl;
}

static void tr_curl_easy_cleanup(CURL* curl)
{
    struct curl_slist* custom_headers = nullptr;
    curl_easy_getinfo(curl, CURLINFO_PRIVATE, &custom_headers);

    curl_easy_cleanup(curl);

    if (custom_headers != nullptr)
    {
        curl_slist_free_all(custom_headers);
    }
}

static int flush(char const* rpcurl, tr_variant* benc, Config& config)
{
    auto const json = tr_variant_serde::json().compact().to_string(*benc);
    auto const scheme = config.use_ssl ? "https"sv : "http"sv;
    auto const rpcurl_http = fmt::format(FMT_STRING("{:s}://{:s}"), scheme, rpcurl);

    auto* const buf = evbuffer_new();
    auto* curl = tr_curl_easy_init(buf, config);
    (void)curl_easy_setopt(curl, CURLOPT_URL, rpcurl_http.c_str());
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, getTimeoutSecs(json));

    if (config.debug)
    {
        fmt::print(stderr, "posting:\n--------\n{:s}\n--------\n", json);
    }

    auto status = EXIT_SUCCESS;
    if (auto const res = curl_easy_perform(curl); res != CURLE_OK)
    {
        tr_logAddWarn(fmt::format(" ({}) {}", rpcurl_http, curl_easy_strerror(res)));
        status |= EXIT_FAILURE;
    }
    else
    {
        long response;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);

        switch (response)
        {
        case 200:
            status |= processResponse(
                rpcurl,
                std::string_view{ reinterpret_cast<char const*>(evbuffer_pullup(buf, -1)), evbuffer_get_length(buf) },
                config);
            break;

        case 409:
            /* Session id failed. Our curl header func has already
             * pulled the new session id from this response's headers,
             * build a new CURL* and try again */
            tr_curl_easy_cleanup(curl);
            curl = nullptr;
            status |= flush(rpcurl, benc, config);
            break;

        default:
            evbuffer_add(buf, "", 1);
            fmt::print(stderr, "Unexpected response: {:s}\n", reinterpret_cast<char const*>(evbuffer_pullup(buf, -1)));
            status |= EXIT_FAILURE;
            break;
        }
    }

    /* cleanup */
    evbuffer_free(buf);

    if (curl != nullptr)
    {
        tr_curl_easy_cleanup(curl);
    }

    benc->clear();

    return status;
}

static tr_variant* ensure_sset(tr_variant& sset)
{
    if (sset.has_value())
    {
        return tr_variantDictFind(&sset, Arguments);
    }

    sset = tr_variant::make_map(3U);
    tr_variantDictAddStrView(&sset, TR_KEY_method, "session-set"sv);
    return tr_variantDictAddDict(&sset, Arguments, 0);
}

static tr_variant* ensure_tset(tr_variant& tset)
{
    if (tset.has_value())
    {
        return tr_variantDictFind(&tset, Arguments);
    }

    tset = tr_variant::make_map(3U);
    tr_variantDictAddStrView(&tset, TR_KEY_method, "torrent-set"sv);
    return tr_variantDictAddDict(&tset, Arguments, 1);
}

static int processArgs(char const* rpcurl, int argc, char const* const* argv, Config& config)
{
    int status = EXIT_SUCCESS;
    char const* optarg;
    auto sset = tr_variant{};
    auto tset = tr_variant{};
    auto tadd = tr_variant{};
    std::string rename_from;

    for (;;)
    {
        int const c = tr_getopt(Usage, argc, argv, std::data(Options), &optarg);
        if (c == TR_OPT_DONE)
        {
            break;
        }

        int const stepMode = getOptMode(c);
        if (stepMode == 0) /* meta commands */
        {
            switch (c)
            {
            case 'a': /* add torrent */
                if (sset.has_value())
                {
                    status |= flush(rpcurl, &sset, config);
                }

                if (tadd.has_value())
                {
                    status |= flush(rpcurl, &tadd, config);
                }

                if (tset.has_value())
                {
                    addIdArg(tr_variantDictFind(&tset, Arguments), config);
                    status |= flush(rpcurl, &tset, config);
                }

                tadd = tr_variant::make_map(3U);
                tr_variantDictAddStrView(&tadd, TR_KEY_method, "torrent-add"sv);
                tr_variantDictAddInt(&tadd, TR_KEY_tag, TAG_TORRENT_ADD);
                tr_variantDictAddDict(&tadd, Arguments, 0);
                break;

            case 'b': /* debug */
                config.debug = true;
                break;

            case 'j': /* return output as JSON */
                config.json = true;
                break;

            case 968: /* Unix domain socket */
                config.unix_socket_path = optarg;
                break;

            case 'n': /* auth */
                config.auth = optarg;
                break;

            case 810: /* authenv */
                if (auto const authstr = tr_env_get_string("TR_AUTH"); !std::empty(authstr))
                {
                    config.auth = authstr;
                }
                else
                {
                    fmt::print(stderr, "The TR_AUTH environment variable is not set\n");
                    exit(0);
                }

                break;

            case 'N':
                config.netrc = optarg;
                break;

            case 820:
                config.use_ssl = true;
                break;

            case 't': /* set current torrent */
                if (tadd.has_value())
                {
                    status |= flush(rpcurl, &tadd, config);
                }

                if (tset.has_value())
                {
                    addIdArg(tr_variantDictFind(&tset, Arguments), config);
                    status |= flush(rpcurl, &tset, config);
                }

                config.torrent_ids = optarg;
                break;

            case 'V': /* show version number */
                fmt::print(stderr, "{:s} {:s}\n", MyName, LONG_VERSION_STRING);
                exit(0);

            case 944:
                fmt::print("{:s}\n", std::empty(config.torrent_ids) ? "all" : config.torrent_ids.c_str());
                break;

            case TR_OPT_ERR:
                fmt::print(stderr, "invalid option\n");
                showUsage();
                status |= EXIT_FAILURE;
                break;

            case TR_OPT_UNK:
                if (tadd.has_value())
                {
                    tr_variant* args = tr_variantDictFind(&tadd, Arguments);
                    auto const tmp = getEncodedMetainfo(optarg);

                    if (!std::empty(tmp))
                    {
                        tr_variantDictAddStr(args, TR_KEY_metainfo, tmp);
                    }
                    else
                    {
                        tr_variantDictAddStr(args, TR_KEY_filename, optarg);
                    }
                }
                else
                {
                    fmt::print(stderr, "Unknown option: {:s}\n", optarg);
                    status |= EXIT_FAILURE;
                }

                break;
            }
        }
        else if (stepMode == MODE_TORRENT_GET)
        {
            auto top = tr_variant::make_map(3U);
            tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-get"sv);
            tr_variant* args = tr_variantDictAddDict(&top, Arguments, 0);
            tr_variant* fields = tr_variantDictAddList(args, TR_KEY_fields, 0);

            if (tset.has_value())
            {
                addIdArg(tr_variantDictFind(&tset, Arguments), config);
                status |= flush(rpcurl, &tset, config);
            }

            switch (c)
            {
            case 'F':
                config.filter = optarg;
                tr_variantDictAddInt(&top, TR_KEY_tag, TAG_FILTER);

                for (auto const& key : DetailsKeys)
                {
                    tr_variantListAddQuark(fields, key);
                }

                addIdArg(args, config, "all");
                break;
            case 'i':
                tr_variantDictAddInt(&top, TR_KEY_tag, TAG_DETAILS);

                for (auto const& key : DetailsKeys)
                {
                    tr_variantListAddQuark(fields, key);
                }

                addIdArg(args, config);
                break;

            case 'l':
                tr_variantDictAddInt(&top, TR_KEY_tag, TAG_LIST);

                for (auto const& key : ListKeys)
                {
                    tr_variantListAddQuark(fields, key);
                }

                addIdArg(args, config, "all");
                break;

            case 940:
                tr_variantDictAddInt(&top, TR_KEY_tag, TAG_FILES);

                for (auto const& key : FilesKeys)
                {
                    tr_variantListAddQuark(fields, key);
                }

                addIdArg(args, config);
                break;

            case 941:
                tr_variantDictAddInt(&top, TR_KEY_tag, TAG_PEERS);
                tr_variantListAddStrView(fields, "peers"sv);
                addIdArg(args, config);
                break;

            case 942:
                tr_variantDictAddInt(&top, TR_KEY_tag, TAG_PIECES);
                tr_variantListAddStrView(fields, "pieces"sv);
                tr_variantListAddStrView(fields, "pieceCount"sv);
                addIdArg(args, config);
                break;

            case 943:
                tr_variantDictAddInt(&top, TR_KEY_tag, TAG_TRACKERS);
                tr_variantListAddStrView(fields, "trackerStats"sv);
                addIdArg(args, config);
                break;

            default:
                assert("unhandled value" && 0);
            }

            status |= flush(rpcurl, &top, config);
        }
        else if (stepMode == MODE_SESSION_SET)
        {
            tr_variant* args = ensure_sset(sset);

            switch (c)
            {
            case 800:
                tr_variantDictAddStr(args, TR_KEY_script_torrent_done_filename, optarg);
                tr_variantDictAddBool(args, TR_KEY_script_torrent_done_enabled, true);
                break;

            case 801:
                tr_variantDictAddBool(args, TR_KEY_script_torrent_done_enabled, false);
                break;

            case 802:
                tr_variantDictAddStr(args, TR_KEY_script_torrent_done_seeding_filename, optarg);
                tr_variantDictAddBool(args, TR_KEY_script_torrent_done_seeding_enabled, true);
                break;

            case 803:
                tr_variantDictAddBool(args, TR_KEY_script_torrent_done_seeding_enabled, false);
                break;

            case 970:
                tr_variantDictAddBool(args, TR_KEY_alt_speed_enabled, true);
                break;

            case 971:
                tr_variantDictAddBool(args, TR_KEY_alt_speed_enabled, false);
                break;

            case 972:
                tr_variantDictAddInt(args, TR_KEY_alt_speed_down, numarg(optarg));
                break;

            case 973:
                tr_variantDictAddInt(args, TR_KEY_alt_speed_up, numarg(optarg));
                break;

            case 974:
                tr_variantDictAddBool(args, TR_KEY_alt_speed_time_enabled, true);
                break;

            case 975:
                tr_variantDictAddBool(args, TR_KEY_alt_speed_time_enabled, false);
                break;

            case 976:
                addTime(args, TR_KEY_alt_speed_time_begin, optarg);
                break;

            case 977:
                addTime(args, TR_KEY_alt_speed_time_end, optarg);
                break;

            case 978:
                addDays(args, TR_KEY_alt_speed_time_day, optarg);
                break;

            case 'c':
                tr_variantDictAddStr(args, TR_KEY_incomplete_dir, optarg);
                tr_variantDictAddBool(args, TR_KEY_incomplete_dir_enabled, true);
                break;

            case 'C':
                tr_variantDictAddBool(args, TR_KEY_incomplete_dir_enabled, false);
                break;

            case 'e':
                tr_variantDictAddInt(args, TR_KEY_cache_size_mb, atoi(optarg));
                break;

            case 910:
                tr_variantDictAddStrView(args, TR_KEY_encryption, "required"sv);
                break;

            case 911:
                tr_variantDictAddStrView(args, TR_KEY_encryption, "preferred"sv);
                break;

            case 912:
                tr_variantDictAddStrView(args, TR_KEY_encryption, "tolerated"sv);
                break;

            case 'm':
                tr_variantDictAddBool(args, TR_KEY_port_forwarding_enabled, true);
                break;

            case 'M':
                tr_variantDictAddBool(args, TR_KEY_port_forwarding_enabled, false);
                break;

            case 'o':
                tr_variantDictAddBool(args, TR_KEY_dht_enabled, true);
                break;

            case 'O':
                tr_variantDictAddBool(args, TR_KEY_dht_enabled, false);
                break;

            case 830:
                tr_variantDictAddBool(args, TR_KEY_utp_enabled, true);
                break;

            case 831:
                tr_variantDictAddBool(args, TR_KEY_utp_enabled, false);
                break;

            case 'p':
                tr_variantDictAddInt(args, TR_KEY_peer_port, numarg(optarg));
                break;

            case 'P':
                tr_variantDictAddBool(args, TR_KEY_peer_port_random_on_start, true);
                break;

            case 'x':
                tr_variantDictAddBool(args, TR_KEY_pex_enabled, true);
                break;

            case 'X':
                tr_variantDictAddBool(args, TR_KEY_pex_enabled, false);
                break;

            case 'y':
                tr_variantDictAddBool(args, TR_KEY_lpd_enabled, true);
                break;

            case 'Y':
                tr_variantDictAddBool(args, TR_KEY_lpd_enabled, false);
                break;

            case 953:
                tr_variantDictAddReal(args, TR_KEY_seedRatioLimit, atof(optarg));
                tr_variantDictAddBool(args, TR_KEY_seedRatioLimited, true);
                break;

            case 954:
                tr_variantDictAddBool(args, TR_KEY_seedRatioLimited, false);
                break;

            case 990:
                tr_variantDictAddBool(args, TR_KEY_start_added_torrents, false);
                break;

            case 991:
                tr_variantDictAddBool(args, TR_KEY_start_added_torrents, true);
                break;

            case 992:
                tr_variantDictAddBool(args, TR_KEY_trash_original_torrent_files, true);
                break;

            case 993:
                tr_variantDictAddBool(args, TR_KEY_trash_original_torrent_files, false);
                break;

            default:
                assert("unhandled value" && 0);
                break;
            }
        }
        else if (stepMode == (MODE_SESSION_SET | MODE_TORRENT_SET))
        {
            tr_variant* targs = nullptr;
            tr_variant* sargs = nullptr;

            if (!std::empty(config.torrent_ids))
            {
                targs = ensure_tset(tset);
            }
            else
            {
                sargs = ensure_sset(sset);
            }

            switch (c)
            {
            case 'd':
                if (targs != nullptr)
                {
                    tr_variantDictAddInt(targs, TR_KEY_downloadLimit, numarg(optarg));
                    tr_variantDictAddBool(targs, TR_KEY_downloadLimited, true);
                }
                else
                {
                    tr_variantDictAddInt(sargs, TR_KEY_speed_limit_down, numarg(optarg));
                    tr_variantDictAddBool(sargs, TR_KEY_speed_limit_down_enabled, true);
                }

                break;

            case 'D':
                if (targs != nullptr)
                {
                    tr_variantDictAddBool(targs, TR_KEY_downloadLimited, false);
                }
                else
                {
                    tr_variantDictAddBool(sargs, TR_KEY_speed_limit_down_enabled, false);
                }

                break;

            case 'u':
                if (targs != nullptr)
                {
                    tr_variantDictAddInt(targs, TR_KEY_uploadLimit, numarg(optarg));
                    tr_variantDictAddBool(targs, TR_KEY_uploadLimited, true);
                }
                else
                {
                    tr_variantDictAddInt(sargs, TR_KEY_speed_limit_up, numarg(optarg));
                    tr_variantDictAddBool(sargs, TR_KEY_speed_limit_up_enabled, true);
                }

                break;

            case 'U':
                if (targs != nullptr)
                {
                    tr_variantDictAddBool(targs, TR_KEY_uploadLimited, false);
                }
                else
                {
                    tr_variantDictAddBool(sargs, TR_KEY_speed_limit_up_enabled, false);
                }

                break;

            case 930:
                if (targs != nullptr)
                {
                    tr_variantDictAddInt(targs, TR_KEY_peer_limit, atoi(optarg));
                }
                else
                {
                    tr_variantDictAddInt(sargs, TR_KEY_peer_limit_global, atoi(optarg));
                }

                break;

            default:
                assert("unhandled value" && 0);
                break;
            }
        }
        else if (stepMode == MODE_TORRENT_SET)
        {
            tr_variant* args = ensure_tset(tset);

            switch (c)
            {
            case 712:
                {
                    tr_variant* list;
                    if (!tr_variantDictFindList(args, TR_KEY_trackerRemove, &list))
                    {
                        list = tr_variantDictAddList(args, TR_KEY_trackerRemove, 1);
                    }
                    tr_variantListAddInt(list, atoi(optarg));
                    break;
                }

            case 950:
                tr_variantDictAddReal(args, TR_KEY_seedRatioLimit, atof(optarg));
                tr_variantDictAddInt(args, TR_KEY_seedRatioMode, TR_RATIOLIMIT_SINGLE);
                break;

            case 951:
                tr_variantDictAddInt(args, TR_KEY_seedRatioMode, TR_RATIOLIMIT_GLOBAL);
                break;

            case 952:
                tr_variantDictAddInt(args, TR_KEY_seedRatioMode, TR_RATIOLIMIT_UNLIMITED);
                break;

            case 984:
                tr_variantDictAddBool(args, TR_KEY_honorsSessionLimits, true);
                break;

            case 985:
                tr_variantDictAddBool(args, TR_KEY_honorsSessionLimits, false);
                break;

            default:
                assert("unhandled value" && 0);
                break;
            }
        }
        else if (stepMode == (MODE_TORRENT_SET | MODE_TORRENT_ADD))
        {
            tr_variant* args;

            if (tadd.has_value())
            {
                args = tr_variantDictFind(&tadd, Arguments);
            }
            else
            {
                args = ensure_tset(tset);
            }

            switch (c)
            {
            case 'g':
                addFiles(args, TR_KEY_files_wanted, optarg);
                break;

            case 'G':
                addFiles(args, TR_KEY_files_unwanted, optarg);
                break;

            case 'L':
                addLabels(args, optarg ? optarg : "");
                break;

            case 730:
                setGroup(args, optarg ? optarg : "");
                break;

            case 731:
                setGroup(args, "");
                break;

            case 900:
                addFiles(args, TR_KEY_priority_high, optarg);
                break;

            case 901:
                addFiles(args, TR_KEY_priority_normal, optarg);
                break;

            case 902:
                addFiles(args, TR_KEY_priority_low, optarg);
                break;

            case 700:
                tr_variantDictAddInt(args, TR_KEY_bandwidthPriority, 1);
                break;

            case 701:
                tr_variantDictAddInt(args, TR_KEY_bandwidthPriority, 0);
                break;

            case 702:
                tr_variantDictAddInt(args, TR_KEY_bandwidthPriority, -1);
                break;

            case 710:
                {
                    tr_variant* list;
                    if (!tr_variantDictFindList(args, TR_KEY_trackerAdd, &list))
                    {
                        list = tr_variantDictAddList(args, TR_KEY_trackerAdd, 1);
                    }
                    tr_variantListAddStr(list, optarg);
                    break;
                }

            default:
                assert("unhandled value" && 0);
                break;
            }
        }
        else if (c == 961) /* set location */
        {
            if (tadd.has_value())
            {
                tr_variant* args = tr_variantDictFind(&tadd, Arguments);
                tr_variantDictAddStr(args, TR_KEY_download_dir, optarg);
            }
            else
            {
                auto top = tr_variant::make_map(2U);
                tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-set-location"sv);
                tr_variant* args = tr_variantDictAddDict(&top, Arguments, 3);
                tr_variantDictAddStr(args, TR_KEY_location, optarg);
                tr_variantDictAddBool(args, TR_KEY_move, false);
                addIdArg(args, config);
                status |= flush(rpcurl, &top, config);
                break;
            }
        }
        else
        {
            switch (c)
            {
            case 920: /* session-info */
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "session-get"sv);
                    tr_variantDictAddInt(&top, TR_KEY_tag, TAG_SESSION);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 's': /* start */
                if (tadd.has_value())
                {
                    tr_variantDictAddBool(tr_variantDictFind(&tadd, TR_KEY_arguments), TR_KEY_paused, false);
                }
                else
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-start"sv);
                    addIdArg(tr_variantDictAddDict(&top, Arguments, 1), config);
                    status |= flush(rpcurl, &top, config);
                }
                break;

            case 'S': /* stop */
                if (tadd.has_value())
                {
                    tr_variantDictAddBool(tr_variantDictFind(&tadd, TR_KEY_arguments), TR_KEY_paused, true);
                }
                else
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-stop"sv);
                    addIdArg(tr_variantDictAddDict(&top, Arguments, 1), config);
                    status |= flush(rpcurl, &top, config);
                }

                break;

            case 'w':
                {
                    auto* args = tadd.has_value() ? tr_variantDictFind(&tadd, TR_KEY_arguments) : ensure_sset(sset);
                    tr_variantDictAddStr(args, TR_KEY_download_dir, optarg);
                    break;
                }

            case 850:
                {
                    auto top = tr_variant::make_map(1U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "session-close"sv);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 963:
                {
                    auto top = tr_variant::make_map(1U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "blocklist-update"sv);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 921:
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "session-stats"sv);
                    tr_variantDictAddInt(&top, TR_KEY_tag, TAG_STATS);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 962:
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "port-test"sv);
                    tr_variantDictAddInt(&top, TR_KEY_tag, TAG_PORTTEST);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 600:
                {
                    if (tset.has_value())
                    {
                        addIdArg(tr_variantDictFind(&tset, Arguments), config);
                        status |= flush(rpcurl, &tset, config);
                    }

                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-reannounce"sv);
                    addIdArg(tr_variantDictAddDict(&top, Arguments, 1), config);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 'v':
                {
                    if (tset.has_value())
                    {
                        addIdArg(tr_variantDictFind(&tset, Arguments), config);
                        status |= flush(rpcurl, &tset, config);
                    }

                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-verify"sv);
                    addIdArg(tr_variantDictAddDict(&top, Arguments, 1), config);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 'r':
            case 840:
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-remove"sv);
                    auto* args = tr_variantDictAddDict(&top, Arguments, 2);
                    tr_variantDictAddBool(args, TR_KEY_delete_local_data, c == 840);
                    addIdArg(args, config);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 960:
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-set-location"sv);
                    auto* args = tr_variantDictAddDict(&top, Arguments, 3);
                    tr_variantDictAddStr(args, TR_KEY_location, optarg);
                    tr_variantDictAddBool(args, TR_KEY_move, true);
                    addIdArg(args, config);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            case 964:
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStr(&top, TR_KEY_method, "torrent-rename-path"sv);
                    auto* args = tr_variantDictAddDict(&top, Arguments, 3);
                    tr_variantDictAddStr(args, TR_KEY_path, rename_from);
                    tr_variantDictAddStr(args, TR_KEY_name, optarg);
                    addIdArg(args, config);
                    status |= flush(rpcurl, &top, config);
                    rename_from.clear();
                    break;
                }

            case 965:
                {
                    rename_from = optarg;
                    break;
                }

            case 732:
                {
                    auto top = tr_variant::make_map(2U);
                    tr_variantDictAddStrView(&top, TR_KEY_method, "group-get"sv);
                    tr_variantDictAddInt(&top, TR_KEY_tag, TAG_GROUPS);
                    status |= flush(rpcurl, &top, config);
                    break;
                }

            default:
                fmt::print(stderr, "got opt [{:d}]\n", c);
                showUsage();
                break;
            }
        }
    }

    if (tadd.has_value())
    {
        status |= flush(rpcurl, &tadd, config);
    }

    if (tset.has_value())
    {
        addIdArg(tr_variantDictFind(&tset, Arguments), config);
        status |= flush(rpcurl, &tset, config);
    }

    if (sset.has_value())
    {
        status |= flush(rpcurl, &sset, config);
    }

    return status;
}

static bool parsePortString(char const* s, int* port)
{
    int const errno_stack = errno;
    errno = 0;

    char* end = nullptr;
    auto const i = int(strtol(s, &end, 10));
    bool const ok = (end != nullptr) && (*end == '\0') && (errno == 0);
    if (ok)
    {
        *port = i;
    }

    errno = errno_stack;
    return ok;
}

/* [host:port] or [host] or [port] or [http(s?)://host:port/transmission/] */
static void getHostAndPortAndRpcUrl(int* argc, char** argv, std::string* host, int* port, std::string* rpcurl, Config& config)
{
    if (*argv[1] == '-')
    {
        return;
    }

    char const* const s = argv[1];
    char const* const last_colon = strrchr(s, ':');

    if (strncmp(s, "http://", 7) == 0) /* user passed in http rpc url */
    {
        *rpcurl = fmt::format(FMT_STRING("{:s}/rpc/"), s + 7);
    }
    else if (strncmp(s, "https://", 8) == 0) /* user passed in https rpc url */
    {
        config.use_ssl = true;
        *rpcurl = fmt::format(FMT_STRING("{:s}/rpc/"), s + 8);
    }
    else if (parsePortString(s, port))
    {
        // it was just a port
    }
    else if (last_colon == nullptr)
    {
        // it was a non-ipv6 host with no port
        *host = s;
    }
    else
    {
        char const* hend;

        // if only one colon, it's probably "$host:$port"
        if ((strchr(s, ':') == last_colon) && parsePortString(last_colon + 1, port))
        {
            hend = last_colon;
        }
        else
        {
            hend = s + strlen(s);
        }

        bool const is_unbracketed_ipv6 = (*s != '[') && (memchr(s, ':', hend - s) != nullptr);

        auto const sv = std::string_view{ s, size_t(hend - s) };
        *host = is_unbracketed_ipv6 ? fmt::format(FMT_STRING("[{:s}]"), sv) : sv;
    }

    *argc -= 1;

    for (int i = 1; i < *argc; ++i)
    {
        argv[i] = argv[i + 1];
    }
}

int tr_main(int argc, char* argv[])
{
    auto const init_mgr = tr_lib_init();

    tr_locale_set_global("");

    auto config = Config{};
    auto port = DefaultPort;
    auto host = std::string{};
    auto rpcurl = std::string{};

    if (argc < 2)
    {
        showUsage();
        return EXIT_FAILURE;
    }

    tr_formatter_mem_init(MemK, MemKStr, MemMStr, MemGStr, MemTStr);
    tr_formatter_size_init(DiskK, DiskKStr, DiskMStr, DiskGStr, DiskTStr);
    tr_formatter_speed_init(SpeedK, SpeedKStr, SpeedMStr, SpeedGStr, SpeedTStr);

    getHostAndPortAndRpcUrl(&argc, argv, &host, &port, &rpcurl, config);

    if (std::empty(host))
    {
        host = DefaultHost;
    }

    if (std::empty(rpcurl))
    {
        rpcurl = fmt::format(FMT_STRING("{:s}:{:d}{:s}"), host, port, DefaultUrl);
    }

    return processArgs(rpcurl.c_str(), argc, (char const* const*)argv, config);
}
