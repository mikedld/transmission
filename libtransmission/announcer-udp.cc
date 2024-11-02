// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // for std::find_if()
#include <array>
#include <chrono> // operator""ms, literals
#include <cstddef> // std::byte
#include <cstdint> // uint32_t, uint64_t
#include <cstring> // memcpy()
#include <ctime>
#include <future>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include <ws2tcpip.h>
#undef gai_strerror
#define gai_strerror gai_strerrorA
#else
#include <netdb.h> // gai_strerror()
#include <netinet/in.h> // IPPROTO_UDP, in_addr
#include <sys/socket.h> // sockaddr_storage, AF_INET
#endif

#include <fmt/core.h>

#define LIBTRANSMISSION_ANNOUNCER_MODULE

#include "libtransmission/announcer.h"
#include "libtransmission/announcer-common.h"
#include "libtransmission/crypto-utils.h" // for tr_rand_obj()
#include "libtransmission/interned-string.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/peer-mgr.h" // for tr_pex::fromCompact4()
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-buffer.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"
#include "libtransmission/web-utils.h"

#define logwarn(name, msg) tr_logAddWarn(msg, name)
#define logdbg(name, msg) tr_logAddDebug(msg, name)
#define logtrace(name, msg) tr_logAddTrace(msg, name)

namespace
{
using namespace std::literals;

// size defined by https://www.bittorrent.org/beps/bep_0015.html
using tau_connection_t = uint64_t;
using tau_transaction_t = uint32_t;

using InBuf = libtransmission::BufferReader<std::byte>;
using PayloadBuffer = libtransmission::StackBuffer<4096, std::byte>;

using ipp_t = std::underlying_type_t<tr_address_type>;

constexpr auto TauConnectionTtlSecs = time_t{ 45 };

auto tau_transaction_new()
{
    return tr_rand_obj<tau_transaction_t>();
}

// used in the "action" field of a request.
// Values defined in https://www.bittorrent.org/beps/bep_0015.html
enum tau_action_t : uint8_t
{
    TAU_ACTION_CONNECT = 0,
    TAU_ACTION_ANNOUNCE = 1,
    TAU_ACTION_SCRAPE = 2,
    TAU_ACTION_ERROR = 3
};

// --- SCRAPE

struct tau_scrape_request
{
    tau_scrape_request(tr_scrape_request const& in, tr_scrape_response_func on_response)
        : on_response_{ std::move(on_response) }
    {
        response.scrape_url = in.scrape_url;
        response.row_count = in.info_hash_count;
        for (int i = 0; i < response.row_count; ++i)
        {
            response.rows[i].info_hash = in.info_hash[i];
        }

        // build the payload
        payload.add_uint32(TAU_ACTION_SCRAPE);
        payload.add_uint32(transaction_id);
        for (int i = 0; i < in.info_hash_count; ++i)
        {
            payload.add(in.info_hash[i]);
        }
    }

    [[nodiscard]] auto has_callback() const noexcept
    {
        return !!on_response_;
    }

    void request_finished() const
    {
        if (on_response_)
        {
            on_response_(response);
        }
    }

    void fail(bool did_connect, bool did_timeout, std::string_view errmsg)
    {
        response.did_connect = did_connect;
        response.did_timeout = did_timeout;
        response.errmsg = errmsg;
        request_finished();
    }

    void on_response(tau_action_t action, InBuf& buf)
    {
        response.did_connect = true;
        response.did_timeout = false;

        if (action == TAU_ACTION_SCRAPE)
        {
            for (int i = 0; i < response.row_count && std::size(buf) >= sizeof(uint32_t) * 3U; ++i)
            {
                auto& row = response.rows[i];
                row.seeders = buf.to_uint32();
                row.downloads = buf.to_uint32();
                row.leechers = buf.to_uint32();
            }

            request_finished();
        }
        else
        {
            std::string const errmsg = action == TAU_ACTION_ERROR && !std::empty(buf) ? buf.to_string() : _("Unknown error");
            fail(true, false, errmsg);
        }
    }

    [[nodiscard]] constexpr auto expires_at() const noexcept
    {
        return created_at_ + TrScrapeTimeoutSec.count();
    }

    PayloadBuffer payload;

    time_t sent_at = 0;
    tau_transaction_t const transaction_id = tau_transaction_new();

    tr_scrape_response response = {};

    static auto constexpr ip_protocol = TR_AF_UNSPEC; // NOLINT(readability-identifier-naming)

private:
    time_t const created_at_ = tr_time();

    tr_scrape_response_func on_response_;
};

// --- ANNOUNCE

struct tau_announce_request
{
    tau_announce_request(
        tr_address_type ip_protocol_in,
        std::optional<tr_address> announce_ip,
        tr_announce_request const& in,
        tr_announce_response_func on_response)
        : ip_protocol{ ip_protocol_in }
        , on_response_{ std::move(on_response) }
    {
        // https://www.bittorrent.org/beps/bep_0015.html sets key size at 32 bits
        static_assert(sizeof(tr_announce_request::key) == sizeof(uint32_t));

        response.info_hash = in.info_hash;

        // build the payload
        payload.add_uint32(TAU_ACTION_ANNOUNCE);
        payload.add_uint32(transaction_id);
        payload.add(in.info_hash);
        payload.add(in.peer_id);
        payload.add_uint64(in.down);
        payload.add_uint64(in.leftUntilComplete);
        payload.add_uint64(in.up);
        payload.add_uint32(get_tau_announce_event(in.event));
        if (announce_ip && announce_ip->is_ipv4())
        {
            // Since size of IP field is only 4 bytes long, we can only announce IPv4 addresses
            payload.add_address(*announce_ip);
        }
        else
        {
            payload.add_uint32(0U);
        }
        payload.add_uint32(in.key);
        payload.add_uint32(in.numwant);
        payload.add_port(in.port);
    }

    [[nodiscard]] auto has_callback() const noexcept
    {
        return !!on_response_;
    }

    void request_finished() const
    {
        if (on_response_)
        {
            on_response_(response);
        }
    }

    void fail(bool did_connect, bool did_timeout, std::string_view errmsg)
    {
        response.did_connect = did_connect;
        response.did_timeout = did_timeout;
        response.errmsg = errmsg;
        request_finished();
    }

    void on_response(tr_address_type ip_protocol_resp, tau_action_t action, InBuf& buf)
    {
        auto const buflen = std::size(buf);

        response.did_connect = true;
        response.did_timeout = false;

        if (action == TAU_ACTION_ANNOUNCE && buflen >= 3 * sizeof(uint32_t))
        {
            response.interval = buf.to_uint32();
            response.leechers = buf.to_uint32();
            response.seeders = buf.to_uint32();

            switch (ip_protocol_resp)
            {
            case TR_AF_INET:
                response.pex = tr_pex::from_compact_ipv4(std::data(buf), std::size(buf), nullptr, 0);
                break;
            case TR_AF_INET6:
                response.pex6 = tr_pex::from_compact_ipv6(std::data(buf), std::size(buf), nullptr, 0);
                break;
            default:
                break;
            }
            request_finished();
        }
        else
        {
            std::string const errmsg = action == TAU_ACTION_ERROR && !std::empty(buf) ? buf.to_string() : _("Unknown error");
            fail(true, false, errmsg);
        }
    }

    [[nodiscard]] constexpr auto expires_at() const noexcept
    {
        return created_at_ + TrAnnounceTimeoutSec.count();
    }

    enum tau_announce_event : uint8_t
    {
        // https://www.bittorrent.org/beps/bep_0015.html
        // Used in the "event" field of an announce request.
        TAU_ANNOUNCE_EVENT_NONE = 0,
        TAU_ANNOUNCE_EVENT_COMPLETED = 1,
        TAU_ANNOUNCE_EVENT_STARTED = 2,
        TAU_ANNOUNCE_EVENT_STOPPED = 3
    };

    PayloadBuffer payload;

    tr_address_type const ip_protocol;
    time_t sent_at = 0;
    tau_transaction_t const transaction_id = tau_transaction_new();

    tr_announce_response response = {};

private:
    [[nodiscard]] static constexpr tau_announce_event get_tau_announce_event(tr_announce_event e)
    {
        switch (e)
        {
        case TR_ANNOUNCE_EVENT_COMPLETED:
            return TAU_ANNOUNCE_EVENT_COMPLETED;

        case TR_ANNOUNCE_EVENT_STARTED:
            return TAU_ANNOUNCE_EVENT_STARTED;

        case TR_ANNOUNCE_EVENT_STOPPED:
            return TAU_ANNOUNCE_EVENT_STOPPED;

        default:
            return TAU_ANNOUNCE_EVENT_NONE;
        }
    }

    time_t const created_at_ = tr_time();

    tr_announce_response_func on_response_;
};

// --- TRACKER

struct tau_tracker
{
    using Mediator = tr_announcer_udp::Mediator;

    tau_tracker(
        Mediator& mediator,
        std::string_view const authority_in,
        std::string_view const host_in,
        std::string_view const host_lookup_in,
        tr_port const port_in)
        : authority{ authority_in }
        , host{ host_in }
        , host_lookup{ host_lookup_in }
        , port{ port_in }
        , mediator_{ mediator }
    {
    }

    void sendto(tr_address_type ip_protocol, std::byte const* buf, size_t buflen)
    {
        TR_ASSERT(tr_address::is_valid(ip_protocol));
        if (!tr_address::is_valid(ip_protocol))
        {
            return;
        }

        auto const& addr = addr_[ip_protocol];
        TR_ASSERT(addr);
        if (!addr)
        {
            return;
        }

        auto const& [ss, sslen] = *addr;
        mediator_.sendto(buf, buflen, reinterpret_cast<sockaddr const*>(&ss), sslen);
    }

    void on_connection_response(tr_address_type ip_protocol, tau_action_t action, InBuf& buf)
    {
        TR_ASSERT(tr_address::is_valid(ip_protocol));
        if (!tr_address::is_valid(ip_protocol))
        {
            return;
        }

        connecting_at[ip_protocol] = 0;
        connection_transaction_id[ip_protocol] = 0;

        if (action == TAU_ACTION_CONNECT)
        {
            auto& conn_id = connection_id[ip_protocol];
            conn_id = buf.to_uint64();
            connection_expiration_time[ip_protocol] = tr_time() + TauConnectionTtlSecs;
            logdbg(
                log_name(),
                fmt::format("Got a new {} connection ID from tracker: {}", tr_ip_protocol_to_sv(ip_protocol), conn_id));
        }
        else if (action == TAU_ACTION_ERROR)
        {
            std::string errmsg = !std::empty(buf) ?
                buf.to_string() :
                fmt::format(_("{ip_protocol} connection failed"), fmt::arg("ip_protocol", tr_ip_protocol_to_sv(ip_protocol)));
            fail_all(true, false, errmsg);
            logdbg(log_name(), std::move(errmsg));
        }

        upkeep();
    }

    void upkeep(bool timeout_reqs = true)
    {
        time_t const now = tr_time();

        for (ipp_t ipp = 0; ipp < NUM_TR_AF_INET_TYPES; ++ipp)
        {
            // do we have a DNS request that's ready?
            if (auto& dns = addr_pending_dns_[ipp]; dns && dns->wait_for(0ms) == std::future_status::ready)
            {
                addr_[ipp] = dns->get();
                dns.reset();
                addr_expires_at_[ipp] = now + DnsRetryIntervalSecs;
            }
        }

        // are there any tracker requests pending?
        if (is_idle())
        {
            return;
        }

        for (ipp_t ipp = 0; ipp < NUM_TR_AF_INET_TYPES; ++ipp)
        {
            // update the addr if our lookup is past its shelf date
            if (auto& dns = addr_pending_dns_[ipp]; !dns && addr_expires_at_[ipp] <= now)
            {
                addr_[ipp].reset();
                dns = std::async(
                    std::launch::async,
                    [this](tr_address_type ip_protocol) { return lookup(ip_protocol); },
                    static_cast<tr_address_type>(ipp));
            }
        }

        // are there any dns requests pending?
        if (is_dns_pending())
        {
            return;
        }

        for (ipp_t ipp = 0; ipp < NUM_TR_AF_INET_TYPES; ++ipp)
        {
            auto const ipp_enum = static_cast<tr_address_type>(ipp);
            auto& conn_at = connecting_at[ipp];
            logtrace(
                log_name(),
                fmt::format(
                    "{} connected {} ({} {}) -- connecting_at {}",
                    tr_ip_protocol_to_sv(ipp_enum),
                    is_connected(ipp_enum, now),
                    connection_expiration_time[ipp],
                    now,
                    conn_at));

            // also need a valid connection ID...
            if (auto const& addr = addr_[ipp]; addr && !is_connected(ipp_enum, now) && conn_at == 0)
            {
                TR_ASSERT(addr->first.ss_family == tr_ip_protocol_to_af(ipp_enum));
                auto& conn_transc_id = connection_transaction_id[ipp];

                conn_at = now;
                conn_transc_id = tau_transaction_new();
                logtrace(
                    log_name(),
                    fmt::format("Trying to connect {}. Transaction ID is {}", tr_ip_protocol_to_sv(ipp_enum), conn_transc_id));

                auto buf = PayloadBuffer{};
                buf.add_uint64(0x41727101980LL);
                buf.add_uint32(TAU_ACTION_CONNECT);
                buf.add_uint32(conn_transc_id);

                sendto(ipp_enum, std::data(buf), std::size(buf));
            }
        }

        if (timeout_reqs)
        {
            timeout_requests(now);
        }

        maybe_send_requests(now);
    }

    [[nodiscard]] constexpr bool is_idle() const noexcept
    {
        return std::empty(announces) && std::empty(scrapes);
    }

private:
    using Sockaddr = std::pair<sockaddr_storage, socklen_t>;
    using MaybeSockaddr = std::optional<Sockaddr>;

    [[nodiscard]] constexpr bool is_connected(tr_address_type ip_protocol, time_t now) const noexcept
    {
        return connection_id[ip_protocol] != tau_connection_t{} && now < connection_expiration_time[ip_protocol];
    }

    [[nodiscard]] TR_CONSTEXPR20 bool is_dns_pending() const noexcept
    {
        return std::any_of(std::begin(addr_pending_dns_), std::end(addr_pending_dns_), [](auto const& o) { return !!o; });
    }

    [[nodiscard]] TR_CONSTEXPR20 bool has_addr() const noexcept
    {
        return std::any_of(std::begin(addr_), std::end(addr_), [](auto const& o) { return !!o; });
    }

    [[nodiscard]] MaybeSockaddr lookup(tr_address_type ip_protocol)
    {
        auto szport = std::array<char, 16>{};
        *fmt::format_to(std::data(szport), "{:d}", port.host()) = '\0';

        auto hints = addrinfo{};
        hints.ai_family = tr_ip_protocol_to_af(ip_protocol);
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* info = nullptr;
        auto const szhost = tr_urlbuf{ host_lookup };
        if (int const rc = getaddrinfo(szhost.c_str(), std::data(szport), &hints, &info); rc != 0)
        {
            logwarn(
                log_name(),
                fmt::format(
                    _("Couldn't look up '{address}:{port}' in {ip_protocol}: {error} ({error_code})"),
                    fmt::arg("address", host),
                    fmt::arg("port", port.host()),
                    fmt::arg("ip_protocol", tr_ip_protocol_to_sv(ip_protocol)),
                    fmt::arg("error", gai_strerror(rc)),
                    fmt::arg("error_code", static_cast<int>(rc))));
            return {};
        }
        auto const info_uniq = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>{ info, freeaddrinfo };

        // N.B. getaddrinfo() will return IPv4-mapped addresses by default on macOS
        auto socket_address = tr_socket_address::from_sockaddr(info->ai_addr);
        if (!socket_address || socket_address->address().is_ipv4_mapped_address())
        {
            logdbg(
                log_name(),
                fmt::format(
                    "Couldn't look up '{address}:{port}' in {ip_protocol}: got invalid address",
                    fmt::arg("address", host),
                    fmt::arg("port", port.host()),
                    fmt::arg("ip_protocol", tr_ip_protocol_to_sv(ip_protocol))));
            return {};
        }

        logdbg(log_name(), fmt::format("{} DNS lookup succeeded", tr_ip_protocol_to_sv(ip_protocol)));
        return socket_address->to_sockaddr();
    }

    void fail_all(bool did_connect, bool did_timeout, std::string_view errmsg)
    {
        for (auto& req : scrapes)
        {
            req.fail(did_connect, did_timeout, errmsg);
        }

        for (auto& req : announces)
        {
            req.fail(did_connect, did_timeout, errmsg);
        }

        scrapes.clear();
        announces.clear();
    }

    // ---

    void timeout_requests(time_t now)
    {
        for (ipp_t ipp = 0; ipp < NUM_TR_AF_INET_TYPES; ++ipp)
        {
            if (auto const conn_at = connecting_at[ipp]; conn_at != 0 && conn_at + ConnectionRequestTtl < now)
            {
                auto empty_buf = PayloadBuffer{};
                on_connection_response(static_cast<tr_address_type>(ipp), TAU_ACTION_ERROR, empty_buf);
            }
        }

        timeout_requests(announces, now, "announce"sv);
        timeout_requests(scrapes, now, "scrape"sv);
    }

    template<typename T>
    void timeout_requests(std::list<T>& requests, time_t now, std::string_view name)
    {
        for (auto it = std::begin(requests); it != std::end(requests);)
        {
            if (auto& req = *it; req.expires_at() <= now)
            {
                logtrace(log_name(), fmt::format("timeout {} req {}", name, fmt::ptr(&req)));
                req.fail(false, true, "");
                it = requests.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // ---

    void maybe_send_requests(time_t now)
    {
        TR_ASSERT(!is_dns_pending());
        if (is_dns_pending() || !has_addr())
        {
            return;
        }

        maybe_send_requests(announces, now);
        maybe_send_requests(scrapes, now);
    }

    template<typename T>
    void maybe_send_requests(std::list<T>& reqs, time_t now)
    {
        for (auto it = std::begin(reqs); it != std::end(reqs);)
        {
            auto& req = *it;

            if (req.sent_at != 0 || // it's already been sent; we're awaiting a response
                !maybe_send_request(req.ip_protocol, std::data(req.payload), std::size(req.payload), now))
            {
                ++it;
                continue;
            }
            logdbg(log_name(), fmt::format("sent req {}", fmt::ptr(&req)));
            req.sent_at = now;

            if (req.has_callback())
            {
                ++it;
                continue;
            }

            // no response needed, so we can remove it now
            it = reqs.erase(it);
        }
    }

    bool maybe_send_request(tr_address_type ip_protocol, std::byte const* payload, size_t payload_len, time_t now)
    {
        for (uint8_t ipp = 0; ipp < NUM_TR_AF_INET_TYPES; ++ipp)
        {
            auto const ipp_enum = static_cast<tr_address_type>(ipp);
            if (addr_[ipp] && (ip_protocol == TR_AF_UNSPEC || ipp == ip_protocol) && is_connected(ipp_enum, now))
            {
                auto const conn_id = connection_id[ipp];
                logdbg(log_name(), fmt::format("sending request w/connection id {}", conn_id));

                auto buf = PayloadBuffer{};
                buf.add_uint64(conn_id);
                buf.add(payload, payload_len);

                sendto(ipp_enum, std::data(buf), std::size(buf));
                return true;
            }
        }
        return false;
    }

public:
    [[nodiscard]] constexpr std::string_view log_name() const noexcept
    {
        return authority;
    }

    std::string_view const authority;
    std::string_view const host;
    std::string_view const host_lookup;
    tr_port const port;

    std::array<time_t, NUM_TR_AF_INET_TYPES> connecting_at = {};
    std::array<time_t, NUM_TR_AF_INET_TYPES> connection_expiration_time = {};
    std::array<tau_connection_t, NUM_TR_AF_INET_TYPES> connection_id = {};
    std::array<tau_transaction_t, NUM_TR_AF_INET_TYPES> connection_transaction_id = {};

    std::list<tau_announce_request> announces;
    std::list<tau_scrape_request> scrapes;

private:
    Mediator& mediator_;

    std::array<std::optional<std::future<MaybeSockaddr>>, NUM_TR_AF_INET_TYPES> addr_pending_dns_;

    std::array<MaybeSockaddr, NUM_TR_AF_INET_TYPES> addr_ = {};
    std::array<time_t, NUM_TR_AF_INET_TYPES> addr_expires_at_ = {};

    static constexpr auto DnsRetryIntervalSecs = time_t{ 3600 };
    static constexpr auto ConnectionRequestTtl = time_t{ 30 };
};

// --- SESSION

class tr_announcer_udp_impl final : public tr_announcer_udp
{
public:
    explicit tr_announcer_udp_impl(Mediator& mediator)
        : mediator_{ mediator }
    {
    }

    void announce(tr_announce_request const& request, tr_announce_response_func on_response) override
    {
        auto* const tracker = get_tracker_from_url(request.announce_url);
        if (tracker == nullptr)
        {
            return;
        }

        for (ipp_t ipp = 0; ipp < NUM_TR_AF_INET_TYPES; ++ipp)
        {
            tracker->announces.emplace_back(static_cast<tr_address_type>(ipp), mediator_.announce_ip(), request, on_response);
        }
        tracker->upkeep(false);
    }

    void scrape(tr_scrape_request const& request, tr_scrape_response_func on_response) override
    {
        auto* const tracker = get_tracker_from_url(request.scrape_url);
        if (tracker == nullptr)
        {
            return;
        }

        tracker->scrapes.emplace_back(request, std::move(on_response));
        tracker->upkeep(false);
    }

    void upkeep() override
    {
        for (auto& tracker : trackers_)
        {
            tracker.upkeep();
        }
    }

    // @brief process an incoming udp message if it's a tracker response.
    // @return true if msg was a tracker response; false otherwise
    bool handle_message(uint8_t const* msg, size_t msglen, struct sockaddr const* from, socklen_t /*fromlen*/) override
    {
        if (msglen < sizeof(uint32_t) * 2)
        {
            return false;
        }

        // extract the action_id and see if it makes sense
        auto buf = PayloadBuffer{};
        buf.add(msg, msglen);
        auto const action_id = static_cast<tau_action_t>(buf.to_uint32());

        if (!is_response_message(action_id, msglen))
        {
            return false;
        }

        // extract the transaction_id and look for a match
        tau_transaction_t const transaction_id = buf.to_uint32();

        auto const socket_address = tr_socket_address::from_sockaddr(from);
        auto const ip_protocol = socket_address ? socket_address->address().type : NUM_TR_AF_INET_TYPES;
        for (auto& tracker : trackers_)
        {
            // is it a connection response?
            if (tr_address::is_valid(ip_protocol) && tracker.connecting_at[ip_protocol] != 0 &&
                transaction_id == tracker.connection_transaction_id[ip_protocol])
            {
                logtrace(tracker.log_name(), fmt::format("{} is my connection request!", transaction_id));
                tracker.on_connection_response(ip_protocol, action_id, buf);
                return true;
            }

            // is it a response to one of this tracker's announces?
            if (auto& reqs = tracker.announces; !std::empty(reqs))
            {
                if (auto it = std::find_if(
                        std::begin(reqs),
                        std::end(reqs),
                        [&transaction_id](auto const& req) { return req.transaction_id == transaction_id; });
                    it != std::end(reqs))
                {
                    logtrace(tracker.log_name(), fmt::format("{} is an announce request!", transaction_id));
                    it->on_response(ip_protocol, action_id, buf);
                    reqs.erase(it);
                    return true;
                }
            }

            // is it a response to one of this tracker's scrapes?
            if (auto& reqs = tracker.scrapes; !std::empty(reqs))
            {
                if (auto it = std::find_if(
                        std::begin(reqs),
                        std::end(reqs),
                        [&transaction_id](auto const& req) { return req.transaction_id == transaction_id; });
                    it != std::end(reqs))
                {
                    logtrace(tracker.log_name(), fmt::format("{} is a scrape request!", transaction_id));
                    it->on_response(action_id, buf);
                    reqs.erase(it);
                    return true;
                }
            }
        }

        // no match...
        return false;
    }

    [[nodiscard]] bool is_idle() const noexcept override
    {
        return std::all_of(std::begin(trackers_), std::end(trackers_), [](auto const& tracker) { return tracker.is_idle(); });
    }

private:
    // Finds the tau_tracker struct that corresponds to this url.
    // If it doesn't exist yet, create one.
    tau_tracker* get_tracker_from_url(tr_interned_string const announce_url)
    {
        // build a lookup key for this tracker
        auto const parsed = tr_urlParseTracker(announce_url);
        TR_ASSERT(parsed);
        if (!parsed)
        {
            return nullptr;
        }

        // see if we already have it
        auto const authority = parsed->authority;
        for (auto& tracker : trackers_)
        {
            if (tracker.authority == authority)
            {
                return &tracker;
            }
        }

        // we don't have it -- build a new one
        auto& tracker = trackers_.emplace_back(
            mediator_,
            authority,
            parsed->host,
            parsed->host_wo_brackets,
            tr_port::from_host(parsed->port));
        logtrace(tracker.log_name(), "New tau_tracker created");
        return &tracker;
    }

    [[nodiscard]] static constexpr bool is_response_message(tau_action_t action, size_t msglen) noexcept
    {
        if (action == TAU_ACTION_CONNECT)
        {
            return msglen == 16;
        }

        if (action == TAU_ACTION_ANNOUNCE)
        {
            return msglen >= 20;
        }

        if (action == TAU_ACTION_SCRAPE)
        {
            return msglen >= 20;
        }

        if (action == TAU_ACTION_ERROR)
        {
            return msglen >= 8;
        }

        return false;
    }

    std::list<tau_tracker> trackers_;

    Mediator& mediator_;
};

} // namespace

std::unique_ptr<tr_announcer_udp> tr_announcer_udp::create(Mediator& mediator)
{
    return std::make_unique<tr_announcer_udp_impl>(mediator);
}
