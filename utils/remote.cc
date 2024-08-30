// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cctype> /* isspace */
#include <cmath> // floor
#include <cstdint> // int64_t
#include <cstdio>
#include <cstdlib>
#include <cstring> /* strcmp */
#include <ctime>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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
#include <libtransmission/tr-assert.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h>
#include <libtransmission/values.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>

using namespace std::literals;

using namespace libtransmission::Values;

#define SPEED_K_STR "kB/s"
#define MEM_M_STR "MiB"

namespace
{
auto constexpr DefaultPort = uint16_t{ TR_DEFAULT_RPC_PORT };
char constexpr DefaultHost[] = "localhost";
char constexpr DefaultUrl[] = TR_DEFAULT_RPC_URL_STR "rpc/";

char constexpr MyName[] = "transmission-remote";
char constexpr Usage[] = "transmission-remote " LONG_VERSION_STRING
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

struct RemoteConfig
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

// --- Display Utilities

[[nodiscard]] std::string eta_to_string(int64_t eta)
{
    if (eta < 0)
    {
        return "Unknown"s;
    }

    if (eta < 60)
    {
        return fmt::format("{:d} sec", eta);
    }

    if (eta < (60 * 60))
    {
        return fmt::format("{:d} min", eta / 60);
    }

    if (eta < (60 * 60 * 24))
    {
        return fmt::format("{:d} hrs", eta / (60 * 60));
    }

    if (eta < (60 * 60 * 24 * 30))
    {
        return fmt::format("{:d} days", eta / (60 * 60 * 24));
    }

    if (eta < (60 * 60 * 24 * 30 * 12))
    {
        return fmt::format("{:d} months", eta / (60 * 60 * 24 * 30));
    }

    if (eta < (60 * 60 * 24 * 365 * 1000LL)) // up to 999 years
    {
        return fmt::format("{:d} years", eta / (60 * 60 * 24 * 365));
    }

    return "∞"s;
}

[[nodiscard]] auto tr_strltime(time_t seconds)
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

    auto const hstr = fmt::format("{:d} {:s}", hours, tr_ngettext("hour", "hours", hours));
    auto const mstr = fmt::format("{:d} {:s}", minutes, tr_ngettext("minute", "minutes", minutes));
    auto const sstr = fmt::format("{:d} {:s}", seconds, tr_ngettext("seconds", "seconds", seconds));

    if (days > 0)
    {
        auto const dstr = fmt::format("{:d} {:s}", days, tr_ngettext("day", "days", days));
        tmpstr = days >= 4 || hours == 0 ? dstr : fmt::format("{:s}, {:s}", dstr, hstr);
    }
    else if (hours > 0)
    {
        tmpstr = hours >= 4 || minutes == 0 ? hstr : fmt::format("{:s}, {:s}", hstr, mstr);
    }
    else if (minutes > 0)
    {
        tmpstr = minutes >= 4 || seconds == 0 ? mstr : fmt::format("{:s}, {:s}", mstr, sstr);
    }
    else
    {
        tmpstr = sstr;
    }

    auto const totstr = fmt::format("{:d} {:s}", total_seconds, tr_ngettext("seconds", "seconds", total_seconds));
    return fmt::format("{:s} ({:s})", tmpstr, totstr);
}

[[nodiscard]] auto strlpercent(double x)
{
    return tr_strpercent(x);
}

[[nodiscard]] auto strlratio2(double ratio)
{
    return tr_strratio(ratio, "Inf");
}

[[nodiscard]] auto strlratio(int64_t numerator, int64_t denominator)
{
    return strlratio2(tr_getRatio(numerator, denominator));
}

[[nodiscard]] auto strlsize(int64_t bytes)
{
    if (bytes < 0)
    {
        return "Unknown"s;
    }

    if (bytes == 0)
    {
        return "None"s;
    }

    return Storage{ bytes, Storage::Units::Bytes }.to_string();
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

// --- Command-Line Arguments

auto constexpr Options = std::array<tr_option, 103>{
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
      { 955,
        "idle-seeding-limit",
        "Let the current torrent(s) seed until a specific amount idle time",
        "isl",
        true,
        "<minutes>" },
      { 956,
        "default-idle-seeding-limit",
        "Let the current torrent(s) use the default idle seeding settings",
        "isld",
        false,
        nullptr },
      { 957, "no-idle-seeding-limit", "Let the current torrent(s) seed regardless of idle time", "ISL", false, nullptr },
      { 958,
        "global-idle-seeding-limit",
        "All torrents, unless overridden by a per-torrent setting, should seed until a specific amount of idle time",
        "gisl",
        true,
        "<minutes>" },
      { 959,
        "no-global-idle-seeding-limit",
        "All torrents, unless overridden by a per-torrent setting, should seed regardless of idle time",
        "GISL",
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
static_assert(Options[std::size(Options) - 2].val != 0);

void show_usage()
{
    tr_getopt_usage(MyName, Usage, std::data(Options));
}

[[nodiscard]] auto numarg(std::string_view arg)
{
    auto remainder = std::string_view{};
    auto const num = tr_num_parse<int64_t>(arg, &remainder);
    if (!num || !std::empty(remainder))
    {
        fmt::print(stderr, "Not a number: '{:s}'\n", arg);
        show_usage();
        exit(EXIT_FAILURE);
    }

    return *num;
}

enum
{
    MODE_META_COMMAND = 0,
    MODE_TORRENT_ACTION = 1 << 0,
    MODE_TORRENT_ADD = 1 << 1,
    MODE_TORRENT_GET = 1 << 2,
    MODE_TORRENT_REMOVE = 1 << 3,
    MODE_TORRENT_SET = 1 << 4,
    MODE_TORRENT_START_STOP = 1 << 5,
    MODE_SESSION_SET = 1 << 6
};

[[nodiscard]] int get_opt_mode(int val)
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
        return MODE_META_COMMAND;

    case 'c': /* incomplete-dir */
    case 'C': /* no-incomplete-dir */
    case 'e': /* cache */
    case 'm': /* portmap */
    case 'M': /* no-portmap */
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
    case 958: /* global-idle-seeding-limit */
    case 959: /* no-global-idle-seeding-limit */
    case 990: /* start-paused */
    case 991: /* no-start-paused */
    case 992: /* trash-torrent */
    case 993: /* no-trash-torrent */
        return MODE_SESSION_SET;

    case 712: /* tracker-remove */
    case 950: /* seedratio */
    case 951: /* seedratio-default */
    case 952: /* no-seedratio */
    case 955: /* idle-seeding-limit */
    case 956: /* default-idle-seeding-limit */
    case 957: /* no-idle-seeding-limit*/
    case 984: /* honor-session */
    case 985: /* no-honor-session */
        return MODE_TORRENT_SET;

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

    case 'r': /* remove */
    case 840: /* remove and delete */
        return MODE_TORRENT_REMOVE;

    case 's': /* start */
    case 'S': /* stop */
        return MODE_TORRENT_START_STOP;

    case 'v': /* verify */
    case 600: /* reannounce */
        return MODE_TORRENT_ACTION;

    case 'w': /* download-dir */
    case 850: /* session-close */
    case 732: /* List groups */
    case 920: /* session-info */
    case 921: /* session-stats */
    case 960: /* move */
    case 961: /* find */
    case 962: /* port-test */
    case 963: /* blocklist-update */
    case 964: /* rename */
    case 965: /* path */
        return -1;

    default:
        fmt::print(stderr, "unrecognized argument {:d}\n", val);
        TR_ASSERT_MSG(false, "unrecognized argument");
        return -2;
    }
}

[[nodiscard]] std::string get_encoded_metainfo(char const* filename)
{
    if (auto contents = std::vector<char>{}; tr_sys_path_exists(filename) && tr_file_read(filename, contents))
    {
        return tr_base64_encode({ std::data(contents), std::size(contents) });
    }

    return {};
}

void add_id_arg(tr_variant::Map& args, std::string_view id_str, std::string_view fallback = "")
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
        args.insert_or_assign(TR_KEY_ids, "recently-active"sv);
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
            args.insert_or_assign(TR_KEY_ids, tr_rpc_parse_list_str(id_str));
        }
        else
        {
            args.insert_or_assign(TR_KEY_ids, id_str); /* it's a torrent sha hash */
        }
    }
}

void add_id_arg(tr_variant::Map& args, RemoteConfig const& config, std::string_view fallback = "")
{
    return add_id_arg(args, config.torrent_ids, fallback);
}

void add_time(tr_variant::Map& args, tr_quark const key, std::string_view arg)
{
    if (std::size(arg) == 4)
    {
        auto const hour = tr_num_parse<int>(arg.substr(0, 2)).value_or(-1);
        auto const min = tr_num_parse<int>(arg.substr(2, 2)).value_or(-1);

        if (0 <= hour && hour < 24 && 0 <= min && min < 60)
        {
            args.insert_or_assign(key, min + hour * 60);
            return;
        }
    }

    fmt::print(stderr, "Please specify the time of day in 'hhmm' format.\n");
}

void add_days(tr_variant::Map& args, tr_quark const key, std::string_view arg)
{
    int days = 0;

    if (!std::empty(arg))
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
        args.insert_or_assign(key, days);
    }
    else
    {
        fmt::print(stderr, "Please specify the days of the week in '1-3,4,7' format.\n");
    }
}

void add_labels(tr_variant::Map& args, std::string_view comma_delimited_labels)
{
    auto* labels = args.find_if<tr_variant::Vector>(TR_KEY_labels);
    if (labels == nullptr)
    {
        labels = args.insert_or_assign(TR_KEY_labels, tr_variant::make_vector(10)).first.get_if<tr_variant::Vector>();
    }

    auto label = std::string_view{};
    while (tr_strv_sep(&comma_delimited_labels, &label, ','))
    {
        labels->emplace_back(label);
    }
}

void set_group(tr_variant::Map& args, std::string_view group)
{
    args.insert_or_assign(TR_KEY_group, tr_variant::unmanaged_string(group));
}

[[nodiscard]] auto make_files_list(std::string_view str_in)
{
    if (std::empty(str_in))
    {
        fmt::print(stderr, "No files specified!\n");
        str_in = "-1"sv; // no file will have this index, so should be a no-op
    }

    auto files = tr_variant::Vector{};

    if (str_in != "all"sv)
    {
        files.reserve(100U);
        for (auto const& idx : tr_num_parse_range(str_in))
        {
            files.emplace_back(idx);
        }
    }

    return files;
}

auto constexpr FilesKeys = std::array<tr_quark, 4>{
    TR_KEY_files,
    TR_KEY_name,
    TR_KEY_priorities,
    TR_KEY_wanted,
};
static_assert(FilesKeys[std::size(FilesKeys) - 1] != tr_quark{});

auto constexpr DetailsKeys = std::array<tr_quark, 55>{
    TR_KEY_activity_date_camel,
    TR_KEY_added_date_camel,
    TR_KEY_bandwidth_priority_camel,
    TR_KEY_comment,
    TR_KEY_corrupt_ever_camel,
    TR_KEY_creator,
    TR_KEY_date_created_camel,
    TR_KEY_desired_available_camel,
    TR_KEY_done_date_camel,
    TR_KEY_download_dir_camel,
    TR_KEY_downloaded_ever_camel,
    TR_KEY_download_limit_camel,
    TR_KEY_download_limited_camel,
    TR_KEY_error,
    TR_KEY_error_string_camel,
    TR_KEY_eta,
    TR_KEY_group,
    TR_KEY_hash_string_camel,
    TR_KEY_have_unchecked_camel,
    TR_KEY_have_valid_camel,
    TR_KEY_honors_session_limits_camel,
    TR_KEY_id,
    TR_KEY_is_finished_camel,
    TR_KEY_is_private_camel,
    TR_KEY_labels,
    TR_KEY_left_until_done_camel,
    TR_KEY_magnet_link_camel,
    TR_KEY_name,
    TR_KEY_peers_connected_camel,
    TR_KEY_peers_getting_from_us_camel,
    TR_KEY_peers_sending_to_us_camel,
    TR_KEY_peer_limit_kebab,
    TR_KEY_piece_count_camel,
    TR_KEY_piece_size_camel,
    TR_KEY_rate_download_camel,
    TR_KEY_rate_upload_camel,
    TR_KEY_recheck_progress_camel,
    TR_KEY_seconds_downloading_camel,
    TR_KEY_seconds_seeding_camel,
    TR_KEY_seed_idle_mode_camel,
    TR_KEY_seed_idle_limit_camel,
    TR_KEY_seed_ratio_mode_camel,
    TR_KEY_seed_ratio_limit_camel,
    TR_KEY_sequential_download,
    TR_KEY_size_when_done_camel,
    TR_KEY_source,
    TR_KEY_start_date_camel,
    TR_KEY_status,
    TR_KEY_total_size_camel,
    TR_KEY_uploaded_ever_camel,
    TR_KEY_upload_limit_camel,
    TR_KEY_upload_limited_camel,
    TR_KEY_upload_ratio_camel,
    TR_KEY_webseeds,
    TR_KEY_webseeds_sending_to_us_camel,
};
static_assert(DetailsKeys[std::size(DetailsKeys) - 1] != tr_quark{});

auto constexpr ListKeys = std::array<tr_quark, 15>{
    TR_KEY_added_date_camel,
    TR_KEY_error,
    TR_KEY_error_string_camel,
    TR_KEY_eta,
    TR_KEY_id,
    TR_KEY_is_finished_camel,
    TR_KEY_left_until_done_camel,
    TR_KEY_name,
    TR_KEY_peers_getting_from_us_camel,
    TR_KEY_peers_sending_to_us_camel,
    TR_KEY_rate_download_camel,
    TR_KEY_rate_upload_camel,
    TR_KEY_size_when_done_camel,
    TR_KEY_status,
    TR_KEY_upload_ratio_camel,
};
static_assert(ListKeys[std::size(ListKeys) - 1] != tr_quark{});

[[nodiscard]] size_t write_func(void* ptr, size_t size, size_t nmemb, void* vbuf)
{
    auto* const buf = static_cast<evbuffer*>(vbuf);
    size_t const byteCount = size * nmemb;
    evbuffer_add(buf, ptr, byteCount);
    return byteCount;
}

/* look for a session id in the header in case the server gives back a 409 */
[[nodiscard]] size_t parse_response_header(void* ptr, size_t size, size_t nmemb, void* vconfig)
{
    auto& config = *static_cast<RemoteConfig*>(vconfig);
    auto const* const line = static_cast<char const*>(ptr);
    size_t const line_len = size * nmemb;
    char const* key = TR_RPC_SESSION_ID_HEADER ": ";
    size_t const key_len = strlen(key);

    if (line_len >= key_len && evutil_ascii_strncasecmp(line, key, key_len) == 0)
    {
        char const* begin = line + key_len;
        char const* end = std::find_if(begin, line + line_len, [](char c) { return isspace(c); });

        config.session_id.assign(begin, end - begin);
    }

    return line_len;
}

[[nodiscard]] long get_timeout_secs(std::string_view req)
{
    if (req.find(R"("method":"blocklist-update")") != std::string_view::npos)
    {
        return 300L;
    }

    return 60L; /* default value */
}

[[nodiscard]] std::string get_status_string(tr_variant::Map const& t)
{
    auto const status = t.value_if<int64_t>(TR_KEY_status);
    if (!status)
    {
        return ""s;
    }

    switch (*status)
    {
    case TR_STATUS_DOWNLOAD_WAIT:
    case TR_STATUS_SEED_WAIT:
        return "Queued"s;

    case TR_STATUS_STOPPED:
        if (t.value_if<bool>({ TR_KEY_is_finished, TR_KEY_is_finished_camel }).value_or(false))
        {
            return "Finished"s;
        }
        return "Stopped"s;

    case TR_STATUS_CHECK_WAIT:
        if (auto percent = t.value_if<double>({ TR_KEY_recheck_progress, TR_KEY_recheck_progress_camel }); percent)
        {
            return fmt::format("Will Verify ({:.0f}%)", floor(*percent * 100.0));
        }
        return "Will Verify"s;

    case TR_STATUS_CHECK:
        if (auto percent = t.value_if<double>({ TR_KEY_recheck_progress, TR_KEY_recheck_progress_camel }); percent)
        {
            return fmt::format("Verifying ({:.0f}%)", floor(*percent * 100.0));
        }
        return "Verifying"s;

    case TR_STATUS_DOWNLOAD:
    case TR_STATUS_SEED:
        if (auto from_us = t.value_if<int64_t>({ TR_KEY_peers_getting_from_us, TR_KEY_peers_getting_from_us_camel })
                               .value_or(0),
            to_us = t.value_if<int64_t>({ TR_KEY_peers_sending_to_us, TR_KEY_peers_sending_to_us_camel }).value_or(0);
            from_us != 0 && to_us != 0)
        {
            return "Up & Down"s;
        }
        else if (to_us != 0)
        {
            return "Downloading"s;
        }
        else if (from_us == 0)
        {
            return "Idle"s;
        }
        if (auto left_until_done = t.value_if<int64_t>({ TR_KEY_left_until_done, TR_KEY_left_until_done_camel }).value_or(0);
            left_until_done > 0)
        {
            return "Uploading"s;
        }
        return "Seeding"s;

    default:
        return "Unknown"s;
    }
}

auto constexpr BandwidthPriorityNames = std::array<std::string_view, 4>{
    "Low"sv,
    "Normal"sv,
    "High"sv,
    "Invalid"sv,
};
static_assert(!BandwidthPriorityNames[std::size(BandwidthPriorityNames) - 1].empty());

template<size_t N>
std::string_view format_date(std::array<char, N>& buf, time_t now)
{
    auto begin = std::data(buf);
    auto end = fmt::format_to_n(begin, N, "{:%a %b %d %T %Y}", fmt::localtime(now)).out;
    return { begin, static_cast<size_t>(end - begin) };
}

void print_details(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    auto* const torrents = args->find_if<tr_variant::Vector>(TR_KEY_torrents);
    if (torrents == nullptr)
    {
        return;
    }

    for (auto const& t_var : *torrents)
    {
        auto* t = t_var.get_if<tr_variant::Map>();
        if (t == nullptr)
        {
            continue;
        }

        std::array<char, 512> buf = {};

        fmt::print("NAME\n");

        if (auto i = t->value_if<int64_t>(TR_KEY_id); i)
        {
            fmt::print("  Id: {:d}\n", *i);
        }

        if (auto sv = t->value_if<std::string_view>(TR_KEY_name); sv)
        {
            fmt::print("  Name: {:s}\n", *sv);
        }

        if (auto sv = t->value_if<std::string_view>({ TR_KEY_hash_string, TR_KEY_hash_string_camel }); sv)
        {
            fmt::print("  Hash: {:s}\n", *sv);
        }

        if (auto sv = t->value_if<std::string_view>({ TR_KEY_magnet_link, TR_KEY_magnet_link_camel }); sv)
        {
            fmt::print("  Magnet: {:s}\n", *sv);
        }

        if (auto* l = t->find_if<tr_variant::Vector>(TR_KEY_labels); l != nullptr)
        {
            fmt::print("  Labels: ");

            for (auto it = std::begin(*l), begin = std::begin(*l), end = std::end(*l); it != end; ++it)
            {
                if (auto sv = it->value_if<std::string_view>(); sv)
                {
                    fmt::print(it == begin ? "{:s}" : ", {:s}", *sv);
                }
            }

            fmt::print("\n");
        }

        if (auto sv = t->value_if<std::string_view>(TR_KEY_group).value_or(""sv); !std::empty(sv))
        {
            fmt::print("  Bandwidth group: {:s}\n", sv);
        }

        fmt::print("\n");

        fmt::print("TRANSFER\n");
        fmt::print("  State: {:s}\n", get_status_string(*t));

        if (auto sv = t->value_if<std::string_view>({ TR_KEY_download_dir, TR_KEY_download_dir_camel }); sv)
        {
            fmt::print("  Location: {:s}\n", *sv);
        }

        if (auto b = t->value_if<bool>(TR_KEY_sequential_download); b)
        {
            fmt::print("  Sequential Download: {:s}\n", *b ? "Yes" : "No");
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_size_when_done, TR_KEY_size_when_done_camel }),
            j = t->value_if<int64_t>({ TR_KEY_left_until_done, TR_KEY_left_until_done_camel });
            i && j)
        {
            fmt::print("  Percent Done: {:s}%\n", strlpercent(100.0 * (*i - *j) / *i));
        }

        if (auto i = t->value_if<int64_t>(TR_KEY_eta); i)
        {
            fmt::print("  ETA: {:s}\n", tr_strltime(*i));
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_rate_download, TR_KEY_rate_download_camel }); i)
        {
            fmt::print("  Download Speed: {:s}\n", Speed{ *i, Speed::Units::Byps }.to_string());
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_rate_upload, TR_KEY_rate_upload_camel }); i)
        {
            fmt::print("  Upload Speed: {:s}\n", Speed{ *i, Speed::Units::Byps }.to_string());
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_have_unchecked, TR_KEY_have_unchecked_camel }),
            j = t->value_if<int64_t>({ TR_KEY_have_valid, TR_KEY_have_valid_camel });
            i && j)
        {
            fmt::print("  Have: {:s} ({:s} verified)\n", strlsize(*i + *j), strlsize(*j));
        }

        if (auto oi = t->value_if<int64_t>({ TR_KEY_size_when_done, TR_KEY_size_when_done_camel }); oi)
        {
            auto const i = *oi;
            if (i < 1)
            {
                fmt::print("  Availability: None\n");
            }
            else if (auto j = t->value_if<int64_t>({ TR_KEY_desired_available, TR_KEY_desired_available_camel }),
                     k = t->value_if<int64_t>({ TR_KEY_left_until_done, TR_KEY_left_until_done_camel });
                     j && k)
            {
                fmt::print("  Availability: {:s}%\n", strlpercent(100.0 * (*j + i - *k) / i));
            }

            if (auto j = t->value_if<int64_t>({ TR_KEY_total_size, TR_KEY_total_size_camel }); j)
            {
                fmt::print("  Total size: {:s} ({:s} wanted)\n", strlsize(*j), strlsize(i));
            }
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_downloaded_ever, TR_KEY_downloaded_ever_camel }); i)
        {
            if (auto corrupt = t->value_if<int64_t>({ TR_KEY_corrupt_ever, TR_KEY_corrupt_ever_camel }).value_or(0);
                corrupt != 0)
            {
                fmt::print("  Downloaded: {:s} (+{:s} discarded after failed checksum)\n", strlsize(*i), strlsize(corrupt));
            }
            else
            {
                fmt::print("  Downloaded: {:s}\n", strlsize(*i));
            }
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_uploaded_ever, TR_KEY_uploaded_ever_camel }); i)
        {
            fmt::print("  Uploaded: {:s}\n", strlsize(*i));

            if (auto j = t->value_if<int64_t>({ TR_KEY_size_when_done, TR_KEY_size_when_done_camel }); j)
            {
                fmt::print("  Ratio: {:s}\n", strlratio(*i, *j));
            }
        }

        if (auto i = t->value_if<int64_t>(TR_KEY_error).value_or(0); i != 0)
        {
            if (auto sv = t->value_if<std::string_view>({ TR_KEY_error_string, TR_KEY_error_string_camel }).value_or(""sv);
                !std::empty(sv))
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
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_peers_connected, TR_KEY_peers_connected_camel }),
            j = t->value_if<int64_t>({ TR_KEY_peers_getting_from_us, TR_KEY_peers_getting_from_us_camel }),
            k = t->value_if<int64_t>({ TR_KEY_peers_sending_to_us, TR_KEY_peers_sending_to_us_camel });
            i && j && k)
        {
            fmt::print("  Peers: connected to {:d}, uploading to {:d}, downloading from {:d}\n", *i, *j, *k);
        }

        if (auto* l = t->find_if<tr_variant::Vector>(TR_KEY_webseeds); l != nullptr)
        {
            if (auto const n = std::size(*l); n > 0)
            {
                if (auto i = t->value_if<int64_t>({ TR_KEY_webseeds_sending_to_us, TR_KEY_webseeds_sending_to_us_camel }); i)
                {
                    fmt::print("  Web Seeds: downloading from {:d} of {:d} web seeds\n", *i, n);
                }
            }
        }

        fmt::print("\n");

        fmt::print("HISTORY\n");

        if (auto i = t->value_if<int64_t>({ TR_KEY_added_date, TR_KEY_added_date_camel }).value_or(0); i != 0)
        {
            fmt::print("  Date added:       {:s}\n", format_date(buf, i));
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_done_date, TR_KEY_done_date_camel }).value_or(0); i != 0)
        {
            fmt::print("  Date finished:    {:s}\n", format_date(buf, i));
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_start_date, TR_KEY_start_date_camel }).value_or(0); i != 0)
        {
            fmt::print("  Date started:     {:s}\n", format_date(buf, i));
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_activity_date, TR_KEY_activity_date_camel }).value_or(0); i != 0)
        {
            fmt::print("  Latest activity:  {:s}\n", format_date(buf, i));
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_seconds_downloading, TR_KEY_seconds_downloading_camel }).value_or(0); i > 0)
        {
            fmt::print("  Downloading Time: {:s}\n", tr_strltime(i));
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_seconds_seeding, TR_KEY_seconds_seeding_camel }).value_or(0); i > 0)
        {
            fmt::print("  Seeding Time:     {:s}\n", tr_strltime(i));
        }

        fmt::print("\n");

        fmt::print("ORIGINS\n");

        if (auto i = t->value_if<int64_t>({ TR_KEY_date_created, TR_KEY_date_created_camel }).value_or(0); i != 0)
        {
            fmt::print("  Date created: {:s}\n", format_date(buf, i));
        }

        if (auto b = t->value_if<bool>({ TR_KEY_is_private, TR_KEY_is_private_camel }); b)
        {
            fmt::print("  Public torrent: {:s}\n", *b ? "No" : "Yes");
        }

        if (auto sv = t->value_if<std::string_view>(TR_KEY_comment).value_or(""sv); !std::empty(sv))
        {
            fmt::print("  Comment: {:s}\n", sv);
        }

        if (auto sv = t->value_if<std::string_view>(TR_KEY_creator).value_or(""sv); !std::empty(sv))
        {
            fmt::print("  Creator: {:s}\n", sv);
        }

        if (auto sv = t->value_if<std::string_view>(TR_KEY_source).value_or(""sv); !std::empty(sv))
        {
            fmt::print("  Source: {:s}\n", sv);
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_piece_count, TR_KEY_piece_count_camel }); i)
        {
            fmt::print("  Piece Count: {:d}\n", *i);
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_piece_size, TR_KEY_piece_size_camel }); i)
        {
            fmt::print("  Piece Size: {:s}\n", Memory{ *i, Memory::Units::Bytes }.to_string());
        }

        fmt::print("\n");

        fmt::print("LIMITS & BANDWIDTH\n");

        if (auto b = t->value_if<bool>({ TR_KEY_download_limited, TR_KEY_download_limited_camel }); b)
        {
            if (auto i = t->value_if<int64_t>({ TR_KEY_download_limit, TR_KEY_download_limit_camel }); i)
            {
                fmt::print("  Download Limit: ");

                if (*b)
                {
                    fmt::print("{:s}\n", Speed{ *i, Speed::Units::KByps }.to_string());
                }
                else
                {
                    fmt::print("Unlimited\n");
                }
            }
        }

        if (auto b = t->value_if<bool>({ TR_KEY_upload_limited, TR_KEY_upload_limited_camel }); b)
        {
            if (auto i = t->value_if<int64_t>({ TR_KEY_upload_limit, TR_KEY_upload_limit_camel }); i)
            {
                fmt::print("  Upload Limit: ");

                if (*b)
                {
                    fmt::print("{:s}\n", Speed{ *i, Speed::Units::KByps }.to_string());
                }
                else
                {
                    fmt::print("Unlimited\n");
                }
            }
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_seed_ratio_mode, TR_KEY_seed_ratio_mode_camel }); i)
        {
            switch (*i)
            {
            case TR_RATIOLIMIT_GLOBAL:
                fmt::print("  Ratio Limit: Default\n");
                break;

            case TR_RATIOLIMIT_SINGLE:
                if (auto d = t->value_if<double>({ TR_KEY_seed_ratio_limit, TR_KEY_seed_ratio_limit_camel }); d)
                {
                    fmt::print("  Ratio Limit: {:s}\n", strlratio2(*d));
                }
                break;

            case TR_RATIOLIMIT_UNLIMITED:
                fmt::print("  Ratio Limit: Unlimited\n");
                break;

            default:
                break;
            }
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_seed_idle_mode, TR_KEY_seed_idle_mode_camel }); i)
        {
            switch (*i)
            {
            case TR_IDLELIMIT_GLOBAL:
                fmt::print("  Idle Limit: Default\n");
                break;

            case TR_IDLELIMIT_SINGLE:
                if (auto j = t->value_if<int64_t>({ TR_KEY_seed_idle_limit, TR_KEY_seed_idle_limit_camel }); j)
                {
                    fmt::print("  Idle Limit: {} minutes\n", *j);
                }

                break;

            case TR_IDLELIMIT_UNLIMITED:
                fmt::print("  Idle Limit: Unlimited\n");
                break;

            default:
                break;
            }
        }

        if (auto b = t->value_if<bool>({ TR_KEY_honors_session_limits, TR_KEY_honors_session_limits_camel }); b)
        {
            fmt::print("  Honors Session Limits: {:s}\n", *b ? "Yes" : "No");
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_peer_limit, TR_KEY_peer_limit_kebab }); i)
        {
            fmt::print("  Peer limit: {:d}\n", *i);
        }

        if (auto i = t->value_if<int64_t>({ TR_KEY_bandwidth_priority, TR_KEY_bandwidth_priority_camel }); i)
        {
            fmt::print("  Bandwidth Priority: {:s}\n", BandwidthPriorityNames[(*i + 1) & 0b11]);
        }

        fmt::print("\n");
    }
}

void print_file_list(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    auto* const torrents = args->find_if<tr_variant::Vector>(TR_KEY_torrents);
    if (torrents == nullptr)
    {
        return;
    }

    for (auto const& t_var : *torrents)
    {
        auto* const t = t_var.get_if<tr_variant::Map>();
        if (t == nullptr)
        {
            continue;
        }

        auto* const files = t->find_if<tr_variant::Vector>(TR_KEY_files);
        auto* const priorities = t->find_if<tr_variant::Vector>(TR_KEY_priorities);
        auto* const wanteds = t->find_if<tr_variant::Vector>(TR_KEY_wanted);
        auto name = t->value_if<std::string_view>(TR_KEY_name);

        if (!name || files == nullptr || priorities == nullptr || wanteds == nullptr)
        {
            continue;
        }

        auto const n = std::size(*files);
        fmt::print("{:s} ({:d} files):\n", *name, n);
        fmt::print("{:>3s}  {:>5s} {:>8s} {:>3s} {:>9s}  {:s}\n", "#", "Done", "Priority", "Get", "Size", "Name");

        for (size_t i = 0; i < n; ++i)
        {
            auto* const file = (*files)[i].get_if<tr_variant::Map>();
            if (file == nullptr)
            {
                continue;
            }

            auto const have = file->value_if<int64_t>({ TR_KEY_bytes_completed, TR_KEY_bytes_completed_camel });
            auto const length = file->value_if<int64_t>(TR_KEY_length);
            auto const priority = priorities->at(i).value_if<int64_t>();
            auto const wanted = wanteds->at(i).value_if<bool>();
            auto const filename = file->value_if<std::string_view>(TR_KEY_name);

            if (!length || !filename || !have || !priority || !wanted)
            {
                continue;
            }

            static auto constexpr Pristr = [](int64_t p)
            {
                switch (p)
                {
                case TR_PRI_LOW:
                    return "Low"sv;
                case TR_PRI_HIGH:
                    return "High"sv;
                default:
                    return "Normal"sv;
                }
            };

            fmt::print(
                "{:3d}: {:>4s}% {:<8s} {:<3s} {:9s}  {:s}\n",
                i,
                strlpercent(100.0 * *have / *length),
                Pristr(*priority),
                *wanted ? "Yes" : "No",
                strlsize(*length),
                *filename);
        }
    }
}

void print_peers_impl(tr_variant::Vector const& peers)
{
    fmt::print("{:<40s}  {:<12s}  {:<5s} {:<6s}  {:<6s}  {:s}\n", "Address", "Flags", "Done", "Down", "Up", "Client");

    for (auto const& peer_var : peers)
    {
        auto* const peer = peer_var.get_if<tr_variant::Map>();
        if (peer == nullptr)
        {
            continue;
        }

        auto const address = peer->value_if<std::string_view>(TR_KEY_address);
        auto const client = peer->value_if<std::string_view>({ TR_KEY_client_name, TR_KEY_client_name_camel });
        auto const flagstr = peer->value_if<std::string_view>({ TR_KEY_flag_str, TR_KEY_flag_str_camel });
        auto const progress = peer->value_if<double>(TR_KEY_progress);
        auto const rate_to_client = peer->value_if<int64_t>({ TR_KEY_rate_to_client, TR_KEY_rate_to_client_camel });
        auto const rate_to_peer = peer->value_if<int64_t>({ TR_KEY_rate_to_peer, TR_KEY_rate_to_peer_camel });

        if (address && client && progress && flagstr && rate_to_client && rate_to_peer)
        {
            fmt::print(
                "{:<40s}  {:<12s}  {:<5s} {:6.1f}  {:6.1f}  {:s}\n",
                *address,
                *flagstr,
                strlpercent(*progress * 100.0),
                Speed{ *rate_to_client, Speed::Units::KByps }.count(Speed::Units::KByps),
                Speed{ *rate_to_peer, Speed::Units::KByps }.count(Speed::Units::KByps),
                *client);
        }
    }
}

void print_peers(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    auto* const torrents = args->find_if<tr_variant::Vector>(TR_KEY_torrents);
    if (torrents == nullptr)
    {
        return;
    }

    for (auto it = std::begin(*torrents), end = std::end(*torrents); it != end; ++it)
    {
        auto* const t = it->get_if<tr_variant::Map>();
        if (t == nullptr)
        {
            continue;
        }

        if (auto* peers = t->find_if<tr_variant::Vector>(TR_KEY_peers); peers != nullptr)
        {
            print_peers_impl(*peers);

            if (it < std::prev(end))
            {
                fmt::print("\n");
            }
        }
    }
}

void print_pieces_impl(std::string_view raw, size_t piece_count)
{
    auto const str = tr_base64_decode(raw);
    fmt::print("  ");

    size_t piece = 0;
    static size_t constexpr col_width = 0b111111; // 64 - 1
    for (auto const ch : str)
    {
        for (int bit = 0; piece < piece_count && bit < 8; ++bit, ++piece)
        {
            fmt::print("{:c}", (ch & (1 << (7 - bit))) != 0 ? '1' : '0');
        }

        fmt::print(" ");

        if ((piece & col_width) == 0) // piece % 64 == 0
        {
            fmt::print("\n  ");
        }
    }

    fmt::print("\n");
}

void print_pieces(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    auto* const torrents = args->find_if<tr_variant::Vector>(TR_KEY_torrents);
    if (torrents == nullptr)
    {
        return;
    }

    for (auto it = std::begin(*torrents), end = std::end(*torrents); it != end; ++it)
    {
        auto* const t = it->get_if<tr_variant::Map>();
        if (t == nullptr)
        {
            continue;
        }

        auto piece_count = t->value_if<int64_t>({ TR_KEY_piece_count, TR_KEY_piece_count_camel });
        auto pieces = t->value_if<std::string_view>(TR_KEY_pieces);

        if (!piece_count || !pieces)
        {
            continue;
        }

        TR_ASSERT(*piece_count >= 0);
        print_pieces_impl(*pieces, static_cast<size_t>(*piece_count));

        if (it < std::prev(end))
        {
            fmt::print("\n");
        }
    }
}

void print_port_test(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    if (auto is_open = args->value_if<bool>(TR_KEY_port_is_open); is_open)
    {
        fmt::print("Port is open: {:s}\n", *is_open ? "Yes" : "No");
    }
}

void print_torrent_list(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    auto* const torrents = args->find_if<tr_variant::Vector>(TR_KEY_torrents);
    if (torrents == nullptr)
    {
        return;
    }

    fmt::print(
        "{:>6s}   {:>5s}  {:>9s}  {:<9s}  {:>6s}  {:>6s}  {:<5s}  {:<11s}  {:<s}\n",
        "ID",
        "Done",
        "Have",
        "ETA",
        "Up",
        "Down",
        "Ratio",
        "Status",
        "Name");

    auto tptrs = std::vector<tr_variant::Map const*>{};
    tptrs.reserve(std::size(*torrents));
    for (auto const& t_var : *torrents)
    {
        if (auto* t = t_var.get_if<tr_variant::Map>(); t != nullptr && t->value_if<int64_t>(TR_KEY_id))
        {
            tptrs.push_back(t);
        }
    }

    std::sort(
        tptrs.begin(),
        tptrs.end(),
        [](tr_variant::Map const* f, tr_variant::Map const* s)
        {
            static auto constexpr Min = std::numeric_limits<int64_t>::min();
            auto const f_time = f->value_if<int64_t>({ TR_KEY_added_date, TR_KEY_added_date_camel }).value_or(Min);
            auto const s_time = s->value_if<int64_t>({ TR_KEY_added_date, TR_KEY_added_date_camel }).value_or(Min);
            return f_time < s_time;
        });

    int64_t total_size = 0;
    int64_t total_up = 0;
    int64_t total_down = 0;
    for (auto const& t : tptrs)
    {
        auto o_tor_id = t->value_if<int64_t>(TR_KEY_id);
        auto o_eta = t->value_if<int64_t>(TR_KEY_eta);
        auto o_status = t->value_if<int64_t>(TR_KEY_status);
        auto o_up = t->value_if<int64_t>({ TR_KEY_rate_upload, TR_KEY_rate_upload_camel });
        auto o_down = t->value_if<int64_t>({ TR_KEY_rate_download, TR_KEY_rate_download_camel });
        auto o_size_when_done = t->value_if<int64_t>({ TR_KEY_size_when_done, TR_KEY_size_when_done_camel });
        auto o_left_until_done = t->value_if<int64_t>({ TR_KEY_left_until_done, TR_KEY_left_until_done_camel });
        auto o_ratio = t->value_if<double>({ TR_KEY_upload_ratio, TR_KEY_upload_ratio_camel });
        auto o_name = t->value_if<std::string_view>(TR_KEY_name);

        if (!o_eta || !o_tor_id || !o_left_until_done || !o_name || !o_down || !o_up || !o_size_when_done || !o_status ||
            !o_ratio)
        {
            continue;
        }

        auto const eta = *o_eta;
        auto const up = *o_up;
        auto const down = *o_down;
        auto const size_when_done = *o_size_when_done;
        auto const left_until_done = *o_left_until_done;

        auto const eta_str = left_until_done != 0 || eta != -1 ? eta_to_string(eta) : "Done";
        auto const error_mark = t->value_if<int64_t>(TR_KEY_error).value_or(0) != 0 ? '*' : ' ';
        auto const done_str = size_when_done != 0 ?
            strlpercent(100.0 * (size_when_done - left_until_done) / size_when_done) + '%' :
            std::string{ "n/a" };

        fmt::print(
            "{:>6d}{:c}  {:>5s}  {:>9s}  {:<9s}  {:6.1f}  {:6.1f}  {:>5s}  {:<11s}  {:<s}\n",
            *o_tor_id,
            error_mark,
            done_str,
            strlsize(size_when_done - left_until_done),
            eta_str,
            Speed{ up, Speed::Units::Byps }.count(Speed::Units::KByps),
            Speed{ down, Speed::Units::Byps }.count(Speed::Units::KByps),
            strlratio2(*o_ratio),
            get_status_string(*t),
            *o_name);

        total_up += up;
        total_down += down;
        total_size += size_when_done - left_until_done;
    }

    fmt::print(
        "Sum:            {:>9s}             {:6.1f}  {:6.1f}\n",
        strlsize(total_size).c_str(),
        Speed{ total_up, Speed::Units::Byps }.count(Speed::Units::KByps),
        Speed{ total_down, Speed::Units::Byps }.count(Speed::Units::KByps));
}

void print_trackers_impl(tr_variant::Vector const& tracker_stats)
{
    for (auto const& t_var : tracker_stats)
    {
        auto* const t = t_var.get_if<tr_variant::Map>();
        if (t == nullptr)
        {
            continue;
        }

        auto const announce_state = t->value_if<int64_t>({ TR_KEY_announce_state, TR_KEY_announce_state_camel });
        auto const download_count = t->value_if<int64_t>({ TR_KEY_download_count, TR_KEY_download_count_camel });
        auto const has_announced = t->value_if<bool>({ TR_KEY_has_announced, TR_KEY_has_announced_camel });
        auto const has_scraped = t->value_if<bool>({ TR_KEY_has_scraped, TR_KEY_has_scraped_camel });
        auto const host = t->value_if<std::string_view>(TR_KEY_host);
        auto const is_backup = t->value_if<bool>({ TR_KEY_is_backup, TR_KEY_is_backup_camel });
        auto const last_announce_peer_count = t->value_if<int64_t>(
            { TR_KEY_last_announce_peer_count, TR_KEY_last_announce_peer_count_camel });
        auto const last_announce_result = t->value_if<std::string_view>(
            { TR_KEY_last_announce_result, TR_KEY_last_announce_result_camel });
        auto const last_announce_start_time = t->value_if<int64_t>(
            { TR_KEY_last_announce_start_time, TR_KEY_last_announce_start_time_camel });
        auto const last_announce_time = t->value_if<int64_t>({ TR_KEY_last_announce_time, TR_KEY_last_announce_time_camel });
        auto const last_scrape_result = t->value_if<std::string_view>(TR_KEY_lastScrapeResult);
        auto const last_scrape_start_time = t->value_if<int64_t>(TR_KEY_lastScrapeStartTime);
        auto const last_scrape_succeeded = t->value_if<bool>(TR_KEY_lastScrapeSucceeded);
        auto const last_scrape_time = t->value_if<int64_t>(TR_KEY_lastScrapeTime);
        auto const last_scrape_timed_out = t->value_if<bool>(TR_KEY_lastScrapeTimedOut);
        auto const leecher_count = t->value_if<int64_t>(TR_KEY_leecherCount);
        auto const next_announce_time = t->value_if<int64_t>(TR_KEY_nextAnnounceTime);
        auto const next_scrape_time = t->value_if<int64_t>(TR_KEY_nextScrapeTime);
        auto const scrape_state = t->value_if<int64_t>(TR_KEY_scrapeState);
        auto const seeder_count = t->value_if<int64_t>(TR_KEY_seederCount);
        auto const tier = t->value_if<int64_t>(TR_KEY_tier);
        auto const tracker_id = t->value_if<int64_t>(TR_KEY_id);
        auto const last_announce_succeeded = t->value_if<bool>(
            { TR_KEY_last_announce_succeeded, TR_KEY_last_announce_succeeded_camel });
        auto const last_announce_timed_out = t->value_if<bool>(
            { TR_KEY_last_announce_timed_out, TR_KEY_last_announce_timed_out_camel });

        if (!download_count || !has_announced || !has_scraped || !host || !tracker_id || !is_backup || !announce_state ||
            !scrape_state || !last_announce_peer_count || !last_announce_result || !last_announce_start_time ||
            !last_announce_succeeded || !last_announce_time || !last_announce_timed_out || !last_scrape_result ||
            !last_scrape_start_time || !last_scrape_succeeded || !last_scrape_time || !last_scrape_timed_out ||
            !leecher_count || !next_announce_time || !next_scrape_time || !seeder_count || !tier)
        {
            continue;
        }

        time_t const now = time(nullptr);

        fmt::print("\n");
        fmt::print("  Tracker {:d}: {:s}\n", *tracker_id, *host);

        if (*is_backup)
        {
            fmt::print("  Backup on tier {:d}\n", *tier);
            continue;
        }
        fmt::print("  Active in tier {:d}\n", *tier);

        if (*has_announced && *announce_state != TR_TRACKER_INACTIVE)
        {
            auto const timestr = tr_strltime(now - *last_announce_time);

            if (*last_announce_succeeded)
            {
                fmt::print("  Got a list of {:d} peers {:s} ago\n", *last_announce_peer_count, timestr);
            }
            else if (*last_announce_timed_out)
            {
                fmt::print("  Peer list request timed out; will retry\n");
            }
            else
            {
                fmt::print("  Got an error '{:s}' {:s} ago\n", *last_announce_result, timestr);
            }
        }

        switch (*announce_state)
        {
        case TR_TRACKER_INACTIVE:
            fmt::print("  No updates scheduled\n");
            break;

        case TR_TRACKER_WAITING:
            fmt::print("  Asking for more peers in {:s}\n", tr_strltime(*next_announce_time - now));
            break;

        case TR_TRACKER_QUEUED:
            fmt::print("  Queued to ask for more peers\n");
            break;

        case TR_TRACKER_ACTIVE:
            fmt::print("  Asking for more peers now... {:s}\n", tr_strltime(now - *last_announce_start_time));
            break;

        default:
            break;
        }

        if (*has_scraped)
        {
            auto const timestr = tr_strltime(now - *last_scrape_time);

            if (*last_scrape_succeeded)
            {
                fmt::print("  Tracker had {:d} seeders and {:d} leechers {:s} ago\n", *seeder_count, *leecher_count, timestr);
            }
            else if (*last_scrape_timed_out)
            {
                fmt::print("  Tracker scrape timed out; will retry\n");
            }
            else
            {
                fmt::print("  Got a scrape error '{:s}' {:s} ago\n", *last_scrape_result, timestr);
            }
        }

        switch (*scrape_state)
        {
        case TR_TRACKER_WAITING:
            fmt::print("  Asking for peer counts in {:s}\n", tr_strltime(*next_scrape_time - now));
            break;

        case TR_TRACKER_QUEUED:
            fmt::print("  Queued to ask for peer counts\n");
            break;

        case TR_TRACKER_ACTIVE:
            fmt::print("  Asking for peer counts now... {:s}\n", tr_strltime(now - *last_scrape_start_time));
            break;

        default: // TR_TRACKER_INACTIVE
            break;
        }
    }
}

void print_trackers(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    auto* const torrents = args->find_if<tr_variant::Vector>(TR_KEY_torrents);
    if (torrents == nullptr)
    {
        return;
    }

    for (auto it = std::begin(*torrents), end = std::end(*torrents); it != end; ++it)
    {
        auto* const t = it->get_if<tr_variant::Map>();
        if (t == nullptr)
        {
            continue;
        }

        auto* const tracker_stats = t->find_if<tr_variant::Vector>({ TR_KEY_tracker_stats, TR_KEY_tracker_stats_camel });
        if (tracker_stats == nullptr)
        {
            continue;
        }

        print_trackers_impl(*tracker_stats);

        if (it < std::prev(end))
        {
            fmt::print("\n");
        }
    }
}

void print_session(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    fmt::print("VERSION\n");

    if (auto sv = args->value_if<std::string_view>(TR_KEY_version); sv)
    {
        fmt::print("  Daemon version: {:s}\n", *sv);
    }

    if (auto i = args->value_if<int64_t>(TR_KEY_rpc_version); i)
    {
        fmt::print("  RPC version: {:d}\n", *i);
    }

    if (auto i = args->value_if<int64_t>(TR_KEY_rpc_version_minimum); i)
    {
        fmt::print("  RPC minimum version: {:d}\n", *i);
    }

    fmt::print("\n");

    fmt::print("CONFIG\n");

    if (auto sv = args->value_if<std::string_view>(TR_KEY_config_dir); sv)
    {
        fmt::print("  Configuration directory: {:s}\n", *sv);
    }

    if (auto sv = args->value_if<std::string_view>({ TR_KEY_download_dir, TR_KEY_download_dir_kebab }); sv)
    {
        fmt::print("  Download directory: {:s}\n", *sv);
    }

    if (auto i = args->value_if<int64_t>(TR_KEY_peer_port); i)
    {
        fmt::print("  Listen port: {:d}\n", *i);
    }

    if (auto b = args->value_if<bool>(TR_KEY_port_forwarding_enabled); b)
    {
        fmt::print("  Port forwarding enabled: {:s}\n", *b ? "Yes" : "No");
    }

    if (auto b = args->value_if<bool>(TR_KEY_utp_enabled); b)
    {
        fmt::print("  µTP enabled: {:s}\n", *b ? "Yes" : "No");
    }

    if (auto b = args->value_if<bool>(TR_KEY_dht_enabled); b)
    {
        fmt::print("  Distributed hash table enabled: {:s}\n", *b ? "Yes" : "No");
    }

    if (auto b = args->value_if<bool>(TR_KEY_lpd_enabled); b)
    {
        fmt::print("  Local peer discovery enabled: {:s}\n", *b ? "Yes" : "No");
    }

    if (auto b = args->value_if<bool>(TR_KEY_pex_enabled); b)
    {
        fmt::print("  Peer exchange allowed: {:s}\n", *b ? "Yes" : "No");
    }

    if (auto sv = args->value_if<std::string_view>(TR_KEY_encryption); sv)
    {
        fmt::print("  Encryption: {:s}\n", *sv);
    }

    if (auto i = args->value_if<int64_t>(TR_KEY_cache_size_mb); i)
    {
        fmt::print("  Maximum memory cache size: {:s}\n", Memory{ *i, Memory::Units::MBytes }.to_string());
    }

    auto const alt_enabled = args->value_if<bool>(TR_KEY_alt_speed_enabled);
    auto const alt_time_enabled = args->value_if<bool>(TR_KEY_alt_speed_time_enabled);
    auto const up_enabled = args->value_if<bool>(TR_KEY_speed_limit_up_enabled);
    auto const down_enabled = args->value_if<bool>(TR_KEY_speed_limit_down_enabled);
    auto const speed_ratio_limited = args->value_if<bool>(TR_KEY_seedRatioLimited);
    auto const idle_seeding_limited = args->value_if<bool>(TR_KEY_idle_seeding_limit_enabled);
    auto const alt_down = args->value_if<int64_t>(TR_KEY_alt_speed_down);
    auto const alt_up = args->value_if<int64_t>(TR_KEY_alt_speed_up);
    auto const alt_begin = args->value_if<int64_t>(TR_KEY_alt_speed_time_begin);
    auto const alt_end = args->value_if<int64_t>(TR_KEY_alt_speed_time_end);
    auto const alt_day = args->value_if<int64_t>(TR_KEY_alt_speed_time_day);
    auto const up_limit = args->value_if<int64_t>(TR_KEY_speed_limit_up);
    auto const down_limit = args->value_if<int64_t>(TR_KEY_speed_limit_down);
    auto const peer_limit = args->value_if<int64_t>(TR_KEY_peer_limit_global);
    auto const idle_seeding_limit = args->value_if<int64_t>(TR_KEY_idle_seeding_limit);
    auto const seed_ratio_limit = args->value_if<double>({ TR_KEY_seed_ratio_limit, TR_KEY_seed_ratio_limit_camel });

    if (alt_down && alt_enabled && alt_begin && alt_time_enabled && alt_end && alt_day && alt_up && peer_limit && down_limit &&
        down_enabled && up_limit && up_enabled && seed_ratio_limit && speed_ratio_limited && idle_seeding_limited &&
        idle_seeding_limit)
    {
        fmt::print("\n");
        fmt::print("LIMITS\n");
        fmt::print("  Peer limit: {:d}\n", *peer_limit);

        fmt::print("  Default seed ratio limit: {:s}\n", *speed_ratio_limited ? strlratio2(*seed_ratio_limit) : "Unlimited");

        fmt::print(
            "  Default idle seeding time limit: {:s}\n",
            *idle_seeding_limited ? std::to_string(*idle_seeding_limit) + " minutes" : "Unlimited");

        std::string effective_up_limit;

        if (*alt_enabled)
        {
            effective_up_limit = Speed{ *alt_up, Speed::Units::KByps }.to_string();
        }
        else if (*up_enabled)
        {
            effective_up_limit = Speed{ *up_limit, Speed::Units::KByps }.to_string();
        }
        else
        {
            effective_up_limit = "Unlimited"s;
        }

        fmt::print(
            "  Upload speed limit: {:s} ({:s} limit: {:s}; {:s} turtle limit: {:s})\n",
            effective_up_limit,
            *up_enabled ? "Enabled" : "Disabled",
            Speed{ *up_limit, Speed::Units::KByps }.to_string(),
            *alt_enabled ? "Enabled" : "Disabled",
            Speed{ *alt_up, Speed::Units::KByps }.to_string());

        std::string effective_down_limit;

        if (*alt_enabled)
        {
            effective_down_limit = Speed{ *alt_down, Speed::Units::KByps }.to_string();
        }
        else if (*down_enabled)
        {
            effective_down_limit = Speed{ *down_limit, Speed::Units::KByps }.to_string();
        }
        else
        {
            effective_down_limit = "Unlimited"s;
        }

        fmt::print(
            "  Download speed limit: {:s} ({:s} limit: {:s}; {:s} turtle limit: {:s})\n",
            effective_down_limit,
            *down_enabled ? "Enabled" : "Disabled",
            Speed{ *down_limit, Speed::Units::KByps }.to_string(),
            *alt_enabled ? "Enabled" : "Disabled",
            Speed{ *alt_down, Speed::Units::KByps }.to_string());

        if (*alt_time_enabled)
        {
            fmt::print(
                "  Turtle schedule: {:02d}:{:02d} - {:02d}:{:02d}  ",
                *alt_begin / 60,
                *alt_begin % 60,
                *alt_end / 60,
                *alt_end % 60);

            if ((*alt_day & TR_SCHED_SUN) != 0)
            {
                fmt::print("Sun ");
            }

            if ((*alt_day & TR_SCHED_MON) != 0)
            {
                fmt::print("Mon ");
            }

            if ((*alt_day & TR_SCHED_TUES) != 0)
            {
                fmt::print("Tue ");
            }

            if ((*alt_day & TR_SCHED_WED) != 0)
            {
                fmt::print("Wed ");
            }

            if ((*alt_day & TR_SCHED_THURS) != 0)
            {
                fmt::print("Thu ");
            }

            if ((*alt_day & TR_SCHED_FRI) != 0)
            {
                fmt::print("Fri ");
            }

            if ((*alt_day & TR_SCHED_SAT) != 0)
            {
                fmt::print("Sat ");
            }

            fmt::print("\n");
        }
    }

    fmt::print("\n");

    fmt::print("MISC\n");

    if (auto b = args->value_if<bool>(TR_KEY_start_added_torrents); b)
    {
        fmt::print("  Autostart added torrents: {:s}\n", *b ? "Yes" : "No");
    }

    if (auto b = args->value_if<bool>(TR_KEY_trash_original_torrent_files); b)
    {
        fmt::print("  Delete automatically added torrents: {:s}\n", *b ? "Yes" : "No");
    }
}

void print_session_stats(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    if (auto* d = args->find_if<tr_variant::Map>(TR_KEY_current_stats); d != nullptr)
    {
        auto const up = d->value_if<int64_t>(TR_KEY_uploadedBytes);
        auto const down = d->value_if<int64_t>(TR_KEY_downloadedBytes);
        auto const secs = d->value_if<int64_t>(TR_KEY_secondsActive);

        if (up && down && secs)
        {
            fmt::print("\nCURRENT SESSION\n");
            fmt::print("  Uploaded:   {:s}\n", strlsize(*up));
            fmt::print("  Downloaded: {:s}\n", strlsize(*down));
            fmt::print("  Ratio:      {:s}\n", strlratio(*up, *down));
            fmt::print("  Duration:   {:s}\n", tr_strltime(*secs));
        }
    }

    if (auto* d = args->find_if<tr_variant::Map>(TR_KEY_cumulative_stats); d != nullptr)
    {
        auto const up = d->value_if<int64_t>(TR_KEY_uploadedBytes);
        auto const down = d->value_if<int64_t>(TR_KEY_downloadedBytes);
        auto const secs = d->value_if<int64_t>(TR_KEY_secondsActive);
        auto const sessions = d->value_if<int64_t>(TR_KEY_sessionCount);

        if (up && down && secs && sessions)
        {
            fmt::print("\nTOTAL\n");
            fmt::print("  Started {:d} times\n", *sessions);
            fmt::print("  Uploaded:   {:s}\n", strlsize(*up));
            fmt::print("  Downloaded: {:s}\n", strlsize(*down));
            fmt::print("  Ratio:      {:s}\n", strlratio(*up, *down));
            fmt::print("  Duration:   {:s}\n", tr_strltime(*secs));
        }
    }
}

void print_groups(tr_variant::Map const& map)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    auto* const groups = args->find_if<tr_variant::Vector>(TR_KEY_group);
    if (groups == nullptr)
    {
        return;
    }

    for (auto const& group_var : *groups)
    {
        auto* const group = group_var.get_if<tr_variant::Map>();
        if (group == nullptr)
        {
            continue;
        }

        auto const name = group->value_if<std::string_view>(TR_KEY_name);
        auto const up_enabled = group->value_if<bool>({ TR_KEY_upload_limited, TR_KEY_upload_limited_camel });
        auto const down_enabled = group->value_if<bool>({ TR_KEY_download_limited, TR_KEY_download_limited_camel });
        auto const up_limit = group->value_if<int64_t>({ TR_KEY_upload_limit, TR_KEY_upload_limit_camel });
        auto const down_limit = group->value_if<int64_t>({ TR_KEY_download_limit, TR_KEY_download_limit_camel });
        auto const honors = group->value_if<bool>({ TR_KEY_honors_session_limits, TR_KEY_honors_session_limits_camel });
        if (name && down_limit && down_enabled && up_limit && up_enabled && honors)
        {
            fmt::print("{:s}: ", *name);
            fmt::print(
                "Upload speed limit: {:s}, Download speed limit: {:s}, {:s} session bandwidth limits\n",
                *up_enabled ? Speed{ *up_limit, Speed::Units::KByps }.to_string() : "unlimited"s,
                *down_enabled ? Speed{ *down_limit, Speed::Units::KByps }.to_string() : "unlimited"s,
                *honors ? "honors" : "does not honor");
        }
    }
}

void filter_ids(tr_variant::Map const& map, RemoteConfig& config)
{
    auto* const args = map.find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        return;
    }

    auto* const torrents = args->find_if<tr_variant::Vector>(TR_KEY_torrents);
    if (torrents == nullptr)
    {
        return;
    }

    std::set<int64_t> ids;

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

    for (auto const& t_var : *torrents)
    {
        auto* const t = t_var.get_if<tr_variant::Map>();
        if (t == nullptr)
        {
            continue;
        }

        auto const tor_id = t->value_if<int64_t>(TR_KEY_id).value_or(-1);
        if (tor_id < 0)
        {
            continue;
        }

        bool include = negate;
        auto const status = get_status_string(*t);
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
            if (status.find("Up") != std::string::npos || status == "Seeding")
            {
                include = !include;
            }
            break;
        case 'l': // label
            if (auto* l = t->find_if<tr_variant::Vector>(TR_KEY_labels); l != nullptr)
            {
                for (auto const& label_var : *l)
                {
                    if (auto sv = label_var.value_if<std::string_view>(); sv && arg == *sv)
                    {
                        include = !include;
                        break;
                    }
                }
            }
            break;
        case 'n': // Torrent name substring
            if (auto name = t->value_if<std::string_view>(TR_KEY_name); !name)
            {
                continue;
            }
            else if (name->find(arg) != std::string::npos)
            {
                include = !include;
            }
            break;
        case 'r': // Minimal ratio
            if (auto ratio = t->value_if<double>({ TR_KEY_upload_ratio, TR_KEY_upload_ratio_camel }); !ratio)
            {
                continue;
            }
            else if (*ratio >= std::stof(std::string{ arg }))
            {
                include = !include;
            }
            break;
        case 'w': // Not all torrent wanted
            if (auto total_size = t->value_if<int64_t>({ TR_KEY_total_size, TR_KEY_total_size_camel }).value_or(-1);
                total_size < 0)
            {
                continue;
            }
            else if (auto size_when_done = t->value_if<int64_t>({ TR_KEY_size_when_done, TR_KEY_size_when_done_camel })
                                               .value_or(-1);
                     size_when_done < 0)
            {
                continue;
            }
            else if (total_size > size_when_done)
            {
                include = !include;
            }
            break;
        default:
            break;
        }

        if (include)
        {
            ids.insert(tor_id);
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

int process_response(char const* rpcurl, std::string_view response, RemoteConfig& config)
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

    if (auto top = tr_variant_serde::json().inplace().parse(response); !top)
    {
        tr_logAddWarn(fmt::format("Unable to parse response '{}'", response));
        status |= EXIT_FAILURE;
    }
    else if (auto* map_ptr = top->get_if<tr_variant::Map>(); map_ptr == nullptr)
    {
        tr_logAddWarn("Response was not a JSON object");
        status |= EXIT_FAILURE;
    }
    else if (auto osv = map_ptr->value_if<std::string_view>(TR_KEY_result); osv)
    {
        auto const& map = *map_ptr;
        auto const& sv = *osv;
        if (sv != "success"sv)
        {
            fmt::print("Error: {:s}\n", sv);
            status |= EXIT_FAILURE;
        }
        else
        {
            auto const tag = map.value_if<int64_t>(TR_KEY_tag).value_or(-1);
            switch (tag)
            {
            case TAG_SESSION:
                print_session(map);
                break;

            case TAG_STATS:
                print_session_stats(map);
                break;

            case TAG_DETAILS:
                print_details(map);
                break;

            case TAG_FILES:
                print_file_list(map);
                break;

            case TAG_LIST:
                print_torrent_list(map);
                break;

            case TAG_PEERS:
                print_peers(map);
                break;

            case TAG_PIECES:
                print_pieces(map);
                break;

            case TAG_PORTTEST:
                print_port_test(map);
                break;

            case TAG_TRACKERS:
                print_trackers(map);
                break;

            case TAG_GROUPS:
                print_groups(map);
                break;

            case TAG_FILTER:
                filter_ids(map, config);
                break;

            case TAG_TORRENT_ADD:
                if (auto* b = map.find_if<tr_variant::Map>(TR_KEY_arguments); b != nullptr)
                {
                    b = b->find_if<tr_variant::Map>(TR_KEY_torrent_added);
                    if (b != nullptr)
                    {
                        if (auto i = b->value_if<int64_t>(TR_KEY_id); i)
                        {
                            config.torrent_ids = std::to_string(*i);
                        }
                    }
                }
                [[fallthrough]];

            default:
                fmt::print("{:s} responded: {:s}\n", rpcurl, sv);
                break;
            }
        }
    }
    else
    {
        status |= EXIT_FAILURE;
    }

    return status;
}

CURL* tr_curl_easy_init(struct evbuffer* writebuf, RemoteConfig& config)
{
    CURL* curl = curl_easy_init();
    (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, fmt::format("{:s}/{:s}", MyName, LONG_VERSION_STRING).c_str());
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, writebuf);
    (void)curl_easy_setopt(curl, CURLOPT_HEADERDATA, &config);
    (void)curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, parse_response_header);
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
        auto const h = fmt::format("{:s}: {:s}", TR_RPC_SESSION_ID_HEADER, str);
        auto* const custom_headers = curl_slist_append(nullptr, h.c_str());

        (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, custom_headers);
        (void)curl_easy_setopt(curl, CURLOPT_PRIVATE, custom_headers);
    }

    return curl;
}

void tr_curl_easy_cleanup(CURL* curl)
{
    struct curl_slist* custom_headers = nullptr;
    curl_easy_getinfo(curl, CURLINFO_PRIVATE, &custom_headers);

    curl_easy_cleanup(curl);

    if (custom_headers != nullptr)
    {
        curl_slist_free_all(custom_headers);
    }
}

int flush(char const* rpcurl, tr_variant* benc, RemoteConfig& config)
{
    auto const json = tr_variant_serde::json().compact().to_string(*benc);
    auto const scheme = config.use_ssl ? "https"sv : "http"sv;
    auto const rpcurl_http = fmt::format("{:s}://{:s}", scheme, rpcurl);

    auto* const buf = evbuffer_new();
    auto* curl = tr_curl_easy_init(buf, config);
    (void)curl_easy_setopt(curl, CURLOPT_URL, rpcurl_http.c_str());
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, get_timeout_secs(json));

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
            status |= process_response(
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
            fmt::print(
                stderr,
                "Unexpected response: {:s}\n",
                std::string_view{ reinterpret_cast<char const*>(evbuffer_pullup(buf, -1)), evbuffer_get_length(buf) });
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

tr_variant::Map& ensure_sset(tr_variant& sset)
{
    auto* map = sset.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        sset = tr_variant::Map{ 3 };
        map = sset.get_if<tr_variant::Map>();
        map->try_emplace(TR_KEY_method, tr_variant::unmanaged_string("session-set"sv));
    }

    auto* args = map->find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        args = map->insert_or_assign(TR_KEY_arguments, tr_variant::Map{}).first.get_if<tr_variant::Map>();
    }
    return *args;
}

tr_variant::Map& ensure_tset(tr_variant& tset)
{
    auto* map = tset.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        tset = tr_variant::Map{ 3 };
        map = tset.get_if<tr_variant::Map>();
        map->try_emplace(TR_KEY_method, tr_variant::unmanaged_string(tr_quark_get_string_view(TR_KEY_torrent_set_kebab)));
    }

    auto* args = map->find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        args = map->insert_or_assign(TR_KEY_arguments, tr_variant::Map{ 1 }).first.get_if<tr_variant::Map>();
    }
    return *args;
}

tr_variant::Map& ensure_tadd(tr_variant& tadd)
{
    auto* map = tadd.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        tadd = tr_variant::Map{ 3 };
        map = tadd.get_if<tr_variant::Map>();
        map->try_emplace(TR_KEY_method, tr_variant::unmanaged_string("torrent-add"sv));
        map->try_emplace(TR_KEY_tag, TAG_TORRENT_ADD);
    }

    auto* args = map->find_if<tr_variant::Map>(TR_KEY_arguments);
    if (args == nullptr)
    {
        args = map->insert_or_assign(TR_KEY_arguments, tr_variant::Map{}).first.get_if<tr_variant::Map>();
    }
    return *args;
}

int process_args(char const* rpcurl, int argc, char const* const* argv, RemoteConfig& config)
{
    auto status = int{ EXIT_SUCCESS };
    char const* optarg;
    auto sset = tr_variant{};
    auto tset = tr_variant{};
    auto tadd = tr_variant{};
    auto rename_from = std::string{};

    for (;;)
    {
        int const c = tr_getopt(Usage, argc, argv, std::data(Options), &optarg);
        if (c == TR_OPT_DONE)
        {
            break;
        }

        auto const optarg_sv = std::string_view{ optarg != nullptr ? optarg : "" };
        if (auto const step_mode = get_opt_mode(c); step_mode == MODE_META_COMMAND) /* meta commands */
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

                if (auto* tset_map = tset.get_if<tr_variant::Map>(); tset_map != nullptr)
                {
                    auto* const args_map = tset_map->find_if<tr_variant::Map>(TR_KEY_arguments);
                    TR_ASSERT(args_map != nullptr);
                    if (args_map != nullptr)
                    {
                        add_id_arg(*args_map, config);
                        status |= flush(rpcurl, &tset, config);
                    }
                }

                ensure_tadd(tadd);
                break;

            case 'b': /* debug */
                config.debug = true;
                break;

            case 'j': /* return output as JSON */
                config.json = true;
                break;

            case 968: /* Unix domain socket */
                config.unix_socket_path = optarg_sv;
                break;

            case 'n': /* auth */
                config.auth = optarg_sv;
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
                config.netrc = optarg_sv;
                break;

            case 820:
                config.use_ssl = true;
                break;

            case 't': /* set current torrent */
                if (tadd.has_value())
                {
                    status |= flush(rpcurl, &tadd, config);
                }

                if (auto* tset_map = tset.get_if<tr_variant::Map>(); tset_map != nullptr)
                {
                    auto* const args_map = tset_map->find_if<tr_variant::Map>(TR_KEY_arguments);
                    TR_ASSERT(args_map != nullptr);
                    if (args_map != nullptr)
                    {
                        add_id_arg(*args_map, config);
                        status |= flush(rpcurl, &tset, config);
                    }
                }

                config.torrent_ids = optarg_sv;
                break;

            case 'V': /* show version number */
                fmt::print(stderr, "{:s} {:s}\n", MyName, LONG_VERSION_STRING);
                exit(0);

            case 944:
                fmt::print("{:s}\n", std::empty(config.torrent_ids) ? "all" : config.torrent_ids.c_str());
                break;

            case TR_OPT_ERR:
                fmt::print(stderr, "invalid option\n");
                show_usage();
                status |= EXIT_FAILURE;
                break;

            case TR_OPT_UNK:
                if (auto* tadd_map = tadd.get_if<tr_variant::Map>(); tadd_map != nullptr)
                {
                    auto* const args_map = tadd_map->find_if<tr_variant::Map>(TR_KEY_arguments);
                    TR_ASSERT(args_map != nullptr);
                    if (args_map != nullptr)
                    {
                        if (auto const metainfo = get_encoded_metainfo(optarg); !std::empty(metainfo))
                        {
                            args_map->try_emplace(TR_KEY_metainfo, metainfo);
                        }
                        else
                        {
                            args_map->try_emplace(TR_KEY_filename, optarg_sv);
                        }
                    }
                }
                else
                {
                    fmt::print(stderr, "Unknown option: {:s}\n", optarg_sv);
                    status |= EXIT_FAILURE;
                }

                break;

            default:
                break;
            }
        }
        else if (step_mode == MODE_TORRENT_GET)
        {
            if (auto* tset_map = tset.get_if<tr_variant::Map>(); tset_map != nullptr)
            {
                auto* const args_map = tset_map->find_if<tr_variant::Map>(TR_KEY_arguments);
                TR_ASSERT(args_map != nullptr);
                if (args_map != nullptr)
                {
                    add_id_arg(*args_map, config);
                    status |= flush(rpcurl, &tset, config);
                }
            }

            auto map = tr_variant::Map{ 3 };
            auto args = tr_variant::Map{ 1 };
            auto fields = tr_variant::Vector{};

            switch (c)
            {
            case 'F':
                config.filter = optarg_sv;
                map.insert_or_assign(TR_KEY_tag, TAG_FILTER);

                for (auto const& key : DetailsKeys)
                {
                    fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(key)));
                }

                add_id_arg(args, config, "all");
                break;
            case 'i':
                map.insert_or_assign(TR_KEY_tag, TAG_DETAILS);

                for (auto const& key : DetailsKeys)
                {
                    fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(key)));
                }

                add_id_arg(args, config);
                break;

            case 'l':
                map.insert_or_assign(TR_KEY_tag, TAG_LIST);

                for (auto const& key : ListKeys)
                {
                    fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(key)));
                }

                add_id_arg(args, config, "all");
                break;

            case 940:
                map.insert_or_assign(TR_KEY_tag, TAG_FILES);

                for (auto const& key : FilesKeys)
                {
                    fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(key)));
                }

                add_id_arg(args, config);
                break;

            case 941:
                map.insert_or_assign(TR_KEY_tag, TAG_PEERS);
                fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(TR_KEY_peers)));
                add_id_arg(args, config);
                break;

            case 942:
                map.insert_or_assign(TR_KEY_tag, TAG_PIECES);
                fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(TR_KEY_pieces)));
                fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(TR_KEY_piece_count_camel)));
                add_id_arg(args, config);
                break;

            case 943:
                map.insert_or_assign(TR_KEY_tag, TAG_TRACKERS);
                fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(TR_KEY_tracker_stats_camel)));
                add_id_arg(args, config);
                break;

            default:
                TR_ASSERT_MSG(false, "unhandled value");
                break;
            }

            args.insert_or_assign(TR_KEY_fields, std::move(fields));
            map.insert_or_assign(TR_KEY_method, tr_variant::unmanaged_string("torrent-get"sv));
            map.insert_or_assign(TR_KEY_arguments, std::move(args));
            auto top = tr_variant{ std::move(map) };
            status |= flush(rpcurl, &top, config);
        }
        else if (step_mode == MODE_SESSION_SET)
        {
            auto& args = ensure_sset(sset);

            switch (c)
            {
            case 800:
                args.insert_or_assign(TR_KEY_script_torrent_done_filename, optarg_sv);
                args.insert_or_assign(TR_KEY_script_torrent_done_enabled, true);
                break;

            case 801:
                args.insert_or_assign(TR_KEY_script_torrent_done_enabled, false);
                break;

            case 802:
                args.insert_or_assign(TR_KEY_script_torrent_done_seeding_filename, optarg_sv);
                args.insert_or_assign(TR_KEY_script_torrent_done_seeding_enabled, true);
                break;

            case 803:
                args.insert_or_assign(TR_KEY_script_torrent_done_seeding_enabled, false);
                break;

            case 970:
                args.insert_or_assign(TR_KEY_alt_speed_enabled, true);
                break;

            case 971:
                args.insert_or_assign(TR_KEY_alt_speed_enabled, false);
                break;

            case 972:
                args.insert_or_assign(TR_KEY_alt_speed_down, numarg(optarg_sv));
                break;

            case 973:
                args.insert_or_assign(TR_KEY_alt_speed_up, numarg(optarg_sv));
                break;

            case 974:
                args.insert_or_assign(TR_KEY_alt_speed_time_enabled, true);
                break;

            case 975:
                args.insert_or_assign(TR_KEY_alt_speed_time_enabled, false);
                break;

            case 976:
                add_time(args, TR_KEY_alt_speed_time_begin, optarg_sv);
                break;

            case 977:
                add_time(args, TR_KEY_alt_speed_time_end, optarg_sv);
                break;

            case 978:
                add_days(args, TR_KEY_alt_speed_time_day, optarg_sv);
                break;

            case 'c':
                args.insert_or_assign(TR_KEY_incomplete_dir, optarg_sv);
                args.insert_or_assign(TR_KEY_incomplete_dir_enabled, true);
                break;

            case 'C':
                args.insert_or_assign(TR_KEY_incomplete_dir_enabled, false);
                break;

            case 'e':
                args.insert_or_assign(TR_KEY_cache_size_mb, tr_num_parse<int64_t>(optarg_sv).value());
                break;

            case 910:
                args.insert_or_assign(TR_KEY_encryption, tr_variant::unmanaged_string("required"sv));
                break;

            case 911:
                args.insert_or_assign(TR_KEY_encryption, tr_variant::unmanaged_string("preferred"sv));
                break;

            case 912:
                args.insert_or_assign(TR_KEY_encryption, tr_variant::unmanaged_string("tolerated"sv));
                break;

            case 'm':
                args.insert_or_assign(TR_KEY_port_forwarding_enabled, true);
                break;

            case 'M':
                args.insert_or_assign(TR_KEY_port_forwarding_enabled, false);
                break;

            case 'o':
                args.insert_or_assign(TR_KEY_dht_enabled, true);
                break;

            case 'O':
                args.insert_or_assign(TR_KEY_dht_enabled, false);
                break;

            case 830:
                args.insert_or_assign(TR_KEY_utp_enabled, true);
                break;

            case 831:
                args.insert_or_assign(TR_KEY_utp_enabled, false);
                break;

            case 'p':
                args.insert_or_assign(TR_KEY_peer_port, numarg(optarg_sv));
                break;

            case 'P':
                args.insert_or_assign(TR_KEY_peer_port_random_on_start, true);
                break;

            case 'x':
                args.insert_or_assign(TR_KEY_pex_enabled, true);
                break;

            case 'X':
                args.insert_or_assign(TR_KEY_pex_enabled, false);
                break;

            case 'y':
                args.insert_or_assign(TR_KEY_lpd_enabled, true);
                break;

            case 'Y':
                args.insert_or_assign(TR_KEY_lpd_enabled, false);
                break;

            case 953:
                args.insert_or_assign(TR_KEY_seed_ratio_limit_camel, tr_num_parse<double>(optarg_sv).value());
                args.insert_or_assign(TR_KEY_seedRatioLimited, true);
                break;

            case 954:
                args.insert_or_assign(TR_KEY_seedRatioLimited, false);
                break;

            case 958:
                args.insert_or_assign(TR_KEY_idle_seeding_limit, tr_num_parse<int64_t>(optarg_sv).value());
                args.insert_or_assign(TR_KEY_idle_seeding_limit_enabled, true);
                break;

            case 959:
                args.insert_or_assign(TR_KEY_idle_seeding_limit_enabled, false);
                break;

            case 990:
                args.insert_or_assign(TR_KEY_start_added_torrents, false);
                break;

            case 991:
                args.insert_or_assign(TR_KEY_start_added_torrents, true);
                break;

            case 992:
                args.insert_or_assign(TR_KEY_trash_original_torrent_files, true);
                break;

            case 993:
                args.insert_or_assign(TR_KEY_trash_original_torrent_files, false);
                break;

            default:
                TR_ASSERT_MSG(false, "unhandled value");
                break;
            }
        }
        else if (step_mode == (MODE_SESSION_SET | MODE_TORRENT_SET))
        {
            tr_variant::Map* targs = nullptr;
            tr_variant::Map* sargs = nullptr;

            if (!std::empty(config.torrent_ids))
            {
                targs = &ensure_tset(tset);
            }
            else
            {
                sargs = &ensure_sset(sset);
            }

            switch (c)
            {
            case 'd':
                if (targs != nullptr)
                {
                    targs->insert_or_assign(TR_KEY_download_limit_camel, numarg(optarg_sv));
                    targs->insert_or_assign(TR_KEY_download_limited_camel, true);
                }
                else
                {
                    sargs->insert_or_assign(TR_KEY_speed_limit_down, numarg(optarg_sv));
                    sargs->insert_or_assign(TR_KEY_speed_limit_down_enabled, true);
                }

                break;

            case 'D':
                if (targs != nullptr)
                {
                    targs->insert_or_assign(TR_KEY_download_limited_camel, false);
                }
                else
                {
                    sargs->insert_or_assign(TR_KEY_speed_limit_down_enabled, false);
                }

                break;

            case 'u':
                if (targs != nullptr)
                {
                    targs->insert_or_assign(TR_KEY_upload_limit_camel, numarg(optarg_sv));
                    targs->insert_or_assign(TR_KEY_upload_limited_camel, true);
                }
                else
                {
                    sargs->insert_or_assign(TR_KEY_speed_limit_up, numarg(optarg_sv));
                    sargs->insert_or_assign(TR_KEY_speed_limit_up_enabled, true);
                }

                break;

            case 'U':
                if (targs != nullptr)
                {
                    targs->insert_or_assign(TR_KEY_upload_limited_camel, false);
                }
                else
                {
                    sargs->insert_or_assign(TR_KEY_speed_limit_up_enabled, false);
                }

                break;

            case 930:
                if (targs != nullptr)
                {
                    targs->insert_or_assign(TR_KEY_peer_limit_kebab, tr_num_parse<int64_t>(optarg_sv).value());
                }
                else
                {
                    sargs->insert_or_assign(TR_KEY_peer_limit_global, tr_num_parse<int64_t>(optarg_sv).value());
                }

                break;

            default:
                TR_ASSERT_MSG(false, "unhandled value");
                break;
            }
        }
        else if (step_mode == MODE_TORRENT_SET)
        {
            tr_variant::Map& args = ensure_tset(tset);

            switch (c)
            {
            case 712:
                {
                    auto* list = args.find_if<tr_variant::Vector>(TR_KEY_tracker_remove_camel);
                    if (list == nullptr)
                    {
                        list = args.insert_or_assign(TR_KEY_tracker_remove_camel, tr_variant::make_vector(1))
                                   .first.get_if<tr_variant::Vector>();
                    }
                    list->emplace_back(tr_num_parse<int64_t>(optarg_sv).value());
                }
                break;

            case 950:
                args.insert_or_assign(TR_KEY_seed_ratio_limit_camel, tr_num_parse<double>(optarg_sv).value());
                args.insert_or_assign(TR_KEY_seed_ratio_mode_camel, TR_RATIOLIMIT_SINGLE);
                break;

            case 951:
                args.insert_or_assign(TR_KEY_seed_ratio_mode_camel, TR_RATIOLIMIT_GLOBAL);
                break;

            case 952:
                args.insert_or_assign(TR_KEY_seed_ratio_mode_camel, TR_RATIOLIMIT_UNLIMITED);
                break;

            case 955:
                args.insert_or_assign(TR_KEY_seed_idle_limit_camel, tr_num_parse<int64_t>(optarg_sv).value());
                args.insert_or_assign(TR_KEY_seed_idle_mode_camel, TR_IDLELIMIT_SINGLE);
                break;

            case 956:
                args.insert_or_assign(TR_KEY_seed_idle_mode_camel, TR_IDLELIMIT_GLOBAL);
                break;

            case 957:
                args.insert_or_assign(TR_KEY_seed_idle_mode_camel, TR_IDLELIMIT_UNLIMITED);
                break;

            case 984:
                args.insert_or_assign(TR_KEY_honors_session_limits_camel, true);
                break;

            case 985:
                args.insert_or_assign(TR_KEY_honors_session_limits_camel, false);
                break;

            default:
                TR_ASSERT_MSG(false, "unhandled value");
                break;
            }
        }
        else if (step_mode == (MODE_TORRENT_SET | MODE_TORRENT_ADD))
        {
            tr_variant::Map& args = tadd.has_value() ? ensure_tadd(tadd) : ensure_tset(tset);

            switch (c)
            {
            case 'g':
                args.insert_or_assign(TR_KEY_files_wanted_kebab, make_files_list(optarg_sv));
                break;

            case 'G':
                args.insert_or_assign(TR_KEY_files_unwanted_kebab, make_files_list(optarg_sv));
                break;

            case 'L':
                add_labels(args, optarg_sv);
                break;

            case 730:
                set_group(args, optarg_sv);
                break;

            case 731:
                set_group(args, ""sv);
                break;

            case 900:
                args.insert_or_assign(TR_KEY_priority_high_kebab, make_files_list(optarg_sv));
                break;

            case 901:
                args.insert_or_assign(TR_KEY_priority_normal_kebab, make_files_list(optarg_sv));
                break;

            case 902:
                args.insert_or_assign(TR_KEY_priority_low_kebab, make_files_list(optarg_sv));
                break;

            case 700:
                args.insert_or_assign(TR_KEY_bandwidth_priority_camel, 1);
                break;

            case 701:
                args.insert_or_assign(TR_KEY_bandwidth_priority_camel, 0);
                break;

            case 702:
                args.insert_or_assign(TR_KEY_bandwidth_priority_camel, -1);
                break;

            case 710:
                {
                    auto* list = args.find_if<tr_variant::Vector>(TR_KEY_tracker_add_camel);
                    if (list == nullptr)
                    {
                        list = args.insert_or_assign(TR_KEY_tracker_add_camel, tr_variant::make_vector(1))
                                   .first.get_if<tr_variant::Vector>();
                    }
                    list->emplace_back(optarg_sv);
                }
                break;

            default:
                TR_ASSERT_MSG(false, "unhandled value");
                break;
            }
        }
        else if (step_mode == MODE_TORRENT_REMOVE)
        {
            auto map = tr_variant::Map{ 2 };
            auto args = tr_variant::Map{ 2 };

            args.try_emplace(TR_KEY_delete_local_data, c == 840);
            add_id_arg(args, config);

            map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string("torrent-remove"sv));
            map.try_emplace(TR_KEY_arguments, std::move(args));

            auto top = tr_variant{ std::move(map) };
            status |= flush(rpcurl, &top, config);
        }
        else if (step_mode == MODE_TORRENT_START_STOP)
        {
            auto const is_stop = c == 'S';
            if (auto* tadd_map = tadd.get_if<tr_variant::Map>(); tadd_map != nullptr)
            {
                auto* const args_map = tadd_map->find_if<tr_variant::Map>(TR_KEY_arguments);
                TR_ASSERT(args_map != nullptr);
                if (args_map != nullptr)
                {
                    args_map->insert_or_assign(TR_KEY_paused, is_stop);
                }
            }
            else
            {
                auto map = tr_variant::Map{ 2 };
                auto args = tr_variant::Map{ 1 };
                add_id_arg(args, config);
                map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(is_stop ? "torrent-stop"sv : "torrent-start"sv));
                map.try_emplace(TR_KEY_arguments, std::move(args));

                auto top = tr_variant{ std::move(map) };
                status |= flush(rpcurl, &top, config);
            }
        }
        else if (step_mode == MODE_TORRENT_ACTION)
        {
            static auto constexpr Method = [](int option)
            {
                switch (option)
                {
                case 'v':
                    return "torrent-verify"sv;
                case 600:
                    return "torrent-reannounce"sv;
                default:
                    TR_ASSERT_MSG(false, "unhandled value");
                    return ""sv;
                }
            };

            if (auto* tset_map = tset.get_if<tr_variant::Map>(); tset_map != nullptr)
            {
                auto* const args_map = tset_map->find_if<tr_variant::Map>(TR_KEY_arguments);
                TR_ASSERT(args_map != nullptr);
                if (args_map != nullptr)
                {
                    add_id_arg(*args_map, config);
                    status |= flush(rpcurl, &tset, config);
                }
            }

            auto map = tr_variant::Map{ 2 };
            auto args = tr_variant::Map{ 1 };
            add_id_arg(args, config);
            map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(Method(c)));
            map.try_emplace(TR_KEY_arguments, std::move(args));
            auto top = tr_variant{ std::move(map) };
            status |= flush(rpcurl, &top, config);
        }
        else
        {
            switch (c)
            {
            case 920: /* session-info */
                {
                    auto map = tr_variant::Map{ 2 };
                    map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string("session-get"sv));
                    map.try_emplace(TR_KEY_tag, TAG_SESSION);

                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                }
                break;

            case 'w':
                {
                    auto& args = tadd.has_value() ? ensure_tadd(tadd) : ensure_sset(sset);
                    args.insert_or_assign(TR_KEY_download_dir_kebab, optarg_sv);
                }
                break;

            case 850:
                {
                    auto map = tr_variant::Map{ 1 };
                    map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string("session-close"sv));
                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                }
                break;

            case 963:
                {
                    auto map = tr_variant::Map{ 1 };
                    map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string("blocklist-update"sv));
                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                }
                break;

            case 921:
                {
                    auto map = tr_variant::Map{ 2 };
                    map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string("session-stats"sv));
                    map.try_emplace(TR_KEY_tag, TAG_STATS);
                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                }
                break;

            case 962:
                {
                    auto map = tr_variant::Map{ 2 };
                    map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string("port-test"sv));
                    map.try_emplace(TR_KEY_tag, TAG_PORTTEST);
                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                }
                break;

            case 960:
                {
                    auto map = tr_variant::Map{ 2 };
                    auto args = tr_variant::Map{ 3 };
                    args.try_emplace(TR_KEY_location, optarg_sv);
                    args.try_emplace(TR_KEY_move, true);
                    add_id_arg(args, config);
                    map.try_emplace(
                        TR_KEY_method,
                        tr_variant::unmanaged_string(tr_quark_get_string_view(TR_KEY_torrent_set_location_kebab)));
                    map.try_emplace(TR_KEY_arguments, std::move(args));
                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                }
                break;

            case 961: /* set location */
                // TODO (5.0.0):
                // 1. Remove tadd.has_value() branch
                // 2. Group with --move under MODE_TORRENT_SET_LOCATION
                if (auto* tadd_map = tadd.get_if<tr_variant::Map>(); tadd_map != nullptr)
                {
                    auto* const args_map = tadd_map->find_if<tr_variant::Map>(TR_KEY_arguments);
                    TR_ASSERT(args_map != nullptr);
                    if (args_map != nullptr)
                    {
                        args_map->try_emplace(TR_KEY_download_dir_kebab, optarg_sv);
                    }
                }
                else
                {
                    auto map = tr_variant::Map{ 2 };
                    auto args = tr_variant::Map{ 3 };
                    args.try_emplace(TR_KEY_location, optarg_sv);
                    args.try_emplace(TR_KEY_move, false);
                    add_id_arg(args, config);
                    map.try_emplace(
                        TR_KEY_method,
                        tr_variant::unmanaged_string(tr_quark_get_string_view(TR_KEY_torrent_set_location_kebab)));
                    map.try_emplace(TR_KEY_arguments, std::move(args));
                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                }
                break;

            case 964:
                {
                    auto map = tr_variant::Map{ 2 };
                    auto args = tr_variant::Map{ 3 };
                    args.try_emplace(TR_KEY_path, rename_from);
                    args.try_emplace(TR_KEY_name, optarg_sv);
                    add_id_arg(args, config);
                    map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string("torrent-rename-path"sv));
                    map.try_emplace(TR_KEY_arguments, std::move(args));
                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                    rename_from.clear();
                }
                break;

            case 965:
                rename_from = optarg_sv;
                break;

            case 732:
                {
                    auto map = tr_variant::Map{ 2 };
                    map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string("group-get"sv));
                    map.try_emplace(TR_KEY_tag, TAG_GROUPS);
                    auto top = tr_variant{ std::move(map) };
                    status |= flush(rpcurl, &top, config);
                }
                break;

            default:
                fmt::print(stderr, "got opt [{:d}]\n", c);
                show_usage();
                break;
            }
        }
    }

    if (tadd.has_value())
    {
        status |= flush(rpcurl, &tadd, config);
    }

    if (auto* tset_map = tset.get_if<tr_variant::Map>(); tset_map != nullptr)
    {
        auto* const args_map = tset_map->find_if<tr_variant::Map>(TR_KEY_arguments);
        TR_ASSERT(args_map != nullptr);
        if (args_map != nullptr)
        {
            add_id_arg(*args_map, config);
            status |= flush(rpcurl, &tset, config);
        }
    }

    if (sset.has_value())
    {
        status |= flush(rpcurl, &sset, config);
    }

    return status;
}

bool parse_port_string(std::string_view sv, uint16_t& port)
{
    auto remainder = std::string_view{};
    auto parsed = tr_num_parse<uint16_t>(sv, &remainder);
    auto ok = parsed && std::empty(remainder);
    if (ok)
    {
        port = *parsed;
    }

    return ok;
}

/* [host:port] or [host] or [port] or [http(s?)://host:port/transmission/] */
void get_host_and_port_and_rpc_url(
    int& argc,
    char** argv,
    std::string& host,
    uint16_t& port,
    std::string& rpcurl,
    RemoteConfig& config)
{
    if (*argv[1] == '-')
    {
        return;
    }

    auto const sv = std::string_view{ argv[1] };
    if (tr_strv_starts_with(sv, "http://")) /* user passed in http rpc url */
    {
        rpcurl = fmt::format("{:s}/rpc/", sv.substr(7));
    }
    else if (tr_strv_starts_with(sv, "https://")) /* user passed in https rpc url */
    {
        config.use_ssl = true;
        rpcurl = fmt::format("{:s}/rpc/", sv.substr(8));
    }
    else if (parse_port_string(sv, port))
    {
        // it was just a port
    }
    else if (auto const first_colon = sv.find(':'); first_colon == std::string_view::npos)
    {
        // it was a non-ipv6 host with no port
        host = sv;
    }
    else if (auto const last_colon = sv.rfind(':'); first_colon == last_colon)
    {
        // if only one colon, it's probably "$host:$port"
        if (parse_port_string(sv.substr(last_colon + 1), port))
        {
            host = sv.substr(0, last_colon);
        }
    }
    else
    {
        auto const is_unbracketed_ipv6 = !tr_strv_starts_with(sv, '[') && last_colon != std::string_view::npos;
        host = is_unbracketed_ipv6 ? fmt::format("[{:s}]", sv) : sv;
    }

    argc -= 1;

    for (int i = 1; i < argc; ++i)
    {
        argv[i] = argv[i + 1];
    }
}
} // namespace

int tr_main(int argc, char* argv[])
{
    auto const init_mgr = tr_lib_init();

    tr_locale_set_global("");

    auto config = RemoteConfig{};
    auto port = DefaultPort;
    auto host = std::string{};
    auto rpcurl = std::string{};

    if (argc < 2)
    {
        show_usage();
        return EXIT_FAILURE;
    }

    get_host_and_port_and_rpc_url(argc, argv, host, port, rpcurl, config);

    if (std::empty(host))
    {
        host = DefaultHost;
    }

    if (std::empty(rpcurl))
    {
        rpcurl = fmt::format("{:s}:{:d}{:s}", host, port, DefaultUrl);
    }

    return process_args(rpcurl.c_str(), argc, (char const* const*)argv, config);
}
