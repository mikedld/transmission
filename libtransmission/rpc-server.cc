// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring> /* memcpy */
#include <ctime>
#include <list>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h> /* TODO: eventually remove this */
#include <event2/listener.h>

#include <fmt/core.h>
#include <fmt/chrono.h>

#include <libdeflate.h>

#include "transmission.h"

#include "crypto-utils.h" /* tr_rand_buffer() */
#include "crypto.h" /* tr_ssha1_matches() */
#include "error.h"
#include "fdlimit.h"
#include "log.h"
#include "net.h"
#include "platform.h" /* tr_getWebClientDir() */
#include "quark.h"
#include "rpc-server.h"
#include "rpcimpl.h"
#include "session-id.h"
#include "session.h"
#include "tr-assert.h"
#include "tr-strbuf.h"
#include "trevent.h"
#include "utils.h"
#include "variant.h"
#include "web-utils.h"
#include "web.h"

using namespace std::literals;

/* session-id is used to make cross-site request forgery attacks difficult.
 * Don't disable this feature unless you really know what you're doing!
 * https://en.wikipedia.org/wiki/Cross-site_request_forgery
 * https://shiflett.org/articles/cross-site-request-forgeries
 * http://www.webappsec.org/lists/websecurity/archive/2008-04/msg00037.html */
#define REQUIRE_SESSION_ID

static auto constexpr TrUnixSocketPrefix = "unix:"sv;

/* The maximum size of a unix socket path is defined per-platform based on sockaddr_un.sun_path.
 * On Windows the fallback is the length of an ipv6 address. Subtracting one at the end is for
 * double counting null terminators from sun_path and TrUnixSocketPrefix. */
#ifdef _WIN32
auto inline constexpr TrUnixAddrStrLen = size_t{ INET6_ADDRSTRLEN };
#else
auto inline constexpr TrUnixAddrStrLen = size_t{ sizeof(((struct sockaddr_un*)nullptr)->sun_path) +
                                                 std::size(TrUnixSocketPrefix) };
#endif

enum tr_rpc_address_type
{
    TR_RPC_AF_INET,
    TR_RPC_AF_INET6,
    TR_RPC_AF_UNIX
};

struct tr_rpc_address
{
    tr_rpc_address_type type;
    union
    {
        struct in_addr addr4;
        struct in6_addr addr6;
        char unixSocketPath[TrUnixAddrStrLen];
    } addr;

    void set_inaddr_any()
    {
        type = TR_RPC_AF_INET;
        addr.addr4 = { INADDR_ANY };
    }
};

#define MY_REALM "Transmission"

static int constexpr DeflateLevel = 6; // medium / default

static bool constexpr tr_rpc_address_is_valid(tr_rpc_address const& a)
{
    return a.type == TR_RPC_AF_INET || a.type == TR_RPC_AF_INET6 || a.type == TR_RPC_AF_UNIX;
}

/***
****
***/

static char const* get_current_session_id(tr_rpc_server* server)
{
    return tr_session_id_get_current(server->session->session_id);
}

/**
***
**/

static void send_simple_response(struct evhttp_request* req, int code, char const* text)
{
    char const* code_text = tr_webGetResponseStr(code);
    struct evbuffer* body = evbuffer_new();

    evbuffer_add_printf(body, "<h1>%d: %s</h1>", code, code_text);

    if (text != nullptr)
    {
        evbuffer_add_printf(body, "%s", text);
    }

    evhttp_send_reply(req, code, code_text, body);

    evbuffer_free(body);
}

/***
****
***/

static char const* mimetype_guess(std::string_view path)
{
    // these are the ones we need for serving the web client's files...
    static auto constexpr Types = std::array<std::pair<std::string_view, char const*>, 7>{ {
        { ".css"sv, "text/css" },
        { ".gif"sv, "image/gif" },
        { ".html"sv, "text/html" },
        { ".ico"sv, "image/vnd.microsoft.icon" },
        { ".js"sv, "application/javascript" },
        { ".png"sv, "image/png" },
        { ".svg"sv, "image/svg+xml" },
    } };

    for (auto const& [suffix, mime_type] : Types)
    {
        if (tr_strvEndsWith(path, suffix))
        {
            return mime_type;
        }
    }

    return "application/octet-stream";
}

static void add_response(struct evhttp_request* req, tr_rpc_server* server, struct evbuffer* out, struct evbuffer* content)
{
    char const* key = "Accept-Encoding";
    char const* encoding = evhttp_find_header(req->input_headers, key);
    bool const do_compress = encoding != nullptr && strstr(encoding, "gzip") != nullptr;

    if (!do_compress)
    {
        evbuffer_add_buffer(out, content);
    }
    else
    {
        auto const* const content_ptr = evbuffer_pullup(content, -1);
        size_t const content_len = evbuffer_get_length(content);
        auto const max_compressed_len = libdeflate_deflate_compress_bound(server->compressor.get(), content_len);

        struct evbuffer_iovec iovec[1];
        evbuffer_reserve_space(out, std::max(content_len, max_compressed_len), iovec, 1);

        auto const compressed_len = libdeflate_gzip_compress(
            server->compressor.get(),
            content_ptr,
            content_len,
            iovec[0].iov_base,
            iovec[0].iov_len);
        if (0 < compressed_len && compressed_len < content_len)
        {
            iovec[0].iov_len = compressed_len;
            evhttp_add_header(req->output_headers, "Content-Encoding", "gzip");
        }
        else
        {
            std::copy_n(content_ptr, content_len, static_cast<char*>(iovec[0].iov_base));
            iovec[0].iov_len = content_len;
        }

        evbuffer_commit_space(out, iovec, 1);
    }
}

static void add_time_header(struct evkeyvalq* headers, char const* key, time_t now)
{
    // RFC 2616 says this must follow RFC 1123's date format, so use gmtime instead of localtime
    evhttp_add_header(headers, key, fmt::format("{:%a %b %d %T %Y%n}", fmt::gmtime(now)).c_str());
}

static void evbuffer_ref_cleanup_tr_free(void const* /*data*/, size_t /*datalen*/, void* extra)
{
    tr_free(extra);
}

static void serve_file(struct evhttp_request* req, tr_rpc_server* server, std::string_view filename)
{
    if (req->type != EVHTTP_REQ_GET)
    {
        evhttp_add_header(req->output_headers, "Allow", "GET");
        send_simple_response(req, 405, nullptr);
    }
    else
    {
        auto file_len = size_t{};
        tr_error* error = nullptr;
        void* const file = tr_loadFile(filename, &file_len, &error);

        if (file == nullptr)
        {
            auto const tmp = fmt::format(FMT_STRING("{:s} ({:s})"), filename, error->message);
            send_simple_response(req, HTTP_NOTFOUND, tmp.c_str());
            tr_error_free(error);
        }
        else
        {
            auto const now = tr_time();

            auto* const content = evbuffer_new();
            evbuffer_add_reference(content, file, file_len, evbuffer_ref_cleanup_tr_free, file);

            auto* const out = evbuffer_new();
            evhttp_add_header(req->output_headers, "Content-Type", mimetype_guess(filename));
            add_time_header(req->output_headers, "Date", now);
            add_time_header(req->output_headers, "Expires", now + (24 * 60 * 60));
            add_response(req, server, out, content);
            evhttp_send_reply(req, HTTP_OK, "OK", out);

            evbuffer_free(out);
            evbuffer_free(content);
        }
    }
}

static void handle_web_client(struct evhttp_request* req, tr_rpc_server* server)
{
    char const* webClientDir = tr_getWebClientDir(server->session);

    if (tr_str_is_empty(webClientDir))
    {
        send_simple_response(
            req,
            HTTP_NOTFOUND,
            "<p>Couldn't find Transmission's web interface files!</p>"
            "<p>Users: to tell Transmission where to look, "
            "set the TRANSMISSION_WEB_HOME environment "
            "variable to the folder where the web interface's "
            "index.html is located.</p>"
            "<p>Package Builders: to set a custom default at compile time, "
            "#define PACKAGE_DATA_DIR in libtransmission/platform.c "
            "or tweak tr_getClutchDir() by hand.</p>");
    }
    else
    {
        // TODO: string_view
        char* const subpath = tr_strdup(req->uri + std::size(server->url()) + 4);
        if (char* pch = strchr(subpath, '?'); pch != nullptr)
        {
            *pch = '\0';
        }

        if (strstr(subpath, "..") != nullptr)
        {
            send_simple_response(req, HTTP_NOTFOUND, "<p>Tsk, tsk.</p>");
        }
        else
        {
            auto const filename = tr_pathbuf{ webClientDir, "/"sv, tr_str_is_empty(subpath) ? "index.html" : subpath };
            serve_file(req, server, filename.sv());
        }

        tr_free(subpath);
    }
}

struct rpc_response_data
{
    struct evhttp_request* req;
    tr_rpc_server* server;
};

static void rpc_response_func(tr_session* /*session*/, tr_variant* response, void* user_data)
{
    auto* data = static_cast<struct rpc_response_data*>(user_data);
    struct evbuffer* response_buf = tr_variantToBuf(response, TR_VARIANT_FMT_JSON_LEAN);
    struct evbuffer* buf = evbuffer_new();

    add_response(data->req, data->server, buf, response_buf);
    evhttp_add_header(data->req->output_headers, "Content-Type", "application/json; charset=UTF-8");
    evhttp_send_reply(data->req, HTTP_OK, "OK", buf);

    evbuffer_free(buf);
    evbuffer_free(response_buf);
    tr_free(data);
}

static void handle_rpc_from_json(struct evhttp_request* req, tr_rpc_server* server, std::string_view json)
{
    auto top = tr_variant{};
    auto const have_content = tr_variantFromBuf(&top, TR_VARIANT_PARSE_JSON | TR_VARIANT_PARSE_INPLACE, json);

    auto* const data = tr_new0(struct rpc_response_data, 1);
    data->req = req;
    data->server = server;

    tr_rpc_request_exec_json(server->session, have_content ? &top : nullptr, rpc_response_func, data);

    if (have_content)
    {
        tr_variantFree(&top);
    }
}

static void handle_rpc(struct evhttp_request* req, tr_rpc_server* server)
{
    if (req->type == EVHTTP_REQ_POST)
    {
        auto json = std::string_view{ reinterpret_cast<char const*>(evbuffer_pullup(req->input_buffer, -1)),
                                      evbuffer_get_length(req->input_buffer) };
        handle_rpc_from_json(req, server, json);
        return;
    }

    if (req->type == EVHTTP_REQ_GET)
    {
        char const* q = strchr(req->uri, '?');

        if (q != nullptr)
        {
            auto* const data = tr_new0(struct rpc_response_data, 1);
            data->req = req;
            data->server = server;
            tr_rpc_request_exec_uri(server->session, q + 1, rpc_response_func, data);
            return;
        }
    }

    send_simple_response(req, 405, nullptr);
}

static bool isAddressAllowed(tr_rpc_server const* server, char const* address)
{
    if (!server->isWhitelistEnabled())
    {
        return true;
    }

    auto const& src = server->whitelist_;
    return std::any_of(std::begin(src), std::end(src), [&address](auto const& s) { return tr_wildmat(address, s); });
}

static bool isIPAddressWithOptionalPort(char const* host)
{
    struct sockaddr_storage address;
    int address_len = sizeof(address);

    /* TODO: move to net.{c,h} */
    return evutil_parse_sockaddr_port(host, (struct sockaddr*)&address, &address_len) != -1;
}

static bool isHostnameAllowed(tr_rpc_server const* server, struct evhttp_request* req)
{
    /* If password auth is enabled, any hostname is permitted. */
    if (server->isPasswordEnabled())
    {
        return true;
    }

    /* If whitelist is disabled, no restrictions. */
    if (!server->isHostWhitelistEnabled)
    {
        return true;
    }

    char const* const host = evhttp_find_header(req->input_headers, "Host");

    /* No host header, invalid request. */
    if (host == nullptr)
    {
        return false;
    }

    /* IP address is always acceptable. */
    if (isIPAddressWithOptionalPort(host))
    {
        return true;
    }

    /* Host header might include the port. */
    auto const hostname = std::string(host, strcspn(host, ":"));

    /* localhost is always acceptable. */
    if (hostname == "localhost" || hostname == "localhost.")
    {
        return true;
    }

    auto const& src = server->hostWhitelist;
    return std::any_of(
        std::begin(src),
        std::end(src),
        [&hostname](auto const& str) { return tr_wildmat(hostname.c_str(), str.c_str()); });
}

static bool test_session_id(tr_rpc_server* server, evhttp_request const* req)
{
    char const* ours = get_current_session_id(server);
    char const* theirs = evhttp_find_header(req->input_headers, TR_RPC_SESSION_ID_HEADER);
    bool const success = theirs != nullptr && strcmp(theirs, ours) == 0;
    return success;
}

static bool isAuthorized(tr_rpc_server const* server, char const* auth_header)
{
    if (!server->isPasswordEnabled())
    {
        return true;
    }

    // https://datatracker.ietf.org/doc/html/rfc7617
    // `Basic ${base64(username)}:${base64(password)}`

    auto constexpr Prefix = "Basic "sv;
    auto auth = std::string_view{ auth_header != nullptr ? auth_header : "" };
    if (!tr_strvStartsWith(auth, Prefix))
    {
        return false;
    }

    auth.remove_prefix(std::size(Prefix));
    auto const decoded_str = tr_base64_decode(auth);
    auto decoded = std::string_view{ decoded_str };
    auto const username = tr_strvSep(&decoded, ':');
    auto const password = decoded;
    return server->username() == username && tr_ssha1_matches(server->salted_password_, password);
}

static void handle_request(struct evhttp_request* req, void* arg)
{
    auto* server = static_cast<tr_rpc_server*>(arg);

    if (req != nullptr && req->evcon != nullptr)
    {
        evhttp_add_header(req->output_headers, "Server", MY_REALM);

        if (server->isAntiBruteForceEnabled() && server->login_attempts_ >= server->anti_brute_force_limit_)
        {
            send_simple_response(req, 403, "<p>Too many unsuccessful login attempts. Please restart transmission-daemon.</p>");
            return;
        }

        if (!isAddressAllowed(server, req->remote_host))
        {
            send_simple_response(
                req,
                403,
                "<p>Unauthorized IP Address.</p>"
                "<p>Either disable the IP address whitelist or add your address to it.</p>"
                "<p>If you're editing settings.json, see the 'rpc-whitelist' and 'rpc-whitelist-enabled' entries.</p>"
                "<p>If you're still using ACLs, use a whitelist instead. See the transmission-daemon manpage for details.</p>");
            return;
        }

        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");

        if (req->type == EVHTTP_REQ_OPTIONS)
        {
            char const* headers = evhttp_find_header(req->input_headers, "Access-Control-Request-Headers");
            if (headers != nullptr)
            {
                evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", headers);
            }

            evhttp_add_header(req->output_headers, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            send_simple_response(req, 200, "");
            return;
        }

        if (!isAuthorized(server, evhttp_find_header(req->input_headers, "Authorization")))
        {
            evhttp_add_header(req->output_headers, "WWW-Authenticate", "Basic realm=\"" MY_REALM "\"");
            if (server->isAntiBruteForceEnabled())
            {
                ++server->login_attempts_;
            }

            auto const unauthuser = fmt::format(
                FMT_STRING("<p>Unauthorized User. {:d} unsuccessful login attempts.</p>"),
                server->login_attempts_);
            send_simple_response(req, 401, unauthuser.c_str());
            return;
        }

        server->login_attempts_ = 0;

        auto uri = std::string_view{ req->uri };
        auto const location = tr_strvStartsWith(uri, server->url()) ? uri.substr(std::size(server->url())) : ""sv;

        if (std::empty(location) || location == "web"sv)
        {
            auto const new_location = fmt::format(FMT_STRING("{:s}web/"), server->url());
            evhttp_add_header(req->output_headers, "Location", new_location.c_str());
            send_simple_response(req, HTTP_MOVEPERM, nullptr);
        }
        else if (tr_strvStartsWith(location, "web/"sv))
        {
            handle_web_client(req, server);
        }
        else if (!isHostnameAllowed(server, req))
        {
            char const* const tmp =
                "<p>Transmission received your request, but the hostname was unrecognized.</p>"
                "<p>To fix this, choose one of the following options:"
                "<ul>"
                "<li>Enable password authentication, then any hostname is allowed.</li>"
                "<li>Add the hostname you want to use to the whitelist in settings.</li>"
                "</ul></p>"
                "<p>If you're editing settings.json, see the 'rpc-host-whitelist' and 'rpc-host-whitelist-enabled' entries.</p>"
                "<p>This requirement has been added to help prevent "
                "<a href=\"https://en.wikipedia.org/wiki/DNS_rebinding\">DNS Rebinding</a> "
                "attacks.</p>";
            send_simple_response(req, 421, tmp);
        }
#ifdef REQUIRE_SESSION_ID
        else if (!test_session_id(server, req))
        {
            char const* sessionId = get_current_session_id(server);
            auto const tmp = fmt::format(
                FMT_STRING("<p>Your request had an invalid session-id header.</p>"
                           "<p>To fix this, follow these steps:"
                           "<ol><li> When reading a response, get its X-Transmission-Session-Id header and remember it"
                           "<li> Add the updated header to your outgoing requests"
                           "<li> When you get this 409 error message, resend your request with the updated header"
                           "</ol></p>"
                           "<p>This requirement has been added to help prevent "
                           "<a href=\"https://en.wikipedia.org/wiki/Cross-site_request_forgery\">CSRF</a> "
                           "attacks.</p>"
                           "<p><code>{:s}: {:s}</code></p>"),
                TR_RPC_SESSION_ID_HEADER,
                sessionId);
            evhttp_add_header(req->output_headers, TR_RPC_SESSION_ID_HEADER, sessionId);
            evhttp_add_header(req->output_headers, "Access-Control-Expose-Headers", TR_RPC_SESSION_ID_HEADER);
            send_simple_response(req, 409, tmp.c_str());
        }
#endif
        else if (tr_strvStartsWith(location, "rpc"sv))
        {
            handle_rpc(req, server);
        }
        else
        {
            send_simple_response(req, HTTP_NOTFOUND, req->uri);
        }
    }
}

static auto constexpr ServerStartRetryCount = int{ 10 };
static auto constexpr ServerStartRetryDelayIncrement = int{ 5 };
static auto constexpr ServerStartRetryDelayStep = int{ 3 };
static auto constexpr ServerStartRetryMaxDelay = int{ 60 };

static char const* tr_rpc_address_to_string(tr_rpc_address const& addr, char* buf, size_t buflen)
{
    TR_ASSERT(tr_rpc_address_is_valid(addr));

    switch (addr.type)
    {
    case TR_RPC_AF_INET:
        return evutil_inet_ntop(AF_INET, &addr.addr, buf, buflen);

    case TR_RPC_AF_INET6:
        return evutil_inet_ntop(AF_INET6, &addr.addr, buf, buflen);

    case TR_RPC_AF_UNIX:
        tr_strlcpy(buf, addr.addr.unixSocketPath, buflen);
        return buf;

    default:
        return nullptr;
    }
}

static std::string tr_rpc_address_with_port(tr_rpc_server const* server)
{
    char addr_buf[TrUnixAddrStrLen];
    tr_rpc_address_to_string(*server->bindAddress, addr_buf, sizeof(addr_buf));

    std::string addr_port_str{ addr_buf };
    if (server->bindAddress->type != TR_RPC_AF_UNIX)
    {
        addr_port_str.append(":" + std::to_string(server->port().host()));
    }
    return addr_port_str;
}

static bool tr_rpc_address_from_string(tr_rpc_address& dst, std::string_view src)
{
    if (tr_strvStartsWith(src, TrUnixSocketPrefix))
    {
        if (std::size(src) >= TrUnixAddrStrLen)
        {
            tr_logAddError(fmt::format(
                _("Unix socket path must be fewer than {count} characters (including '{prefix}' prefix)"),
                fmt::arg("count", TrUnixAddrStrLen - 1),
                fmt::arg("prefix", TrUnixSocketPrefix)));
            return false;
        }

        dst.type = TR_RPC_AF_UNIX;
        tr_strlcpy(dst.addr.unixSocketPath, std::string{ src }.c_str(), TrUnixAddrStrLen);
        return true;
    }

    if (evutil_inet_pton(AF_INET, std::string{ src }.c_str(), &dst.addr) == 1)
    {
        dst.type = TR_RPC_AF_INET;
        return true;
    }

    if (evutil_inet_pton(AF_INET6, std::string{ src }.c_str(), &dst.addr) == 1)
    {
        dst.type = TR_RPC_AF_INET6;
        return true;
    }

    return false;
}

static bool bindUnixSocket(
    [[maybe_unused]] struct event_base* base,
    [[maybe_unused]] struct evhttp* httpd,
    [[maybe_unused]] char const* path,
    [[maybe_unused]] int socket_mode)
{
#ifdef _WIN32
    tr_logAddError(fmt::format(
        _("Unix sockets are unsupported on Windows. Please change '{key}' in your settings."),
        fmt::arg("key", tr_quark_get_string(TR_KEY_rpc_bind_address))));
    return false;
#else
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    tr_strlcpy(addr.sun_path, path + std::size(TrUnixSocketPrefix), sizeof(addr.sun_path));

    unlink(addr.sun_path);

    struct evconnlistener* lev = evconnlistener_new_bind(
        base,
        nullptr,
        nullptr,
        LEV_OPT_CLOSE_ON_FREE,
        -1,
        reinterpret_cast<sockaddr const*>(&addr),
        sizeof(addr));

    if (lev == nullptr)
    {
        return false;
    }

    if (chmod(addr.sun_path, (mode_t)socket_mode) != 0)
    {
        tr_logAddWarn(
            fmt::format(_("Couldn't set RPC socket mode to {mode:#o}, defaulting to 0755"), fmt::arg("mode", socket_mode)));
    }

    return evhttp_bind_listener(httpd, lev) != nullptr;
#endif
}

static void startServer(tr_rpc_server* server);

static void rpc_server_on_start_retry(evutil_socket_t /*fd*/, short /*type*/, void* context)
{
    startServer(static_cast<tr_rpc_server*>(context));
}

static int rpc_server_start_retry(tr_rpc_server* server)
{
    int retry_delay = (server->start_retry_counter / ServerStartRetryDelayStep + 1) * ServerStartRetryDelayIncrement;
    retry_delay = std::min(retry_delay, int{ ServerStartRetryMaxDelay });

    if (server->start_retry_timer == nullptr)
    {
        server->start_retry_timer = evtimer_new(server->session->event_base, rpc_server_on_start_retry, server);
    }

    tr_timerAdd(*server->start_retry_timer, retry_delay, 0);
    ++server->start_retry_counter;

    return retry_delay;
}

static void rpc_server_start_retry_cancel(tr_rpc_server* server)
{
    if (server->start_retry_timer != nullptr)
    {
        event_free(server->start_retry_timer);
        server->start_retry_timer = nullptr;
    }

    server->start_retry_counter = 0;
}

static void startServer(tr_rpc_server* server)
{
    if (server->httpd != nullptr)
    {
        return;
    }

    struct event_base* base = server->session->event_base;
    struct evhttp* httpd = evhttp_new(base);

    evhttp_set_allowed_methods(httpd, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_OPTIONS);

    auto const address = server->getBindAddress();
    auto const port = server->port();

    bool const success = server->bindAddress->type == TR_RPC_AF_UNIX ?
        bindUnixSocket(base, httpd, address.c_str(), server->socket_mode_) :
        (evhttp_bind_socket(httpd, address.c_str(), port.host()) != -1);

    auto const addr_port_str = tr_rpc_address_with_port(server);

    if (!success)
    {
        evhttp_free(httpd);

        if (server->start_retry_counter < ServerStartRetryCount)
        {
            int const retry_delay = rpc_server_start_retry(server);

            tr_logAddDebug(fmt::format("Couldn't bind to {}, retrying in {} seconds", addr_port_str, retry_delay));
            return;
        }

        tr_logAddError(fmt::format(
            ngettext(
                "Couldn't bind to {address} after {count} attempt, giving up",
                "Couldn't bind to {address} after {count} attempts, giving up",
                ServerStartRetryCount),
            fmt::arg("address", addr_port_str),
            fmt::arg("count", ServerStartRetryCount)));
    }
    else
    {
        evhttp_set_gencb(httpd, handle_request, server);
        server->httpd = httpd;

        tr_logAddInfo(fmt::format(_("Listening for RPC and Web requests on '{address}'"), fmt::arg("address", addr_port_str)));
    }

    rpc_server_start_retry_cancel(server);
}

static void stopServer(tr_rpc_server* server)
{
    auto const lock = server->session->unique_lock();

    rpc_server_start_retry_cancel(server);

    struct evhttp* httpd = server->httpd;

    if (httpd == nullptr)
    {
        return;
    }

    auto const address = server->getBindAddress();

    server->httpd = nullptr;
    evhttp_free(httpd);

    if (server->bindAddress->type == TR_RPC_AF_UNIX)
    {
        unlink(address.c_str() + std::size(TrUnixSocketPrefix));
    }

    tr_logAddInfo(fmt::format(
        _("Stopped listening for RPC and Web requests on '{address}'"),
        fmt::arg("address", tr_rpc_address_with_port(server))));
}

void tr_rpc_server::setEnabled(bool is_enabled)
{
    is_enabled_ = is_enabled;

    tr_runInEventThread(
        this->session,
        [this]()
        {
            if (!is_enabled_)
            {
                stopServer(this);
            }
            else
            {
                startServer(this);
            }
        });
}

static void restartServer(tr_rpc_server* const server)
{
    if (server->isEnabled())
    {
        stopServer(server);
        startServer(server);
    }
}

void tr_rpc_server::setPort(tr_port port) noexcept
{
    if (port_ == port)
    {
        return;
    }

    port_ = port;

    if (isEnabled())
    {
        tr_runInEventThread(session, restartServer, this);
    }
}

void tr_rpc_server::setUrl(std::string_view url)
{
    url_ = url;
    tr_logAddDebug(fmt::format(FMT_STRING("setting our URL to '{:s}'"), url_));
}

static auto parseWhitelist(std::string_view whitelist)
{
    auto list = std::vector<std::string>{};

    while (!std::empty(whitelist))
    {
        auto const pos = whitelist.find_first_of(" ,;"sv);
        auto const token = tr_strvStrip(whitelist.substr(0, pos));
        list.emplace_back(token);
        whitelist = pos == std::string_view::npos ? ""sv : whitelist.substr(pos + 1);

        if (token.find_first_of("+-"sv) != std::string_view::npos)
        {
            tr_logAddWarn(fmt::format(
                _("Added '{entry}' to host whitelist and it has a '+' or '-'! Are you using an old ACL by mistake?"),
                fmt::arg("entry", token)));
        }
        else
        {
            tr_logAddInfo(fmt::format(_("Added '{entry}' to host whitelist"), fmt::arg("entry", token)));
        }
    }

    return list;
}

void tr_rpc_server::setWhitelist(std::string_view sv)
{
    this->whitelist_str_ = sv;
    this->whitelist_ = parseWhitelist(sv);
}

/****
*****  PASSWORD
****/

void tr_rpc_server::setUsername(std::string_view username)
{
    username_ = username;
    tr_logAddDebug(fmt::format(FMT_STRING("setting our username to '{:s}'"), username_));
}

static bool isSalted(std::string_view password)
{
    return tr_ssha1_test(password);
}

void tr_rpc_server::setPassword(std::string_view password) noexcept
{
    salted_password_ = isSalted(password) ? password : tr_ssha1(password);

    tr_logAddDebug(fmt::format(FMT_STRING("setting our salted password to '{:s}'"), salted_password_));
}

void tr_rpc_server::setPasswordEnabled(bool enabled)
{
    is_password_enabled_ = enabled;
    tr_logAddDebug(fmt::format("setting password-enabled to '{}'", enabled));
}

std::string tr_rpc_server::getBindAddress() const
{
    auto buf = std::array<char, TrUnixAddrStrLen>{};
    return tr_rpc_address_to_string(*this->bindAddress, std::data(buf), std::size(buf));
}

void tr_rpc_server::setAntiBruteForceEnabled(bool enabled) noexcept
{
    is_anti_brute_force_enabled_ = enabled;

    if (!enabled)
    {
        login_attempts_ = 0;
    }
}

/****
*****  LIFE CYCLE
****/

static void missing_settings_key(tr_quark const q)
{
    tr_logAddDebug(fmt::format("Couldn't find settings key '{}'", tr_quark_get_string(q)));
}

tr_rpc_server::tr_rpc_server(tr_session* session_in, tr_variant* settings)
    : compressor{ libdeflate_alloc_compressor(DeflateLevel), libdeflate_free_compressor }
    , bindAddress(std::make_unique<struct tr_rpc_address>())
    , session{ session_in }
{
    auto boolVal = bool{};
    auto i = int64_t{};
    auto sv = std::string_view{};

    auto key = TR_KEY_rpc_enabled;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        this->is_enabled_ = boolVal;
    }

    key = TR_KEY_rpc_port;

    if (!tr_variantDictFindInt(settings, key, &i))
    {
        missing_settings_key(key);
    }
    else
    {
        this->port_.setHost(i);
    }

    key = TR_KEY_rpc_url;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
    }
    else if (std::empty(sv) || sv.back() != '/')
    {
        this->url_ = fmt::format(FMT_STRING("{:s}/"), sv);
    }
    else
    {
        this->url_ = sv;
    }

    key = TR_KEY_rpc_whitelist_enabled;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        this->setWhitelistEnabled(boolVal);
    }

    key = TR_KEY_rpc_host_whitelist_enabled;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        this->isHostWhitelistEnabled = boolVal;
    }

    key = TR_KEY_rpc_host_whitelist;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
    }
    else if (!std::empty(sv))
    {
        this->hostWhitelist = parseWhitelist(sv);
    }

    key = TR_KEY_rpc_authentication_required;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        this->setPasswordEnabled(boolVal);
    }

    key = TR_KEY_rpc_whitelist;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
    }
    else if (!std::empty(sv))
    {
        this->setWhitelist(sv);
    }

    key = TR_KEY_rpc_username;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
    }
    else
    {
        this->setUsername(sv);
    }

    key = TR_KEY_rpc_password;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
    }
    else
    {
        this->setPassword(sv);
    }

    key = TR_KEY_anti_brute_force_enabled;

    if (!tr_variantDictFindBool(settings, key, &boolVal))
    {
        missing_settings_key(key);
    }
    else
    {
        this->setAntiBruteForceEnabled(boolVal);
    }

    key = TR_KEY_anti_brute_force_threshold;

    if (!tr_variantDictFindInt(settings, key, &i))
    {
        missing_settings_key(key);
    }
    else
    {
        this->setAntiBruteForceLimit(i);
    }

    key = TR_KEY_rpc_socket_mode;
    bool is_missing_rpc_socket_mode_key = true;

    if (tr_variantDictFindStrView(settings, key, &sv))
    {
        /* Read the socket permission as a string representing an octal number. */
        is_missing_rpc_socket_mode_key = false;
        i = tr_parseNum<int>(sv, 8).value_or(tr_rpc_server::DefaultRpcSocketMode);
    }
    else if (tr_variantDictFindInt(settings, key, &i))
    {
        /* Or as a base 10 integer to remain compatible with the old settings format. */
        is_missing_rpc_socket_mode_key = false;
    }
    if (is_missing_rpc_socket_mode_key)
    {
        missing_settings_key(key);
    }
    else
    {
        this->socket_mode_ = i;
    }

    key = TR_KEY_rpc_bind_address;

    if (!tr_variantDictFindStrView(settings, key, &sv))
    {
        missing_settings_key(key);
        bindAddress->set_inaddr_any();
    }
    else if (!tr_rpc_address_from_string(*bindAddress, sv))
    {
        tr_logAddWarn(fmt::format(
            _("The '{key}' setting is '{value}' but must be an IPv4 or IPv6 address or a Unix socket path. Using default value '0.0.0.0'"),
            fmt::format("key", tr_quark_get_string(key)),
            fmt::format("value", sv)));
        bindAddress->set_inaddr_any();
    }

    if (bindAddress->type == TR_RPC_AF_UNIX)
    {
        this->setWhitelistEnabled(false);
        this->isHostWhitelistEnabled = false;
    }

    if (this->isEnabled())
    {
        auto const rpc_uri = tr_rpc_address_with_port(this) + this->url_;
        tr_logAddInfo(fmt::format(_("Serving RPC and Web requests on {address}"), fmt::arg("address", rpc_uri)));
        tr_runInEventThread(session, startServer, this);

        if (this->isWhitelistEnabled())
        {
            tr_logAddInfo(_("Whitelist enabled"));
        }

        if (this->isPasswordEnabled())
        {
            tr_logAddInfo(_("Password required"));
        }
    }

    char const* webClientDir = tr_getWebClientDir(this->session);
    if (!tr_str_is_empty(webClientDir))
    {
        tr_logAddInfo(fmt::format(_("Serving RPC and Web requests from '{path}'"), fmt::arg("path", webClientDir)));
    }
}

tr_rpc_server::~tr_rpc_server()
{
    stopServer(this);
}
