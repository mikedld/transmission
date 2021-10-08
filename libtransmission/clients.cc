/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

/* thanks amc1! */

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <ctype.h> /* isprint() */
#include <stdlib.h> /* strtol() */
#include <string.h>

#include "transmission.h"
#include "clients.h"
#include "utils.h" /* tr_snprintf(), tr_strlcpy() */

namespace
{

constexpr int charint(uint8_t ch)
{
    if ('0' <= ch && ch <= '9')
    {
        return ch - '0';
    }

    if ('A' <= ch && ch <= 'Z')
    {
        return 10 + ch - 'A';
    }

    if ('a' <= ch && ch <= 'z')
    {
        return 36 + ch - 'a';
    }

    return 0;
}

constexpr std::optional<int> getShadowInt(uint8_t ch)
{
    auto constexpr str = std::string_view{ "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.-" };
    auto const pos = str.find(ch);
    return pos != std::string_view::npos ? pos : std::optional<int>{};
}

constexpr std::optional<int> getFDMInt(uint8_t ch)
{
    auto constexpr str = std::string_view{ "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_.!~*()" };
    auto const pos = str.find(ch);
    return pos != std::string_view::npos ? pos : std::optional<int>{};
}

int strint(void const* pch, int span)
{
    char tmp[64];
    memcpy(tmp, pch, span);
    tmp[span] = '\0';
    return strtol(tmp, nullptr, 0);
}

constexpr char const* getMnemonicEnd(uint8_t ch)
{
    switch (ch)
    {
    case 'b':
    case 'B':
        return " (Beta)";

    case 'd':
        return " (Debug)";

    case 'x':
    case 'X':
    case 'Z':
        return " (Dev)";

    default:
        return "";
    }
}

void three_digits_formatter(char* buf, size_t buflen, std::string_view name, char const* digits)
{
    tr_snprintf(buf, buflen, "%*.*s %d.%d.%d", int(std::size(name)), int(std::size(name)), std::data(name), charint(digits[0]), charint(digits[1]), charint(digits[2]));
}

void three_digits(char* buf, size_t buflen, char const* name, uint8_t const* digits) // FIXME: should be removed when done
{
    tr_snprintf(buf, buflen, "%s %d.%d.%d", name, charint(digits[0]), charint(digits[1]), charint(digits[2]));
}

void four_digit_formatter(char* buf, size_t buflen, std::string_view name, char const* digits)
{
    tr_snprintf(
        buf,
        buflen,
        "%*.*s %d.%d.%d.%d",
        int(std::size(name)),
        int(std::size(name)),
        std::data(name),
        charint(digits[3]),
        charint(digits[4]),
        charint(digits[5]),
        charint(digits[6]));
}

void four_digits(char* buf, size_t buflen, char const* name, uint8_t const* digits) // FIXME: should be removed when done
{
    tr_snprintf(
        buf,
        buflen,
        "%s %d.%d.%d.%d",
        name,
        charint(digits[0]),
        charint(digits[1]),
        charint(digits[2]),
        charint(digits[3]));
}

void two_major_two_minor(char* buf, size_t buflen, char const* name, uint8_t const* digits)
{
    tr_snprintf(buf, buflen, "%s %d.%02d", name, strint(digits, 2), strint(digits + 2, 2));
}

void no_version(char* buf, size_t buflen, char const* name)
{
    tr_strlcpy(buf, name, buflen);
}

void mainline_style(char* buf, size_t buflen, char const* name, uint8_t const* id)
{
    if (id[4] == '-' && id[6] == '-')
    {
        tr_snprintf(buf, buflen, "%s %c.%c.%c", name, id[1], id[3], id[5]);
    }
    else if (id[5] == '-')
    {
        tr_snprintf(buf, buflen, "%s %c.%c%c.%c", name, id[1], id[3], id[4], id[6]);
    }
}

constexpr bool isMainlineStyle(uint8_t const* peer_id)
{
    /**
     * One of the following styles will be used:
     *   Mx-y-z--
     *   Mx-yy-z-
     */
    return peer_id[2] == '-' && peer_id[7] == '-' && (peer_id[4] == '-' || peer_id[5] == '-');
}

bool decodeBitCometClient(char* buf, size_t buflen, uint8_t const* id)
{
    char const* chid = (char const*)id;
    char const* mod = nullptr;

    if (strncmp(chid, "exbc", 4) == 0)
    {
        mod = "";
    }
    else if (strncmp(chid, "FUTB", 4) == 0)
    {
        mod = " (Solidox Mod) ";
    }
    else if (strncmp(chid, "xUTB", 4) == 0)
    {
        mod = " (Mod 2) ";
    }
    else
    {
        return false;
    }

    bool const is_bitlord = strncmp(chid + 6, "LORD", 4) == 0;
    char const* const name = (is_bitlord) ? "BitLord " : "BitComet ";
    int const major = id[4];
    int const minor = id[5];

    /**
     * Bitcomet, and older versions of BitLord, are of the form x.yy.
     * Bitcoment 1.0 and onwards are of the form x.y.
     */
    if (is_bitlord && major > 0)
    {
        tr_snprintf(buf, buflen, "%s%s%d.%d", name, mod, major, minor);
    }
    else
    {
        tr_snprintf(buf, buflen, "%s%s%d.%02d", name, mod, major, minor);
    }

    return true;
}

using format_func = void (*)(char* buf, size_t buflen, std::string_view name, char const* id);

void transmission_formatter(char* buf, size_t buflen, std::string_view name, char const* chid)
{
    if (strncmp(chid + 3, "000", 3) == 0) // very old client style: -TR0006- is 0.6
    {
        tr_snprintf(buf, buflen, "%*.*s 0.%c", int(std::size(name)), int(std::size(name)), std::data(name), chid[6]);
    }
    else if (strncmp(chid + 3, "00", 2) == 0) // previous client style: -TR0072- is 0.72
    {
        tr_snprintf(buf, buflen, "%*.*s 0.%02d", int(std::size(name)), int(std::size(name)), std::data(name), strint(chid + 5, 2));
    }
    else // current client style: -TR111Z- is 1.11+ */
    {
        tr_snprintf(
            buf,
            buflen,
            "%*.*s %d.%02d%s",
            int(std::size(name)),
            int(std::size(name)),
            std::data(name),
            strint(chid + 3, 1),
            strint(chid + 4, 2),
            (chid[6] == 'Z' || chid[6] == 'X') ? "+" : "");
    }
}

void ktorrent_formatter(char* buf, size_t buflen, std::string_view name, char const* id)
{
    if (id[5] == 'D')
    {
        tr_snprintf(buf, buflen, "%*.*s %d.%d Dev %d", int(std::size(name)), int(std::size(name)), std::data(name), charint(id[3]), charint(id[4]), charint(id[6]));
    }
    else if (id[5] == 'R')
    {
        tr_snprintf(buf, buflen, "%*.*s %d.%d RC %d", int(std::size(name)), int(std::size(name)), std::data(name), charint(id[3]), charint(id[4]), charint(id[6]));
    }
    else
    {
        three_digits_formatter(buf, buflen, name, id);
    }
}

void utorrent_formatter(char* buf, size_t buflen, std::string_view name, char const* id)
{
    if (id[7] == '-')
    {
        tr_snprintf(
            buf,
            buflen,
            "%*.*s %d.%d.%d%s",
            int(std::size(name)),
            int(std::size(name)),
            std::data(name),
            strint(id + 3, 1),
            strint(id + 4, 1),
            strint(id + 5, 1),
            getMnemonicEnd(id[6]));
    }
    else // uTorrent replaces the trailing dash with an extra digit for longer version numbers
    {
        tr_snprintf(
            buf,
            buflen,
            "%*.*s %d.%d.%d%s",
            int(std::size(name)),
            int(std::size(name)),
            std::data(name),
            strint(id + 3, 1),
            strint(id + 4, 1),
            strint(id + 5, 2),
            getMnemonicEnd(id[7]));
    }
}

struct Client
{
    std::string_view begins_with;
    std::string_view name;
    format_func formatter;
};

auto constexpr Clients = std::array<Client, 8>
{{
    { "-AZ", "Azureus / Vuze", four_digit_formatter },
    { "-BT", "BitTorrent", utorrent_formatter },
    { "-KT", "KTorrent", ktorrent_formatter },
    { "-TR", "Transmission", transmission_formatter },
    { "-UE", "\xc2\xb5Torrent Embedded", utorrent_formatter },
    { "-UM", "\xc2\xb5Torrent Mac", utorrent_formatter },
    { "-UT", "\xc2\xb5Torrent", utorrent_formatter },
    { "-UW", "\xc2\xb5Torrent Web", utorrent_formatter }
}};

} // namespace

#include <iostream> // FIXME do not commit

char* tr_clientForId(char* buf, size_t buflen, void const* id_in)
{
    auto const* id = static_cast<uint8_t const*>(id_in);
    auto const* chid = static_cast<char const*>(id_in);

    *buf = '\0';

    if (id == nullptr)
    {
        return buf;
    }

    struct Compare
    {
        bool operator()(std::string_view const& key, Client const& client) const
        {
            auto const key_lhs = std::string_view{ std::data(key), std::min(std::size(key), std::size(client.begins_with)) };
            auto const ret = key_lhs < client.begins_with;
            // std::cerr << "is [" << key_lhs << " less than " << client.begins_with << ' ' << ret << std::endl;
            return ret;
        }

        bool operator()(Client const& client, std::string_view const& key) const
        {
            auto const key_lhs = std::string_view{ std::data(key), std::min(std::size(key), std::size(client.begins_with)) };
            auto const ret = client.begins_with < key_lhs;
            // std::cerr << "is [" << client.begins_with << " less than " << key_lhs << ' ' << ret << std::endl;
            return ret;
        }
    };

    auto const key = std::string_view{ chid };
    auto const compare = Compare{};
#if 0
    auto constexpr compare = [](std::string_view const& key, Client const& client){
        auto const key_lhs = std::string_view { std::data(key), std::min(std::size(key), std::size(client.begins_with)) };
        return client.begins_with < key_lhs;
    };
#endif
    auto eq = std::equal_range(std::begin(Clients), std::end(Clients), key, compare);
    // std::cerr << "eq.first distance from begin " << std::distance(std::begin(Clients), eq.first) << std::endl;
    // std::cerr << "eq.second distance from begin " << std::distance(std::begin(Clients), eq.second) << std::endl;
    if (eq.first != std::end(Clients) && eq.first != eq.second)
    {
        eq.first->formatter(buf, buflen, eq.first->name, chid);
        std::cerr << "got a match [" << key << "] -> [" << buf << ']' << std::endl;
        return buf;
    }

    /* Azureus-style */
    if (id[0] == '-' && id[7] == '-')
    {
#if 0
        if (strncmp(chid + 1, "TR", 2) == 0)
        {
            if (strncmp(chid + 3, "000", 3) == 0) /* very old client style: -TR0006- is 0.6 */
            {
                tr_snprintf(buf, buflen, "Transmission 0.%c", id[6]);
            }
            else if (strncmp(chid + 3, "00", 2) == 0) /* previous client style: -TR0072- is 0.72 */
            {
                tr_snprintf(buf, buflen, "Transmission 0.%02d", strint(id + 5, 2));
            }
            else /* current client style: -TR111Z- is 1.11+ */
            {
                tr_snprintf(
                    buf,
                    buflen,
                    "Transmission %d.%02d%s",
                    strint(id + 3, 1),
                    strint(id + 4, 2),
                    (id[6] == 'Z' || id[6] == 'X') ? "+" : "");
            }
        }
        else if (strncmp(chid + 1, "UT", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "\xc2\xb5Torrent %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 1),
                getMnemonicEnd(id[6]));
        }
        else if (strncmp(chid + 1, "BT", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "BitTorrent %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 1),
                getMnemonicEnd(id[6]));
        }
        else if (strncmp(chid + 1, "UM", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "\xc2\xb5Torrent Mac %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 1),
                getMnemonicEnd(id[6]));
        }
        else if (strncmp(chid + 1, "UE", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "\xc2\xb5Torrent Embedded %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 1),
                getMnemonicEnd(id[6]));
        }
        else if (strncmp(chid + 1, "UW", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "\xc2\xb5Torrent Web %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 1),
                getMnemonicEnd(id[6]));
        }
        /* */
        else if (strncmp(chid + 1, "AZ", 2) == 0)
        {
            if (id[3] > '3' || (id[3] == '3' && id[4] >= '1')) /* Vuze starts at version 3.1.0.0 */
            {
                four_digits(buf, buflen, "Vuze", id + 3);
            }
            else
            {
                four_digits(buf, buflen, "Azureus", id + 3);
            }
        }
        /* */
        else if (strncmp(chid + 1, "KT", 2) == 0)
        {
            if (id[5] == 'D')
            {
                tr_snprintf(buf, buflen, "KTorrent %d.%d Dev %d", charint(id[3]), charint(id[4]), charint(id[6]));
            }
            else if (id[5] == 'R')
            {
                tr_snprintf(buf, buflen, "KTorrent %d.%d RC %d", charint(id[3]), charint(id[4]), charint(id[6]));
            }
            else
            {
                three_digits(buf, buflen, "KTorrent", id + 3);
            }
        }
        else
#endif
        /* */
        if (strncmp(chid + 1, "AG", 2) == 0)
        {
            four_digits(buf, buflen, "Ares", id + 3);
        }
        else if (strncmp(chid + 1, "AR", 2) == 0)
        {
            four_digits(buf, buflen, "Arctic", id + 3);
        }
        else if (strncmp(chid + 1, "AT", 2) == 0)
        {
            four_digits(buf, buflen, "Artemis", id + 3);
        }
        else if (strncmp(chid + 1, "AV", 2) == 0)
        {
            four_digits(buf, buflen, "Avicora", id + 3);
        }
        else if (strncmp(chid + 1, "BE", 2) == 0)
        {
            four_digits(buf, buflen, "BitTorrent SDK", id + 3);
        }
        else if (strncmp(chid + 1, "BG", 2) == 0)
        {
            four_digits(buf, buflen, "BTGetit", id + 3);
        }
        else if (strncmp(chid + 1, "BH", 2) == 0)
        {
            four_digits(buf, buflen, "BitZilla", id + 3);
        }
        else if (strncmp(chid + 1, "BI", 2) == 0)
        {
            four_digits(buf, buflen, "BiglyBT", id + 3);
        }
        else if (strncmp(chid + 1, "BM", 2) == 0)
        {
            four_digits(buf, buflen, "BitMagnet", id + 3);
        }
        else if (strncmp(chid + 1, "BP", 2) == 0)
        {
            four_digits(buf, buflen, "BitTorrent Pro (Azureus + Spyware)", id + 3);
        }
        else if (strncmp(chid + 1, "BX", 2) == 0)
        {
            four_digits(buf, buflen, "BittorrentX", id + 3);
        }
        else if (strncmp(chid + 1, "bk", 2) == 0)
        {
            four_digits(buf, buflen, "BitKitten (libtorrent)", id + 3);
        }
        else if (strncmp(chid + 1, "BS", 2) == 0)
        {
            four_digits(buf, buflen, "BTSlave", id + 3);
        }
        else if (strncmp(chid + 1, "BW", 2) == 0)
        {
            four_digits(buf, buflen, "BitWombat", id + 3);
        }
        else if (strncmp(chid + 1, "EB", 2) == 0)
        {
            four_digits(buf, buflen, "EBit", id + 3);
        }
        else if (strncmp(chid + 1, "DE", 2) == 0)
        {
            four_digits(buf, buflen, "Deluge", id + 3);
        }
        else if (strncmp(chid + 1, "DP", 2) == 0)
        {
            four_digits(buf, buflen, "Propagate Data Client", id + 3);
        }
        else if (strncmp(chid + 1, "FC", 2) == 0)
        {
            four_digits(buf, buflen, "FileCroc", id + 3);
        }
        else if (strncmp(chid + 1, "FT", 2) == 0)
        {
            four_digits(buf, buflen, "FoxTorrent/RedSwoosh", id + 3);
        }
        else if (strncmp(chid + 1, "GR", 2) == 0)
        {
            four_digits(buf, buflen, "GetRight", id + 3);
        }
        else if (strncmp(chid + 1, "GS", 2) == 0)
        {
            four_digits(buf, buflen, "GSTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "HK", 2) == 0)
        {
            four_digits(buf, buflen, "Hekate", id + 3);
        }
        else if (strncmp(chid + 1, "HN", 2) == 0)
        {
            four_digits(buf, buflen, "Hydranode", id + 3);
        }
        else if (strncmp(chid + 1, "KG", 2) == 0)
        {
            four_digits(buf, buflen, "KGet", id + 3);
        }
        else if (strncmp(chid + 1, "LC", 2) == 0)
        {
            four_digits(buf, buflen, "LeechCraft", id + 3);
        }
        else if (strncmp(chid + 1, "LH", 2) == 0)
        {
            four_digits(buf, buflen, "LH-ABC", id + 3);
        }
        else if (strncmp(chid + 1, "NX", 2) == 0)
        {
            four_digits(buf, buflen, "Net Transport", id + 3);
        }
        else if (strncmp(chid + 1, "MK", 2) == 0)
        {
            four_digits(buf, buflen, "Meerkat", id + 3);
        }
        else if (strncmp(chid + 1, "MO", 2) == 0)
        {
            four_digits(buf, buflen, "MonoTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "MR", 2) == 0)
        {
            four_digits(buf, buflen, "Miro", id + 3);
        }
        else if (strncmp(chid + 1, "MT", 2) == 0)
        {
            four_digits(buf, buflen, "Moonlight", id + 3);
        }
        else if (strncmp(chid + 1, "OS", 2) == 0)
        {
            four_digits(buf, buflen, "OneSwarm", id + 3);
        }
        else if (strncmp(chid + 1, "OT", 2) == 0)
        {
            four_digits(buf, buflen, "OmegaTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "PD", 2) == 0)
        {
            four_digits(buf, buflen, "Pando", id + 3);
        }
        else if (strncmp(chid + 1, "QD", 2) == 0)
        {
            four_digits(buf, buflen, "QQDownload", id + 3);
        }
        else if (strncmp(chid + 1, "RS", 2) == 0)
        {
            four_digits(buf, buflen, "Rufus", id + 3);
        }
        else if (strncmp(chid + 1, "RT", 2) == 0)
        {
            four_digits(buf, buflen, "Retriever", id + 3);
        }
        else if (strncmp(chid + 1, "RZ", 2) == 0)
        {
            four_digits(buf, buflen, "RezTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "SD", 2) == 0)
        {
            four_digits(buf, buflen, "Thunder", id + 3);
        }
        else if (strncmp(chid + 1, "SM", 2) == 0)
        {
            four_digits(buf, buflen, "SoMud", id + 3);
        }
        else if (strncmp(chid + 1, "SS", 2) == 0)
        {
            four_digits(buf, buflen, "SwarmScope", id + 3);
        }
        else if (strncmp(chid + 1, "ST", 2) == 0)
        {
            four_digits(buf, buflen, "SymTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "SZ", 2) == 0)
        {
            four_digits(buf, buflen, "Shareaza", id + 3);
        }
        else if (strncmp(chid + 1, "S~", 2) == 0)
        {
            four_digits(buf, buflen, "Shareaza", id + 3);
        }
        else if (strncmp(chid + 1, "st", 2) == 0)
        {
            four_digits(buf, buflen, "SharkTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "TN", 2) == 0)
        {
            four_digits(buf, buflen, "Torrent .NET", id + 3);
        }
        else if (strncmp(chid + 1, "TS", 2) == 0)
        {
            four_digits(buf, buflen, "TorrentStorm", id + 3);
        }
        else if (strncmp(chid + 1, "TT", 2) == 0)
        {
            four_digits(buf, buflen, "TuoTu", id + 3);
        }
        else if (strncmp(chid + 1, "UL", 2) == 0)
        {
            four_digits(buf, buflen, "uLeecher!", id + 3);
        }
        else if (strncmp(chid + 1, "VG", 2) == 0)
        {
            four_digits(buf, buflen, "Vagaa", id + 3);
        }
        else if (strncmp(chid + 1, "WT", 2) == 0)
        {
            four_digits(buf, buflen, "BitLet", id + 3);
        }
        else if (strncmp(chid + 1, "WY", 2) == 0)
        {
            four_digits(buf, buflen, "FireTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "WW", 2) == 0)
        {
            four_digits(buf, buflen, "WebTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "XL", 2) == 0)
        {
            four_digits(buf, buflen, "Xunlei", id + 3);
        }
        else if (strncmp(chid + 1, "XS", 2) == 0)
        {
            four_digits(buf, buflen, "XSwifter", id + 3);
        }
        else if (strncmp(chid + 1, "XT", 2) == 0)
        {
            four_digits(buf, buflen, "XanTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "XX", 2) == 0)
        {
            four_digits(buf, buflen, "Xtorrent", id + 3);
        }
        else if (strncmp(chid + 1, "ZT", 2) == 0)
        {
            four_digits(buf, buflen, "Zip Torrent", id + 3);
        }
        else if (strncmp(chid + 1, "ZO", 2) == 0)
        {
            four_digits(buf, buflen, "Zona", id + 3);
        }
        /* */
        else if (strncmp(chid + 1, "A~", 2) == 0)
        {
            three_digits(buf, buflen, "Ares", id + 3);
        }
        else if (strncmp(chid + 1, "ES", 2) == 0)
        {
            three_digits(buf, buflen, "Electric Sheep", id + 3);
        }
        else if (strncmp(chid + 1, "FW", 2) == 0)
        {
            three_digits(buf, buflen, "FrostWire", id + 3);
        }
        else if (strncmp(chid + 1, "HL", 2) == 0)
        {
            three_digits(buf, buflen, "Halite", id + 3);
        }
        else if (strncmp(chid + 1, "LT", 2) == 0)
        {
            three_digits(buf, buflen, "libtorrent (Rasterbar)", id + 3);
        }
        else if (strncmp(chid + 1, "lt", 2) == 0)
        {
            three_digits(buf, buflen, "libTorrent (Rakshasa)", id + 3);
        }
        else if (strncmp(chid + 1, "MP", 2) == 0)
        {
            three_digits(buf, buflen, "MooPolice", id + 3);
        }
        else if (strncmp(chid + 1, "pb", 2) == 0)
        {
            three_digits(buf, buflen, "pbTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "qB", 2) == 0)
        {
            three_digits(buf, buflen, "qBittorrent", id + 3);
        }
        /* */
        else if (strncmp(chid + 1, "AX", 2) == 0)
        {
            two_major_two_minor(buf, buflen, "BitPump", id + 3);
        }
        else if (strncmp(chid + 1, "BC", 2) == 0)
        {
            two_major_two_minor(buf, buflen, "BitComet", id + 3);
        }
        else if (strncmp(chid + 1, "CD", 2) == 0)
        {
            two_major_two_minor(buf, buflen, "Enhanced CTorrent", id + 3);
        }
        else if (strncmp(chid + 1, "LP", 2) == 0)
        {
            two_major_two_minor(buf, buflen, "Lphant", id + 3);
        }
        /* */
        else if (strncmp(chid + 1, "BF", 2) == 0)
        {
            no_version(buf, buflen, "BitFlu");
        }
        else if (strncmp(chid + 1, "LW", 2) == 0)
        {
            no_version(buf, buflen, "LimeWire");
        }
        /* */
        else if (strncmp(chid + 1, "BB", 2) == 0)
        {
            tr_snprintf(buf, buflen, "BitBuddy %c.%c%c%c", id[3], id[4], id[5], id[6]);
        }
        else if (strncmp(chid + 1, "BR", 2) == 0)
        {
            tr_snprintf(buf, buflen, "BitRocket %c.%c (%c%c)", id[3], id[4], id[5], id[6]);
        }
        else if (strncmp(chid + 1, "CT", 2) == 0)
        {
            tr_snprintf(buf, buflen, "CTorrent %d.%d.%02d", charint(id[3]), charint(id[4]), strint(id + 5, 2));
        }
        else if (strncmp(chid + 1, "XC", 2) == 0 || strncmp(chid + 1, "XX", 2) == 0)
        {
            tr_snprintf(buf, buflen, "Xtorrent %d.%d (%d)", charint(id[3]), charint(id[4]), strint(id + 5, 2));
        }
        else if (strncmp(chid + 1, "BOW", 3) == 0)
        {
            if (strncmp(&chid[4], "A0B", 3) == 0)
            {
                tr_snprintf(buf, buflen, "Bits on Wheels 1.0.5");
            }
            else if (strncmp(&chid[4], "A0C", 3) == 0)
            {
                tr_snprintf(buf, buflen, "Bits on Wheels 1.0.6");
            }
            else
            {
                tr_snprintf(buf, buflen, "Bits on Wheels %c.%c.%c", id[4], id[5], id[5]);
            }
        }
        else if (strncmp(chid + 1, "MG", 2) == 0)
        {
            tr_snprintf(buf, buflen, "MediaGet %d.%02d", charint(id[3]), charint(id[4]));
        }
        else if (strncmp(chid + 1, "XF", 2) == 0)
        {
            if (chid[6] == '0')
            {
                three_digits(buf, buflen, "Xfplay", id + 3);
            }
            else
            {
                tr_snprintf(buf, buflen, "Xfplay %d.%d.%d", strint(id + 3, 1), strint(id + 4, 1), strint(id + 5, 2));
            }
        }
        else if (strncmp(chid + 1, "PI", 2) == 0)
        {
            tr_snprintf(buf, buflen, "PicoTorrent %d.%d%d.%d", charint(id[3]), charint(id[4]), charint(id[5]), charint(id[6]));
        }
        else if (strncmp(chid + 1, "FD", 2) == 0)
        {
            auto const c = getFDMInt(id[5]);
            if (c)
            {
                tr_snprintf(buf, buflen, "Free Download Manager %d.%d.%d", charint(id[3]), charint(id[4]), *c);
            }
            else
            {
                tr_snprintf(buf, buflen, "Free Download Manager %d.%d.x", charint(id[3]), charint(id[4]));
            }
        }
        else if (strncmp(chid + 1, "FL", 2) == 0)
        {
            tr_snprintf(buf, buflen, "Folx %d.x", charint(id[3]));
        }
        else if (strncmp(chid + 1, "BN", 2) == 0)
        {
            tr_snprintf(buf, buflen, "Baidu Netdisk");
        }
        else if (strncmp(chid + 1, "WS", 2) == 0)
        {
            no_version(buf, buflen, "HTTP Seed");
        }

        if (!tr_str_is_empty(buf))
        {
            return buf;
        }
    }

#if 0
    /* uTorrent will replace the trailing dash with an extra digit for longer version numbers */
    if (id[0] == '-')
    {
        if (strncmp(chid + 1, "UT", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "\xc2\xb5Torrent %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 2),
                getMnemonicEnd(id[7]));
        }
        else if (strncmp(chid + 1, "UM", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "\xc2\xb5Torrent Mac %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 2),
                getMnemonicEnd(id[7]));
        }
        else if (strncmp(chid + 1, "UE", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "\xc2\xb5Torrent Embedded %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 2),
                getMnemonicEnd(id[7]));
        }
        else if (strncmp(chid + 1, "UW", 2) == 0)
        {
            tr_snprintf(
                buf,
                buflen,
                "\xc2\xb5Torrent Web %d.%d.%d%s",
                strint(id + 3, 1),
                strint(id + 4, 1),
                strint(id + 5, 2),
                getMnemonicEnd(id[7]));
        }

        if (!tr_str_is_empty(buf))
        {
            return buf;
        }
    }
#endif

    /* Mainline */
    if (isMainlineStyle(id))
    {
        if (*id == 'M')
        {
            mainline_style(buf, buflen, "BitTorrent", id);
        }

        if (*id == 'Q')
        {
            mainline_style(buf, buflen, "Queen Bee", id);
        }

        if (!tr_str_is_empty(buf))
        {
            return buf;
        }
    }

    if (decodeBitCometClient(buf, buflen, id))
    {
        return buf;
    }

    /* Clients with no version */
    if (strncmp(chid, "AZ2500BT", 8) == 0)
    {
        no_version(buf, buflen, "BitTyrant (Azureus Mod)");
    }
    else if (strncmp(chid, "LIME", 4) == 0)
    {
        no_version(buf, buflen, "Limewire");
    }
    else if (strncmp(chid, "martini", 7) == 0)
    {
        no_version(buf, buflen, "Martini Man");
    }
    else if (strncmp(chid, "Pando", 5) == 0)
    {
        no_version(buf, buflen, "Pando");
    }
    else if (strncmp(chid, "a00---0", 7) == 0)
    {
        no_version(buf, buflen, "Swarmy");
    }
    else if (strncmp(chid, "a02---0", 7) == 0)
    {
        no_version(buf, buflen, "Swarmy");
    }
    else if (strncmp(chid, "-G3", 3) == 0)
    {
        no_version(buf, buflen, "G3 Torrent");
    }
    else if (strncmp(chid, "10-------", 9) == 0)
    {
        no_version(buf, buflen, "JVtorrent");
    }
    else if (strncmp(chid, "346-", 4) == 0)
    {
        no_version(buf, buflen, "TorrentTopia");
    }
    else if (strncmp(chid, "eX", 2) == 0)
    {
        no_version(buf, buflen, "eXeem");
    }
    else if (strncmp(chid, "aria2-", 6) == 0)
    {
        no_version(buf, buflen, "aria2");
    }
    else if (strncmp(chid, "-WT-", 4) == 0)
    {
        no_version(buf, buflen, "BitLet");
    }
    else if (strncmp(chid, "-FG", 3) == 0)
    {
        two_major_two_minor(buf, buflen, "FlashGet", id + 3);
    }
    /* Everything else */
    else if (strncmp(chid, "S3", 2) == 0 && id[2] == '-' && id[4] == '-' && id[6] == '-')
    {
        tr_snprintf(buf, buflen, "Amazon S3 %c.%c.%c", id[3], id[5], id[7]);
    }
    else if (strncmp(chid, "OP", 2) == 0)
    {
        tr_snprintf(buf, buflen, "Opera (Build %c%c%c%c)", id[2], id[3], id[4], id[5]);
    }
    else if (strncmp(chid, "-ML", 3) == 0)
    {
        tr_snprintf(buf, buflen, "MLDonkey %c%c%c%c%c", id[3], id[4], id[5], id[6], id[7]);
    }
    else if (strncmp(chid, "DNA", 3) == 0)
    {
        tr_snprintf(buf, buflen, "BitTorrent DNA %d.%d.%d", strint(id + 3, 2), strint(id + 5, 2), strint(id + 7, 2));
    }
    else if (strncmp(chid, "Plus", 4) == 0)
    {
        tr_snprintf(buf, buflen, "Plus! v2 %c.%c%c", id[4], id[5], id[6]);
    }
    else if (strncmp(chid, "XBT", 3) == 0)
    {
        tr_snprintf(buf, buflen, "XBT Client %c.%c.%c%s", id[3], id[4], id[5], getMnemonicEnd(id[6]));
    }
    else if (strncmp(chid, "Mbrst", 5) == 0)
    {
        tr_snprintf(buf, buflen, "burst! %c.%c.%c", id[5], id[7], id[9]);
    }
    else if (strncmp(chid, "btpd", 4) == 0)
    {
        tr_snprintf(buf, buflen, "BT Protocol Daemon %c%c%c", id[5], id[6], id[7]);
    }
    else if (strncmp(chid, "BLZ", 3) == 0)
    {
        tr_snprintf(buf, buflen, "Blizzard Downloader %d.%d", id[3] + 1, id[4]);
    }
    else if (strncmp(chid, "-SP", 3) == 0)
    {
        three_digits(buf, buflen, "BitSpirit", id + 3);
    }
    else if ('\0' == id[0] && strncmp(chid + 2, "BS", 2) == 0)
    {
        tr_snprintf(buf, buflen, "BitSpirit %u", (id[1] == 0 ? 1 : id[1]));
    }
    else if (strncmp(chid, "QVOD", 4) == 0)
    {
        four_digits(buf, buflen, "QVOD", id + 4);
    }
    else if (strncmp(chid, "-NE", 3) == 0)
    {
        four_digits(buf, buflen, "BT Next Evolution", id + 3);
    }
    else if (strncmp(chid, "TIX", 3) == 0)
    {
        two_major_two_minor(buf, buflen, "Tixati", id + 3);
    }
    else if (strncmp(chid, "A2", 2) == 0)
    {
        if (id[4] == '-' && id[6] == '-' && id[8] == '-')
        {
            tr_snprintf(buf, buflen, "aria2 %c.%c.%c", id[3], id[5], id[7]);
        }
        else if (id[4] == '-' && id[7] == '-' && id[9] == '-')
        {
            tr_snprintf(buf, buflen, "aria2 %c.%c%c.%c", id[3], id[5], id[6], id[8]);
        }
        else
        {
            no_version(buf, buflen, "aria2");
        }
    }
    else if (strncmp(chid, "-BL", 3) == 0)
    {
        tr_snprintf(buf, buflen, "BitLord %c.%c.%c-%c%c%c", id[3], id[4], id[5], id[6], id[7], id[8]);
    }

    /* Shad0w-style */
    if (tr_str_is_empty(buf))
    {
        auto const a = getShadowInt(id[1]);
        auto const b = getShadowInt(id[2]);
        auto const c = getShadowInt(id[3]);

        if (strchr("AOQRSTU", id[0]) != nullptr && a && b && c)
        {
            char const* name = nullptr;

            switch (id[0])
            {
            case 'A':
                name = "ABC";
                break;

            case 'O':
                name = "Osprey";
                break;

            case 'Q':
                name = "BTQueue";
                break;

            case 'R':
                name = "Tribler";
                break;

            case 'S':
                name = "Shad0w";
                break;

            case 'T':
                name = "BitTornado";
                break;

            case 'U':
                name = "UPnP NAT Bit Torrent";
                break;
            }

            if (name != nullptr)
            {
                tr_snprintf(buf, buflen, "%s %d.%d.%d", name, *a, *b, *c);
                return buf;
            }
        }
    }

    /* No match */
    if (tr_str_is_empty(buf))
    {
        char out[32];
        char* walk = out;

        for (size_t i = 0; i < 8; ++i)
        {
            char const c = chid[i];

            if (isprint((unsigned char)c))
            {
                *walk++ = c;
            }
            else
            {
                tr_snprintf(walk, out + sizeof(out) - walk, "%%%02X", (unsigned int)c);
                walk += 3;
            }
        }

        *walk = '\0';
        tr_strlcpy(buf, out, buflen);
    }

    return buf;
}
