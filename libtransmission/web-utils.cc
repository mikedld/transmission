/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <event2/buffer.h>

#include "transmission.h"

#include "net.h"
#include "web-utils.h"
#include "utils.h"

using namespace std::literals;

/***
****
***/

bool tr_addressIsIP(char const* str)
{
    tr_address tmp;
    return tr_address_from_string(&tmp, str);
}

char const* tr_webGetResponseStr(long code)
{
    switch (code)
    {
    case 0:
        return "No Response";

    case 101:
        return "Switching Protocols";

    case 200:
        return "OK";

    case 201:
        return "Created";

    case 202:
        return "Accepted";

    case 203:
        return "Non-Authoritative Information";

    case 204:
        return "No Content";

    case 205:
        return "Reset Content";

    case 206:
        return "Partial Content";

    case 300:
        return "Multiple Choices";

    case 301:
        return "Moved Permanently";

    case 302:
        return "Found";

    case 303:
        return "See Other";

    case 304:
        return "Not Modified";

    case 305:
        return "Use Proxy";

    case 306:
        return " (Unused)";

    case 307:
        return "Temporary Redirect";

    case 400:
        return "Bad Request";

    case 401:
        return "Unauthorized";

    case 402:
        return "Payment Required";

    case 403:
        return "Forbidden";

    case 404:
        return "Not Found";

    case 405:
        return "Method Not Allowed";

    case 406:
        return "Not Acceptable";

    case 407:
        return "Proxy Authentication Required";

    case 408:
        return "Request Timeout";

    case 409:
        return "Conflict";

    case 410:
        return "Gone";

    case 411:
        return "Length Required";

    case 412:
        return "Precondition Failed";

    case 413:
        return "Request Entity Too Large";

    case 414:
        return "Request-URI Too Long";

    case 415:
        return "Unsupported Media Type";

    case 416:
        return "Requested Range Not Satisfiable";

    case 417:
        return "Expectation Failed";

    case 421:
        return "Misdirected Request";

    case 500:
        return "Internal Server Error";

    case 501:
        return "Not Implemented";

    case 502:
        return "Bad Gateway";

    case 503:
        return "Service Unavailable";

    case 504:
        return "Gateway Timeout";

    case 505:
        return "HTTP Version Not Supported";

    default:
        return "Unknown Error";
    }
}

void tr_http_escape(struct evbuffer* out, std::string_view str, bool escape_reserved)
{
    auto constexpr ReservedChars = std::string_view{ "!*'();:@&=+$,/?%#[]" };
    auto constexpr UnescapedChars = std::string_view{ "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.~" };

    for (auto& ch : str)
    {
        if (tr_strvContains(UnescapedChars, ch) || (tr_strvContains(ReservedChars, ch) && !escape_reserved))
        {
            evbuffer_add_printf(out, "%c", ch);
        }
        else
        {
            evbuffer_add_printf(out, "%%%02X", (unsigned)(ch & 0xFF));
        }
    }
}

void tr_http_escape(std::string& appendme, std::string_view str, bool escape_reserved)
{
    auto constexpr ReservedChars = std::string_view{ "!*'();:@&=+$,/?%#[]" };
    auto constexpr UnescapedChars = std::string_view{ "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.~" };

    for (auto& ch : str)
    {
        if (tr_strvContains(UnescapedChars, ch) || (!escape_reserved && tr_strvContains(ReservedChars, ch)))
        {
            appendme += ch;
        }
        else
        {
            char buf[16];
            tr_snprintf(buf, sizeof(buf), "%%%02X", (unsigned)(ch & 0xFF));
            appendme += buf;
        }
    }
}

static bool is_rfc2396_alnum(uint8_t ch)
{
    return ('0' <= ch && ch <= '9') || ('A' <= ch && ch <= 'Z') || ('a' <= ch && ch <= 'z') || ch == '.' || ch == '-' ||
        ch == '_' || ch == '~';
}

void tr_http_escape_sha1(char* out, tr_sha1_digest_t const& digest)
{
    for (auto const b : digest)
    {
        if (is_rfc2396_alnum(uint8_t(b)))
        {
            *out++ = (char)b;
        }
        else
        {
            out += tr_snprintf(out, 4, "%%%02x", (unsigned int)b);
        }
    }

    *out = '\0';
}

void tr_http_escape_sha1(char* out, uint8_t const* sha1_digest)
{
    auto digest = tr_sha1_digest_t{};
    std::copy_n(reinterpret_cast<std::byte const*>(sha1_digest), std::size(digest), std::begin(digest));
    tr_http_escape_sha1(out, digest);
}

//// URLs

namespace
{

auto parsePort(std::string_view port_sv)
{
    auto const port = tr_parseNum<int>(port_sv);

    return port && *port >= std::numeric_limits<tr_port>::min() && *port <= std::numeric_limits<tr_port>::max() ? *port : -1;
}

constexpr std::string_view getPortForScheme(std::string_view scheme)
{
    auto constexpr KnownSchemes = std::array<std::pair<std::string_view, std::string_view>, 5>{ {
        { "ftp"sv, "21"sv },
        { "http"sv, "80"sv },
        { "https"sv, "443"sv },
        { "sftp"sv, "22"sv },
        { "udp"sv, "80"sv },
    } };

    for (auto const& [known_scheme, port] : KnownSchemes)
    {
        if (scheme == known_scheme)
        {
            return port;
        }
    }

    return "-1"sv;
}

bool urlCharsAreValid(std::string_view url)
{
    // rfc2396
    auto constexpr ValidChars = std::string_view{
        "abcdefghijklmnopqrstuvwxyz" // lowalpha
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ" // upalpha
        "0123456789" // digit
        "-_.!~*'()" // mark
        ";/?:@&=+$," // reserved
        "<>#%<\"" // delims
        "{}|\\^[]`" // unwise
    };

    return !std::empty(url) &&
        std::all_of(std::begin(url), std::end(url), [&ValidChars](auto ch) { return tr_strvContains(ValidChars, ch); });
}

bool tr_isValidTrackerScheme(std::string_view scheme)
{
    auto constexpr Schemes = std::array<std::string_view, 3>{ "http"sv, "https"sv, "udp"sv };
    return std::find(std::begin(Schemes), std::end(Schemes), scheme) != std::end(Schemes);
}

} // namespace

std::optional<tr_url_parsed_t> tr_urlParse(std::string_view url)
{
    url = tr_strvStrip(url);

    auto parsed = tr_url_parsed_t{};
    parsed.full = url;

    // So many magnet links are malformed, e.g. not escaping text
    // in the display name, that we're better off handling magnets
    // as a special case before even scanning for invalid chars.
    auto constexpr MagnetStart = "magnet:?"sv;
    if (tr_strvStartsWith(url, MagnetStart))
    {
        parsed.scheme = "magnet"sv;
        parsed.query = url.substr(std::size(MagnetStart));
        return parsed;
    }

    if (!urlCharsAreValid(url))
    {
        return std::nullopt;
    }

    // scheme
    parsed.scheme = tr_strvSep(&url, ':');
    if (std::empty(parsed.scheme))
    {
        return std::nullopt;
    }

    // authority
    // The authority component is preceded by a double slash ("//") and is
    // terminated by the next slash ("/"), question mark ("?"), or number
    // sign ("#") character, or by the end of the URI.
    if (auto key = "//"sv; tr_strvStartsWith(url, key))
    {
        url.remove_prefix(std::size(key));
        auto pos = url.find_first_of("/?#");
        parsed.authority = url.substr(0, pos);
        url = pos == std::string_view::npos ? ""sv : url.substr(pos);

        auto remain = parsed.authority;
        parsed.host = tr_strvSep(&remain, ':');
        parsed.portstr = !std::empty(remain) ? remain : getPortForScheme(parsed.scheme);
        parsed.port = parsePort(parsed.portstr);
    }

    //  The path is terminated by the first question mark ("?") or
    //  number sign ("#") character, or by the end of the URI.
    auto pos = url.find_first_of("?#");
    parsed.path = url.substr(0, pos);
    url = pos == std::string_view::npos ? ""sv : url.substr(pos);

    // query
    if (tr_strvStartsWith(url, '?'))
    {
        url.remove_prefix(1);
        pos = url.find('#');
        parsed.query = url.substr(0, pos);
        url = pos == std::string_view::npos ? ""sv : url.substr(pos);
    }

    // fragment
    if (tr_strvStartsWith(url, '#'))
    {
        parsed.fragment = url.substr(1);
    }

    return parsed;
}

std::optional<tr_url_parsed_t> tr_urlParseTracker(std::string_view url)
{
    auto const parsed = tr_urlParse(url);
    return parsed && tr_isValidTrackerScheme(parsed->scheme) ? std::make_optional(*parsed) : std::nullopt;
}

bool tr_urlIsValidTracker(std::string_view url)
{
    return !!tr_urlParseTracker(url);
}

bool tr_urlIsValid(std::string_view url)
{
    auto constexpr Schemes = std::array<std::string_view, 5>{ "http"sv, "https"sv, "ftp"sv, "sftp"sv, "udp"sv };
    auto const parsed = tr_urlParse(url);
    return parsed && std::find(std::begin(Schemes), std::end(Schemes), parsed->scheme) != std::end(Schemes);
}

tr_url_query_view::iterator& tr_url_query_view::iterator::operator++()
{
    auto pair = tr_strvSep(&remain, '&');
    keyval.first = tr_strvSep(&pair, '=');
    keyval.second = pair;
    return *this;
}

tr_url_query_view::iterator tr_url_query_view::begin() const
{
    auto it = iterator{};
    it.remain = query;
    ++it;
    return it;
}

std::string tr_urlPercentDecode(std::string_view in)
{
    auto out = std::string{};
    out.reserve(std::size(in));

    for (;;)
    {
        auto pos = in.find('%');
        out += in.substr(0, pos);
        if (pos == std::string_view::npos)
        {
            break;
        }

        in.remove_prefix(pos);
        if (std::size(in) >= 3 && in[0] == '%' && std::isxdigit(in[1]) && std::isxdigit(in[2]))
        {
            auto hexstr = std::array<char, 3>{ in[1], in[2], '\0' };
            auto const hex = strtoul(std::data(hexstr), nullptr, 16);
            out += char(hex);
            in.remove_prefix(3);
        }
        else
        {
            out += in.front();
            in.remove_prefix(1);
        }
    }

    return out;
}
