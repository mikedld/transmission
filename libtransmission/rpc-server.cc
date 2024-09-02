// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring> /* for strcspn() */
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/http_struct.h> /* TODO: eventually remove this */
#include <event2/listener.h>

#include <fmt/core.h>
#include <fmt/chrono.h>

#include <libdeflate.h>

#include "libtransmission/transmission.h"

#include "libtransmission/crypto-utils.h" /* tr_ssha1_matches() */
#include "libtransmission/error.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/platform.h" /* tr_getWebClientDir() */
#include "libtransmission/quark.h"
#include "libtransmission/rpc-server.h"
#include "libtransmission/rpcimpl.h"
#include "libtransmission/session.h"
#include "libtransmission/timer.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"
#include "libtransmission/web-utils.h"

struct evbuffer;

/* session_id is used to make cross-site request forgery attacks difficult.
 * Don't disable this feature unless you really know what you're doing!
 * https://en.wikipedia.org/wiki/Cross-site_request_forgery
 * https://shiflett.org/articles/cross-site-request-forgeries
 * http://www.webappsec.org/lists/websecurity/archive/2008-04/msg00037.html */
#define REQUIRE_SESSION_ID

#define MY_REALM "Transmission"

using namespace std::literals;

namespace
{
auto constexpr TrUnixSocketPrefix = "unix:"sv;

/* The maximum size of a unix socket path is defined per-platform based on sockaddr_un.sun_path.
 * On Windows the fallback is the length of an ipv6 address. Subtracting one at the end is for
 * double counting null terminators from sun_path and TrUnixSocketPrefix. */
#ifdef _WIN32
auto inline constexpr TrUnixAddrStrLen = size_t{ INET6_ADDRSTRLEN };
#else
auto inline constexpr TrUnixAddrStrLen = size_t{ sizeof(std::declval<struct sockaddr_un>().sun_path) +
                                                 std::size(TrUnixSocketPrefix) };
#endif

enum tr_rpc_address_type : uint8_t
{
    TR_RPC_INET_ADDR,
    TR_RPC_UNIX_ADDR
};

class tr_unix_addr
{
public:
    [[nodiscard]] std::string to_string() const
    {
        return std::empty(unix_socket_path_) ? std::string(TrUnixSocketPrefix) : unix_socket_path_;
    }

    [[nodiscard]] bool from_string(std::string_view src)
    {
        if (!tr_strv_starts_with(src, TrUnixSocketPrefix))
        {
            return false;
        }

        if (std::size(src) >= TrUnixAddrStrLen)
        {
            tr_logAddError(fmt::format(
                _("Unix socket path must be fewer than {count} characters (including '{prefix}' prefix)"),
                fmt::arg("count", TrUnixAddrStrLen - 1),
                fmt::arg("prefix", TrUnixSocketPrefix)));
            return false;
        }
        unix_socket_path_ = src;
        return true;
    }

private:
    std::string unix_socket_path_;
};
} // namespace

class tr_rpc_address
{
public:
    tr_rpc_address()
        : inet_addr_{ tr_address::any(TR_AF_INET) }
    {
    }

    [[nodiscard]] constexpr auto is_unix_addr() const noexcept
    {
        return type_ == TR_RPC_UNIX_ADDR;
    }

    [[nodiscard]] constexpr auto is_inet_addr() const noexcept
    {
        return type_ == TR_RPC_INET_ADDR;
    }

    bool from_string(std::string_view src)
    {
        if (auto address = tr_address::from_string(src); address.has_value())
        {
            type_ = TR_RPC_INET_ADDR;
            inet_addr_ = address.value();
            return true;
        }

        if (unix_addr_.from_string(src))
        {
            type_ = TR_RPC_UNIX_ADDR;
            return true;
        }

        return false;
    }

    [[nodiscard]] std::string to_string(tr_port port = {}) const
    {
        if (type_ == TR_RPC_UNIX_ADDR)
        {
            return unix_addr_.to_string();
        }

        if (std::empty(port))
        {
            return inet_addr_.display_name();
        }
        return tr_socket_address::display_name(inet_addr_, port);
    }

private:
    tr_rpc_address_type type_ = TR_RPC_INET_ADDR;
    struct tr_address inet_addr_;
    class tr_unix_addr unix_addr_;
};

namespace
{
int constexpr DeflateLevel = 6; // medium / default

// ---

void send_simple_response(struct evhttp_request* req, int code, char const* text = nullptr)
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

// ---

[[nodiscard]] constexpr char const* mimetype_guess(std::string_view path)
{
    // these are the ones we need for serving the web client's files...
    auto constexpr Types = std::array<std::pair<std::string_view, char const*>, 7>{ {
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
        if (tr_strv_ends_with(path, suffix))
        {
            return mime_type;
        }
    }

    return "application/octet-stream";
}

[[nodiscard]] evbuffer* make_response(struct evhttp_request* req, tr_rpc_server const* server, std::string_view content)
{
    auto* const out = evbuffer_new();

    char const* key = "Accept-Encoding";
    char const* encoding = evhttp_find_header(req->input_headers, key);

    if (bool const do_compress = encoding != nullptr && tr_strv_contains(encoding, "gzip"sv); !do_compress)
    {
        evbuffer_add(out, std::data(content), std::size(content));
    }
    else
    {
        auto const max_compressed_len = libdeflate_deflate_compress_bound(server->compressor.get(), std::size(content));

        auto iov = evbuffer_iovec{};
        evbuffer_reserve_space(out, std::max(std::size(content), max_compressed_len), &iov, 1);

        auto const compressed_len = libdeflate_gzip_compress(
            server->compressor.get(),
            std::data(content),
            std::size(content),
            iov.iov_base,
            iov.iov_len);
        if (0 < compressed_len && compressed_len < std::size(content))
        {
            iov.iov_len = compressed_len;
            evhttp_add_header(req->output_headers, "Content-Encoding", "gzip");
        }
        else
        {
            std::copy(std::begin(content), std::end(content), static_cast<char*>(iov.iov_base));
            iov.iov_len = std::size(content);
        }

        evbuffer_commit_space(out, &iov, 1);
    }

    return out;
}

void add_time_header(struct evkeyvalq* headers, char const* key, time_t now)
{
    // RFC 2616 says this must follow RFC 1123's date format, so use gmtime instead of localtime
    evhttp_add_header(headers, key, fmt::format("{:%a %b %d %T %Y%n}", fmt::gmtime(now)).c_str());
}

void serve_file(struct evhttp_request* req, tr_rpc_server const* server, std::string_view filename)
{
    if (req->type != EVHTTP_REQ_GET)
    {
        evhttp_add_header(req->output_headers, "Allow", "GET");
        send_simple_response(req, HTTP_BADMETHOD);
        return;
    }

    auto content = std::vector<char>{};

    if (auto error = tr_error{}; !tr_file_read(filename, content, &error))
    {
        send_simple_response(req, HTTP_NOTFOUND, fmt::format("{} ({})", filename, error.message()).c_str());
        return;
    }

    auto const now = tr_time();
    add_time_header(req->output_headers, "Date", now);
    add_time_header(req->output_headers, "Expires", now + (24 * 60 * 60));
    evhttp_add_header(req->output_headers, "Content-Type", mimetype_guess(filename));

    auto* const response = make_response(req, server, std::string_view{ std::data(content), std::size(content) });
    evhttp_send_reply(req, HTTP_OK, "OK", response);
    evbuffer_free(response);
}

void handle_web_client(struct evhttp_request* req, tr_rpc_server const* server)
{
    if (std::empty(server->web_client_dir_))
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
        // convert `req->uri` (ex: "/transmission/web/images/favicon.png")
        // into a filesystem path (ex: "/usr/share/transmission/web/images/favicon.png")

        // remove the "/transmission/web/" prefix
        static auto constexpr Web = "web/"sv;
        auto subpath = std::string_view{ req->uri }.substr(std::size(server->url()) + std::size(Web));

        // remove any trailing query / fragment
        subpath = subpath.substr(0, subpath.find_first_of("?#"sv));

        // if the query is empty, use the default
        static auto constexpr DefaultPage = "index.html"sv;
        if (std::empty(subpath))
        {
            subpath = DefaultPage;
        }

        if (tr_strv_contains(subpath, ".."sv))
        {
            send_simple_response(req, HTTP_NOTFOUND);
        }
        else
        {
            serve_file(req, server, tr_pathbuf{ server->web_client_dir_, '/', subpath });
        }
    }
}

void handle_rpc_from_json(struct evhttp_request* req, tr_rpc_server* server, std::string_view json)
{
    if (auto otop = tr_variant_serde::json().inplace().parse(json); otop)
    {
        tr_rpc_request_exec(
            server->session,
            *otop,
            [req, server](tr_session* /*session*/, tr_variant&& content)
            {
                auto* const response = make_response(req, server, tr_variant_serde::json().compact().to_string(content));
                evhttp_add_header(req->output_headers, "Content-Type", "application/json; charset=UTF-8");
                evhttp_send_reply(req, HTTP_OK, "OK", response);
                evbuffer_free(response);
            });
    }
}

void handle_rpc(struct evhttp_request* req, tr_rpc_server* server)
{
    if (req->type == EVHTTP_REQ_POST)
    {
        auto json = std::string_view{ reinterpret_cast<char const*>(evbuffer_pullup(req->input_buffer, -1)),
                                      evbuffer_get_length(req->input_buffer) };
        handle_rpc_from_json(req, server, json);
        return;
    }

    send_simple_response(req, HTTP_BADMETHOD);
}

bool is_address_allowed(tr_rpc_server const* server, char const* address)
{
    if (!server->is_whitelist_enabled())
    {
        return true;
    }

    auto const& src = server->whitelist_;
    return std::any_of(std::begin(src), std::end(src), [&address](auto const& s) { return tr_wildmat(address, s); });
}

bool isIPAddressWithOptionalPort(char const* host)
{
    auto address = sockaddr_storage{};
    int address_len = sizeof(address);

    /* TODO: move to net.{c,h} */
    return evutil_parse_sockaddr_port(host, reinterpret_cast<sockaddr*>(&address), &address_len) != -1;
}

bool isHostnameAllowed(tr_rpc_server const* server, evhttp_request const* req)
{
    /* If password auth is enabled, any hostname is permitted. */
    if (server->is_password_enabled())
    {
        return true;
    }

    /* If whitelist is disabled, no restrictions. */
    if (!server->settings_.is_host_whitelist_enabled)
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

    auto const& src = server->host_whitelist_;
    return std::any_of(std::begin(src), std::end(src), [&hostname](auto const& str) { return tr_wildmat(hostname, str); });
}

bool test_session_id(tr_rpc_server const* server, evhttp_request const* req)
{
    char const* const session_id = evhttp_find_header(req->input_headers, TR_RPC_SESSION_ID_HEADER);
    return session_id != nullptr && server->session->sessionId() == session_id;
}

bool is_authorized(tr_rpc_server const* server, char const* auth_header)
{
    if (!server->is_password_enabled())
    {
        return true;
    }

    // https://datatracker.ietf.org/doc/html/rfc7617
    // `Basic ${base64(username)}:${base64(password)}`

    auto constexpr Prefix = "Basic "sv;
    auto auth = std::string_view{ auth_header != nullptr ? auth_header : "" };
    if (!tr_strv_starts_with(auth, Prefix))
    {
        return false;
    }

    auth.remove_prefix(std::size(Prefix));
    auto const decoded_str = tr_base64_decode(auth);
    auto decoded = std::string_view{ decoded_str };
    auto const username = tr_strv_sep(&decoded, ':');
    auto const password = decoded;
    return server->username() == username && tr_ssha1_matches(server->settings().salted_password, password);
}

void handle_request(struct evhttp_request* req, void* arg)
{
    auto constexpr HttpErrorUnauthorized = 401;
    auto constexpr HttpErrorForbidden = 403;

    auto* server = static_cast<tr_rpc_server*>(arg);

    if (req != nullptr && req->evcon != nullptr)
    {
        evhttp_add_header(req->output_headers, "Server", MY_REALM);

        if (server->is_anti_brute_force_enabled() && server->login_attempts_ >= server->settings().anti_brute_force_limit)
        {
            send_simple_response(req, HttpErrorForbidden);
            return;
        }

        if (!is_address_allowed(server, req->remote_host))
        {
            send_simple_response(req, HttpErrorForbidden);
            return;
        }

        evhttp_add_header(req->output_headers, "Access-Control-Allow-Origin", "*");

        if (req->type == EVHTTP_REQ_OPTIONS)
        {
            if (char const* headers = evhttp_find_header(req->input_headers, "Access-Control-Request-Headers");
                headers != nullptr)
            {
                evhttp_add_header(req->output_headers, "Access-Control-Allow-Headers", headers);
            }

            evhttp_add_header(req->output_headers, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            send_simple_response(req, HTTP_OK);
            return;
        }

        if (!is_authorized(server, evhttp_find_header(req->input_headers, "Authorization")))
        {
            evhttp_add_header(req->output_headers, "WWW-Authenticate", "Basic realm=\"" MY_REALM "\"");
            if (server->is_anti_brute_force_enabled())
            {
                ++server->login_attempts_;
            }

            send_simple_response(req, HttpErrorUnauthorized);
            return;
        }

        server->login_attempts_ = 0;

        auto uri = std::string_view{ req->uri };
        auto const location = tr_strv_starts_with(uri, server->url()) ? uri.substr(std::size(server->url())) : ""sv;

        if (std::empty(location) || location == "web"sv)
        {
            auto const new_location = fmt::format("{:s}web/", server->url());
            evhttp_add_header(req->output_headers, "Location", new_location.c_str());
            send_simple_response(req, HTTP_MOVEPERM, nullptr);
        }
        else if (tr_strv_starts_with(location, "web/"sv))
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
            auto const session_id = std::string{ server->session->sessionId() };
            auto const tmp = fmt::format(
                "<p>Your request had an invalid session_id header.</p>"
                "<p>To fix this, follow these steps:"
                "<ol><li> When reading a response, get its " TR_RPC_SESSION_ID_HEADER
                " header and remember it"
                "<li> Add the updated header to your outgoing requests"
                "<li> When you get this 409 error message, resend your request with the updated header"
                "</ol></p>"
                "<p>This requirement has been added to help prevent "
                "<a href=\"https://en.wikipedia.org/wiki/Cross-site_request_forgery\">CSRF</a> "
                "attacks.</p>"
                "<p><code>{:s}: {:s}</code></p>",
                TR_RPC_SESSION_ID_HEADER,
                session_id);
            evhttp_add_header(req->output_headers, TR_RPC_SESSION_ID_HEADER, session_id.c_str());
            evhttp_add_header(req->output_headers, "Access-Control-Expose-Headers", TR_RPC_SESSION_ID_HEADER);
            send_simple_response(req, 409, tmp.c_str());
        }
#endif
        else if (tr_strv_starts_with(location, "rpc"sv))
        {
            handle_rpc(req, server);
        }
        else
        {
            send_simple_response(req, HTTP_NOTFOUND, req->uri);
        }
    }
}

auto constexpr ServerStartRetryCount = 10;
auto constexpr ServerStartRetryDelayIncrement = 5s;
auto constexpr ServerStartRetryMaxDelay = 60s;

bool bindUnixSocket(
    [[maybe_unused]] struct event_base* base,
    [[maybe_unused]] struct evhttp* httpd,
    [[maybe_unused]] char const* path,
    [[maybe_unused]] tr_mode_t socket_mode)
{
#ifdef _WIN32
    tr_logAddError(fmt::format(
        _("Unix sockets are unsupported on Windows. Please change '{key}' in your settings."),
        fmt::arg("key", tr_quark_get_string_view(TR_KEY_rpc_bind_address))));
    return false;
#else
    auto addr = sockaddr_un{};
    addr.sun_family = AF_UNIX;
    *fmt::format_to_n(addr.sun_path, sizeof(addr.sun_path) - 1, "{:s}", path + std::size(TrUnixSocketPrefix)).out = '\0';

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

    if (chmod(addr.sun_path, socket_mode) != 0)
    {
        tr_logAddWarn(
            fmt::format(_("Couldn't set RPC socket mode to {mode:#o}, defaulting to 0755"), fmt::arg("mode", socket_mode)));
    }

    return evhttp_bind_listener(httpd, lev) != nullptr;
#endif
}

void start_server(tr_rpc_server* server);

auto rpc_server_start_retry(tr_rpc_server* server)
{
    if (!server->start_retry_timer)
    {
        server->start_retry_timer = server->session->timerMaker().create([server]() { start_server(server); });
    }

    ++server->start_retry_counter;
    auto const interval = std::min(ServerStartRetryDelayIncrement * server->start_retry_counter, ServerStartRetryMaxDelay);
    server->start_retry_timer->start_single_shot(std::chrono::duration_cast<std::chrono::milliseconds>(interval));
    return interval;
}

void rpc_server_start_retry_cancel(tr_rpc_server* server)
{
    server->start_retry_timer.reset();
    server->start_retry_counter = 0;
}

void start_server(tr_rpc_server* server)
{
    if (server->httpd)
    {
        return;
    }

    auto* const base = server->session->event_base();
    auto* const httpd = evhttp_new(base);

    evhttp_set_allowed_methods(httpd, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_OPTIONS);

    auto const address = server->get_bind_address();
    auto const port = server->port();

    bool const success = server->bind_address_->is_unix_addr() ?
        bindUnixSocket(base, httpd, address.c_str(), server->settings().socket_mode) :
        (evhttp_bind_socket(httpd, address.c_str(), port.host()) != -1);

    auto const addr_port_str = server->bind_address_->to_string(port);

    if (!success)
    {
        evhttp_free(httpd);

        if (server->start_retry_counter < ServerStartRetryCount)
        {
            auto const retry_delay = rpc_server_start_retry(server);
            auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(retry_delay).count();
            tr_logAddDebug(fmt::format("Couldn't bind to {}, retrying in {} seconds", addr_port_str, seconds));
            return;
        }

        tr_logAddError(fmt::format(
            tr_ngettext(
                "Couldn't bind to {address} after {count} attempt, giving up",
                "Couldn't bind to {address} after {count} attempts, giving up",
                ServerStartRetryCount),
            fmt::arg("address", addr_port_str),
            fmt::arg("count", ServerStartRetryCount)));
    }
    else
    {
        evhttp_set_gencb(httpd, handle_request, server);
        server->httpd.reset(httpd);

        tr_logAddInfo(fmt::format(_("Listening for RPC and Web requests on '{address}'"), fmt::arg("address", addr_port_str)));
    }

    rpc_server_start_retry_cancel(server);
}

void stop_server(tr_rpc_server* server)
{
    auto const lock = server->session->unique_lock();

    rpc_server_start_retry_cancel(server);

    auto& httpd = server->httpd;
    if (!httpd)
    {
        return;
    }

    auto const address = server->get_bind_address();

    httpd.reset();

    if (server->bind_address_->is_unix_addr())
    {
        unlink(address.c_str() + std::size(TrUnixSocketPrefix));
    }

    tr_logAddInfo(fmt::format(
        _("Stopped listening for RPC and Web requests on '{address}'"),
        fmt::arg("address", server->bind_address_->to_string(server->port()))));
}

void restart_server(tr_rpc_server* const server)
{
    if (server->is_enabled())
    {
        stop_server(server);
        start_server(server);
    }
}

auto parse_whitelist(std::string_view whitelist)
{
    auto list = std::vector<std::string>{};

    while (!std::empty(whitelist))
    {
        auto const pos = whitelist.find_first_of(" ,;"sv);
        auto const token = tr_strv_strip(whitelist.substr(0, pos));
        list.emplace_back(token);
        tr_logAddInfo(fmt::format(_("Added '{entry}' to host whitelist"), fmt::arg("entry", token)));
        whitelist = pos == std::string_view::npos ? ""sv : whitelist.substr(pos + 1);
    }

    return list;
}

} // namespace

void tr_rpc_server::set_enabled(bool is_enabled)
{
    settings_.is_enabled = is_enabled;

    session->run_in_session_thread(
        [this]()
        {
            if (!settings_.is_enabled)
            {
                stop_server(this);
            }
            else
            {
                start_server(this);
            }
        });
}

void tr_rpc_server::set_port(tr_port port) noexcept
{
    if (settings_.port == port)
    {
        return;
    }

    settings_.port = port;

    if (is_enabled())
    {
        session->run_in_session_thread(&restart_server, this);
    }
}

void tr_rpc_server::set_url(std::string_view url)
{
    settings_.url = url;
    tr_logAddDebug(fmt::format("setting our URL to '{:s}'", url));
}

void tr_rpc_server::set_whitelist(std::string_view whitelist)
{
    settings_.whitelist_str = whitelist;
    whitelist_ = parse_whitelist(whitelist);
}

// --- PASSWORD

void tr_rpc_server::set_username(std::string_view username)
{
    settings_.username = username;
    tr_logAddDebug(fmt::format("setting our username to '{:s}'", username));
}

void tr_rpc_server::set_password(std::string_view password) noexcept
{
    auto const is_salted = tr_ssha1_test(password);
    settings_.salted_password = is_salted ? password : tr_ssha1(password);
    tr_logAddDebug(fmt::format("setting our salted password to '{:s}'", settings_.salted_password));
}

void tr_rpc_server::set_password_enabled(bool enabled)
{
    settings_.authentication_required = enabled;
    tr_logAddDebug(fmt::format("setting password-enabled to '{}'", enabled));
}

std::string tr_rpc_server::get_bind_address() const
{
    return bind_address_->to_string();
}

void tr_rpc_server::set_anti_brute_force_enabled(bool enabled) noexcept
{
    settings_.is_anti_brute_force_enabled = enabled;

    if (!enabled)
    {
        login_attempts_ = 0;
    }
}

// --- LIFECYCLE

tr_rpc_server::tr_rpc_server(tr_session* session_in, Settings&& settings)
    : compressor{ libdeflate_alloc_compressor(DeflateLevel), libdeflate_free_compressor }
    , web_client_dir_{ tr_getWebClientDir(session_in) }
    , bind_address_{ std::make_unique<class tr_rpc_address>() }
    , session{ session_in }
{
    load(std::move(settings));
}

void tr_rpc_server::load(Settings&& settings)
{
    settings_ = std::move(settings);

    if (!tr_strv_ends_with(settings_.url, '/'))
    {
        settings_.url = fmt::format("{:s}/", settings_.url);
    }

    host_whitelist_ = parse_whitelist(settings_.host_whitelist_str);
    set_password_enabled(settings_.authentication_required);
    set_whitelist(settings_.whitelist_str);
    set_username(settings_.username);
    set_password(settings_.salted_password);

    if (!bind_address_->from_string(settings_.bind_address_str))
    {
        // NOTE: bind_address_ is default initialized to INADDR_ANY
        tr_logAddWarn(fmt::format(
            _("The '{key}' setting is '{value}' but must be an IPv4 or IPv6 address or a Unix socket path. Using default value '0.0.0.0'"),
            fmt::arg("key", tr_quark_get_string_view(TR_KEY_rpc_bind_address)),
            fmt::arg("value", settings_.bind_address_str)));
    }

    if (bind_address_->is_unix_addr())
    {
        set_whitelist_enabled(false);
        settings_.is_host_whitelist_enabled = false;
    }
    if (this->is_enabled())
    {
        auto const rpc_uri = bind_address_->to_string(port()) + settings_.url;
        tr_logAddInfo(fmt::format(_("Serving RPC and Web requests on {address}"), fmt::arg("address", rpc_uri)));
        session->run_in_session_thread(start_server, this);

        if (this->is_whitelist_enabled())
        {
            tr_logAddInfo(_("Whitelist enabled"));
        }

        if (this->is_password_enabled())
        {
            tr_logAddInfo(_("Password required"));
        }
    }

    if (!std::empty(web_client_dir_))
    {
        tr_logAddInfo(fmt::format(_("Serving RPC and Web requests from '{path}'"), fmt::arg("path", web_client_dir_)));
    }
}

tr_rpc_server::~tr_rpc_server()
{
    stop_server(this);
}
