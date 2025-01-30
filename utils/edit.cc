// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstdio> // stderr
#include <cstdlib> // EXIT_FAILURE
#include <string>
#include <string_view>
#include <vector>

#include <fmt/core.h>

#include <libtransmission/error.h>
#include <libtransmission/log.h>
#include <libtransmission/quark.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>

#include <utils/tools.h>

namespace
{
char constexpr MyName[] = "transmission-edit";
char constexpr Usage[] = "Usage: transmission-edit [options] torrent-file(s)";

struct app_options
{
    std::vector<std::string_view> files;
    char const* add = nullptr;
    char const* deleteme = nullptr;
    std::array<char const*, 2> replace;
    char const* source = nullptr;
    bool show_version = false;
};

auto constexpr Options = std::array<tr_option, 6>{
    { { 'a', "add", "Add a tracker's announce URL", "a", true, "<url>" },
      { 'd', "delete", "Delete a tracker's announce URL", "d", true, "<url>" },
      { 'r', "replace", "Search and replace a substring in the announce URLs", "r", true, "<old> <new>" },
      { 's', "source", "Set the source", "s", true, "<source>" },
      { 'V', "version", "Show version number and exit", "V", false, nullptr },
      { 0, nullptr, nullptr, nullptr, false, nullptr } }
};

int parseCommandLine(app_options& opts, int argc, char const* const* argv)
{
    int c;
    char const* optarg;

    while ((c = tr_getopt(Usage, argc, argv, std::data(Options), &optarg)) != TR_OPT_DONE)
    {
        switch (c)
        {
        case 'a':
            opts.add = optarg;
            break;

        case 'd':
            opts.deleteme = optarg;
            break;

        case 'r':
            opts.replace[0] = optarg;
            c = tr_getopt(Usage, argc, argv, std::data(Options), &optarg);

            if (c != TR_OPT_UNK)
            {
                return 1;
            }

            opts.replace[1] = optarg;
            break;

        case 's':
            opts.source = optarg;
            break;

        case 'V':
            opts.show_version = true;
            break;

        case TR_OPT_UNK:
            opts.files.push_back(optarg);
            break;

        default:
            return 1;
        }
    }

    return 0;
}

bool removeURL(tr_variant* metainfo, std::string_view url)
{
    auto sv = std::string_view{};
    tr_variant* announce_list;
    bool changed = false;

    if (tr_variantDictFindStrView(metainfo, TR_KEY_announce, &sv) && url == sv)
    {
        fmt::print("\tRemoved '{:s}' from 'announce'\n", sv);
        tr_variantDictRemove(metainfo, TR_KEY_announce);
        changed = true;
    }

    if (tr_variantDictFindList(metainfo, TR_KEY_announce_list, &announce_list))
    {
        tr_variant* tier;
        int tierIndex = 0;

        while ((tier = tr_variantListChild(announce_list, tierIndex)) != nullptr)
        {
            int nodeIndex = 0;
            tr_variant const* node;
            while ((node = tr_variantListChild(tier, nodeIndex)) != nullptr)
            {
                if (tr_variantGetStrView(node, &sv) && url == sv)
                {
                    fmt::print("\tRemoved '{:s}' from 'announce-list' tier #{:d}\n", sv, tierIndex + 1);
                    tr_variantListRemove(tier, nodeIndex);
                    changed = true;
                }
                else
                {
                    ++nodeIndex;
                }
            }

            if (tr_variantListSize(tier) == 0)
            {
                fmt::print("\tNo URLs left in tier #{:d}... removing tier\n", tierIndex + 1);
                tr_variantListRemove(announce_list, tierIndex);
            }
            else
            {
                ++tierIndex;
            }
        }

        if (tr_variantListSize(announce_list) == 0)
        {
            fmt::print("\tNo tiers left... removing announce-list\n");
            tr_variantDictRemove(metainfo, TR_KEY_announce_list);
        }
    }

    /* if we removed the "announce" field and there's still another track left,
     * use it as the "announce" field */
    if (changed && !tr_variantDictFindStrView(metainfo, TR_KEY_announce, &sv))
    {
        tr_variant* const tier = tr_variantListChild(announce_list, 0);
        if (tier != nullptr)
        {
            tr_variant const* const node = tr_variantListChild(tier, 0);
            if ((node != nullptr) && tr_variantGetStrView(node, &sv))
            {
                tr_variantDictAddStr(metainfo, TR_KEY_announce, sv);
                fmt::print("\tAdded '{:s}' to announce\n", sv);
            }
        }
    }

    return changed;
}

[[nodiscard]] auto replaceSubstr(std::string_view str, std::string_view oldval, std::string_view newval)
{
    auto ret = std::string{};

    while (!std::empty(str))
    {
        auto const pos = str.find(oldval);
        ret += str.substr(0, pos);
        if (pos == std::string_view::npos)
        {
            break;
        }
        ret += newval;
        str.remove_prefix(pos + std::size(oldval));
    }

    return ret;
}

bool replaceURL(tr_variant* metainfo, std::string_view oldval, std::string_view newval)
{
    auto sv = std::string_view{};
    tr_variant* announce_list;
    bool changed = false;

    if (tr_variantDictFindStrView(metainfo, TR_KEY_announce, &sv) && tr_strv_contains(sv, oldval))
    {
        auto const newstr = replaceSubstr(sv, oldval, newval);
        fmt::print("\tReplaced in 'announce': '{:s}' --> '{:s}'\n", sv, newstr);
        tr_variantDictAddStr(metainfo, TR_KEY_announce, newstr);
        changed = true;
    }

    if (tr_variantDictFindList(metainfo, TR_KEY_announce_list, &announce_list))
    {
        tr_variant* tier;
        int tierCount = 0;

        while ((tier = tr_variantListChild(announce_list, tierCount)) != nullptr)
        {
            tr_variant* node;
            int nodeCount = 0;

            while ((node = tr_variantListChild(tier, nodeCount)) != nullptr)
            {
                if (tr_variantGetStrView(node, &sv) && tr_strv_contains(sv, oldval))
                {
                    auto const newstr = replaceSubstr(sv, oldval, newval);
                    fmt::print("\tReplaced in 'announce-list' tier #{:d}: '{:s}' --> '{:s}'\n", tierCount + 1, sv, newstr);
                    node->clear();
                    *node = newstr;
                    changed = true;
                }

                ++nodeCount;
            }

            ++tierCount;
        }
    }

    return changed;
}

[[nodiscard]] bool announce_list_has_url(tr_variant* announce_list, char const* url)
{
    int tierCount = 0;
    tr_variant* tier;

    while ((tier = tr_variantListChild(announce_list, tierCount)) != nullptr)
    {
        int nodeCount = 0;
        tr_variant const* node;

        while ((node = tr_variantListChild(tier, nodeCount)) != nullptr)
        {
            if (auto sv = std::string_view{}; tr_variantGetStrView(node, &sv) && sv == url)
            {
                return true;
            }

            ++nodeCount;
        }

        ++tierCount;
    }

    return false;
}

bool addURL(tr_variant* metainfo, char const* url)
{
    auto announce = std::string_view{};
    tr_variant* announce_list = nullptr;
    bool changed = false;
    bool const had_announce = tr_variantDictFindStrView(metainfo, TR_KEY_announce, &announce);
    bool const had_announce_list = tr_variantDictFindList(metainfo, TR_KEY_announce_list, &announce_list);

    if (!had_announce && !had_announce_list)
    {
        /* this new tracker is the only one, so add it to "announce"... */
        fmt::print("\tAdded '{:s}' in 'announce'\n", url);
        tr_variantDictAddStr(metainfo, TR_KEY_announce, url);
        changed = true;
    }
    else
    {
        if (!had_announce_list)
        {
            announce_list = tr_variantDictAddList(metainfo, TR_KEY_announce_list, 2);

            if (had_announce)
            {
                /* we're moving from an 'announce' to an 'announce-list',
                 * so copy the old announce URL to the list */
                tr_variant* tier = tr_variantListAddList(announce_list, 1);
                tr_variantListAddStr(tier, announce);
                changed = true;
            }
        }

        /* If the user-specified URL isn't in the announce list yet, add it */
        if (!announce_list_has_url(announce_list, url))
        {
            tr_variant* tier = tr_variantListAddList(announce_list, 1);
            tr_variantListAddStr(tier, url);
            fmt::print("\tAdded '{:s}' to 'announce-list' tier #{:d}\n", url, tr_variantListSize(announce_list));
            changed = true;
        }
    }

    return changed;
}

bool setSource(tr_variant* metainfo, char const* source_value)
{
    auto current_source = std::string_view{};
    bool const had_source = tr_variantDictFindStrView(metainfo, TR_KEY_source, &current_source);
    bool changed = false;

    if (!had_source)
    {
        fmt::print("\tAdded '{:s}' as source\n", source_value);
        tr_variantDictAddStr(metainfo, TR_KEY_source, source_value);
        changed = true;
    }
    else if (current_source.compare(source_value) != 0)
    {
        fmt::print("\tUpdated source: '{:s}' -> '{:s}'\n", current_source.data(), source_value);
        tr_variantDictAddStr(metainfo, TR_KEY_source, source_value);
        changed = true;
    }

    return changed;
}
} // namespace

static
int do_tr_edit(int argc, char* argv[])
{
    tr_locale_set_global("");

    int changedCount = 0;

    tr_logSetLevel(TR_LOG_ERROR);

    auto options = app_options{};
    if (parseCommandLine(options, argc, (char const* const*)argv) != 0)
    {
        return EXIT_FAILURE;
    }

    if (options.show_version)
    {
        fmt::print(stderr, "{:s} {:s}\n", MyName, LONG_VERSION_STRING);
        return EXIT_SUCCESS;
    }

    if (std::empty(options.files))
    {
        fmt::print(stderr, "ERROR: No torrent files specified.\n");
        tr_getopt_usage(MyName, Usage, std::data(Options));
        fmt::print(stderr, "\n");
        return EXIT_FAILURE;
    }

    if (options.add == nullptr && options.deleteme == nullptr && options.replace[0] == nullptr && options.source == nullptr)
    {
        fmt::print(stderr, "ERROR: Must specify -a, -d, -r or -s\n");
        tr_getopt_usage(MyName, Usage, std::data(Options));
        fmt::print(stderr, "\n");
        return EXIT_FAILURE;
    }

    auto serde = tr_variant_serde::benc();
    for (auto const& filename : options.files)
    {
        bool changed = false;

        fmt::print("{:s}\n", filename);

        auto otop = serde.parse_file(filename);
        if (!otop)
        {
            fmt::print("\tError reading file: {:s}\n", serde.error_.message());
            continue;
        }
        auto& top = *otop;

        if (options.deleteme != nullptr)
        {
            changed |= removeURL(&top, options.deleteme);
        }

        if (options.add != nullptr)
        {
            changed = addURL(&top, options.add);
        }

        if (options.replace[0] != nullptr && options.replace[1] != nullptr)
        {
            changed |= replaceURL(&top, options.replace[0], options.replace[1]);
        }

        if (options.source != nullptr)
        {
            changed = setSource(&top, options.source);
        }

        if (changed)
        {
            ++changedCount;
            serde.to_file(top, filename);
        }
    }

    fmt::print("Changed {:d} files\n", changedCount);

    return EXIT_SUCCESS;
}

struct tr_cmd tr_edit = {
	.name = "edit",
	.cmd = do_tr_edit,
};
