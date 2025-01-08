// Except where noted, This file Copyright © Johannes Lieder.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef> // std::byte
#include <cstdint> // uint16_t
#include <cstring>
#include <ctime> // time_t
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h> /* sockaddr_in */
#include <sys/socket.h> /* socket(), bind() */
#endif

#include <event2/event.h>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/crypto-utils.h" // for tr_rand_obj()
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/timer.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-lpd.h"
#include "libtransmission/utils.h" // for tr_net_init()
#include "libtransmission/utils-ev.h" // for tr_net_init()

using namespace std::literals;

// Code in this namespace Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only), MIT (SPDX: MIT),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.
namespace
{

using ipp_t = std::underlying_type_t<tr_address_type>;

// opaque value, allowing the sending client to filter out its
// own announces if it receives them via multicast loopback
auto makeCookie()
{
    static auto constexpr Pool = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"sv;

    auto buf = tr_rand_obj<std::array<char, 12>>();
    for (auto& ch : buf)
    {
        ch = Pool[static_cast<unsigned char>(ch) % std::size(Pool)];
    }

    return std::string{ std::data(buf), std::size(buf) };
}

auto constexpr McastSockAddr = std::array{ "239.192.152.143:6771"sv, "[ff15::efc0:988f]:6771"sv };
static_assert(std::size(McastSockAddr) == NUM_TR_AF_INET_TYPES);

/*
 * A LSD announce is formatted as follows:
 *
 * ```
 * BT-SEARCH * HTTP/1.1\r\n
 * Host: <host>\r\n
 * Port: <port>\r\n
 * Infohash: <ihash>\r\n
 * cookie: <cookie (optional)>\r\n
 * \r\n
 * \r\n
 * ```
 *
 * An announce may contain multiple, consecutive Infohash headers
 * to announce the participation in more than one torrent. This
 * may not be supported by older implementations. When sending
 * multiple infohashes the packet length should not exceed 1400
 * bytes to avoid MTU/fragmentation problems.
 */
std::string makeAnnounceMsg(
    tr_address_type ip_protocol,
    std::string_view cookie,
    tr_port port,
    std::vector<std::string_view> const& info_hash_strings)
{
    TR_ASSERT(tr_address::is_valid(ip_protocol));
    if (!tr_address::is_valid(ip_protocol))
    {
        return {};
    }

    auto ret = fmt::format(
        "BT-SEARCH * HTTP/1.1\r\n"
        "Host: {:s}\r\n"
        "Port: {:d}\r\n",
        McastSockAddr[ip_protocol],
        port.host());

    for (auto const& info_hash : info_hash_strings)
    {
        ret += fmt::format("Infohash: {:s}\r\n", tr_strupper(info_hash));
    }

    if (!std::empty(cookie))
    {
        ret += fmt::format("cookie: {:s}\r\n", cookie);
    }

    return ret + "\r\n\r\n";
}

struct ParsedAnnounce
{
    int major;
    int minor;
    tr_port port;
    std::vector<std::string_view> info_hash_strings;
    std::string_view cookie;
};

std::optional<ParsedAnnounce> parseAnnounceMsg(std::string_view announce)
{
    static auto constexpr CrLf = "\r\n"sv;

    auto ret = ParsedAnnounce{};

    // get major, minor
    auto key = "BT-SEARCH * HTTP/"sv;
    if (auto const pos = announce.find(key); pos != std::string_view::npos)
    {
        // parse `${major}.${minor}`
        auto walk = announce.substr(pos + std::size(key));
        if (auto const major = tr_num_parse<int>(walk, &walk); major && tr_strv_starts_with(walk, '.'))
        {
            ret.major = *major;
        }
        else
        {
            return {};
        }

        walk.remove_prefix(1); // the '.' between major and minor
        if (auto const minor = tr_num_parse<int>(walk, &walk); minor && tr_strv_starts_with(walk, CrLf))
        {
            ret.minor = *minor;
        }
        else
        {
            return {};
        }
    }

    key = "Port: "sv;
    if (auto const pos = announce.find(key); pos != std::string_view::npos)
    {
        auto walk = announce.substr(pos + std::size(key));
        if (auto const port = tr_num_parse<uint16_t>(walk, &walk); port && tr_strv_starts_with(walk, CrLf))
        {
            ret.port = tr_port::from_host(*port);
        }
        else
        {
            return {};
        }
    }

    key = "cookie: "sv;
    if (auto const pos = announce.find(key); pos != std::string_view::npos)
    {
        auto walk = announce.substr(pos + std::size(key));
        if (auto const end = walk.find(CrLf); end != std::string_view::npos)
        {
            ret.cookie = walk.substr(0, end);
        }
        else
        {
            return {};
        }
    }

    key = "Infohash: "sv;
    for (;;)
    {
        if (auto const pos = announce.find(key); pos != std::string_view::npos)
        {
            announce.remove_prefix(pos + std::size(key));
        }
        else
        {
            break;
        }

        if (auto const end = announce.find(CrLf); end != std::string_view::npos)
        {
            ret.info_hash_strings.push_back(announce.substr(0, end));
            announce.remove_prefix(end + std::size(CrLf));
        }
        else
        {
            return {};
        }
    }

    return ret;
}

} // namespace

class tr_lpd_impl final : public tr_lpd
{
public:
    tr_lpd_impl(Mediator& mediator, struct event_base* event_base)
        : mediator_{ mediator }
        , announce_timer_{ mediator.timerMaker().create([this]() { announceUpkeep(); }) }
        , dos_timer_{ mediator.timerMaker().create([this]() { dosUpkeep(); }) }
    {
        if (!init(event_base))
        {
            return;
        }

        announce_timer_->start_repeating(AnnounceInterval);
        announceUpkeep();
        dos_timer_->start_repeating(DosInterval);
        dosUpkeep();
    }

    tr_lpd_impl(tr_lpd_impl&&) = delete;
    tr_lpd_impl(tr_lpd_impl const&) = delete;
    tr_lpd_impl& operator=(tr_lpd_impl&&) = delete;
    tr_lpd_impl& operator=(tr_lpd_impl const&) = delete;

    ~tr_lpd_impl() override
    {
        for (auto& event : events_)
        {
            event.reset();
        }

        for (auto const sock : mcast_sockets_)
        {
            if (sock != TR_BAD_SOCKET)
            {
                tr_net_close_socket(sock);
            }
        }

        tr_logAddTrace("Done uninitialising Local Peer Discovery");
    }

private:
    bool init(struct event_base* event_base)
    {
        ipp_t n_success = NUM_TR_AF_INET_TYPES;
        if (!initImpl<TR_AF_INET>(event_base))
        {
            auto const err = sockerrno;
            tr_net_close_socket(mcast_sockets_[TR_AF_INET]);
            mcast_sockets_[TR_AF_INET] = TR_BAD_SOCKET;
            tr_logAddWarn(fmt::format(
                fmt::runtime(_("Couldn't initialize {ip_protocol} LPD: {error} ({error_code})")),
                fmt::arg("ip_protocol", tr_ip_protocol_to_sv(TR_AF_INET)),
                fmt::arg("error", tr_strerror(err)),
                fmt::arg("error_code", err)));
            --n_success;
        }

        if (!initImpl<TR_AF_INET6>(event_base))
        {
            auto const err = sockerrno;
            tr_net_close_socket(mcast_sockets_[TR_AF_INET6]);
            mcast_sockets_[TR_AF_INET6] = TR_BAD_SOCKET;
            tr_logAddWarn(fmt::format(
                fmt::runtime(_("Couldn't initialize {ip_protocol} LPD: {error} ({error_code})")),
                fmt::arg("ip_protocol", tr_ip_protocol_to_sv(TR_AF_INET6)),
                fmt::arg("error", tr_strerror(err)),
                fmt::arg("error_code", err)));
            --n_success;
        }

        return n_success != 0U;
    }

    /**
     * @brief Initializes Local Peer Discovery for this node
     *
     * For the most part, this means setting up an appropriately configured multicast socket
     * and event-based message handling.
     */
    template<tr_address_type ip_protocol>
    bool initImpl(struct event_base* event_base)
    {
        auto const opt_on = 1;
        auto& sock = mcast_sockets_[ip_protocol];

        static_assert(AnnounceScope > 0);
        static_assert(tr_address::is_valid(ip_protocol));

        tr_logAddDebug(fmt::format("Initialising {} Local Peer Discovery", tr_ip_protocol_to_sv(ip_protocol)));

        // setup datagram socket
        sock = socket(tr_ip_protocol_to_af(ip_protocol), SOCK_DGRAM, 0);

        if (sock == TR_BAD_SOCKET)
        {
            return false;
        }

        if (evutil_make_socket_nonblocking(sock) == -1)
        {
            return false;
        }

        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&opt_on), sizeof(opt_on)) == -1)
        {
            return false;
        }

#if HAVE_SO_REUSEPORT
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<char const*>(&opt_on), sizeof(opt_on)) == -1)
        {
            return false;
        }
#endif

        if constexpr (ip_protocol == TR_AF_INET6)
        {
            // must be done before binding on Linux
            if (evutil_make_listen_socket_ipv6only(sock) == -1)
            {
                return false;
            }
        }

        auto const mcast_sockaddr = tr_socket_address::from_string(McastSockAddr[ip_protocol]);
        TR_ASSERT(mcast_sockaddr);
        auto const [mcast_ss, mcast_sslen] = mcast_sockaddr->to_sockaddr();

        auto const [bind_ss, bind_sslen] = tr_socket_address::to_sockaddr(tr_address::any(ip_protocol), mcast_sockaddr->port());
        if (bind(sock, reinterpret_cast<sockaddr const*>(&bind_ss), bind_sslen) == -1)
        {
            return false;
        }

        if constexpr (ip_protocol == TR_AF_INET)
        {
            std::memcpy(&mcast_addr_, &mcast_ss, mcast_sslen);

            // we want to join that LPD multicast group
            struct ip_mreq mcast_req = {};
            mcast_req.imr_multiaddr = mcast_addr_.sin_addr;
            mcast_req.imr_interface = mediator_.bind_address(ip_protocol).addr.addr4;

            if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<char const*>(&mcast_req), sizeof(mcast_req)) ==
                -1)
            {
                return false;
            }

            // configure outbound multicast TTL
            if (setsockopt(
                    sock,
                    IPPROTO_IP,
                    IP_MULTICAST_TTL,
                    reinterpret_cast<char const*>(&AnnounceScope),
                    sizeof(AnnounceScope)) == -1)
            {
                return false;
            }

            if (setsockopt(
                    sock,
                    IPPROTO_IP,
                    IP_MULTICAST_IF,
                    reinterpret_cast<char const*>(&mcast_req.imr_interface),
                    sizeof(mcast_req.imr_interface)) == -1)
            {
                return false;
            }

            // needed to announce to BT clients on the same interface
            if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<char const*>(&opt_on), sizeof(opt_on)) == -1)
            {
                return false;
            }
        }
        else // TR_AF_INET6
        {
            std::memcpy(&mcast6_addr_, &mcast_ss, mcast_sslen);

            // we want to join that LPD multicast group
            struct ipv6_mreq mcast_req = {};
            mcast_req.ipv6mr_multiaddr = mcast6_addr_.sin6_addr;
            mcast_req.ipv6mr_interface = mediator_.bind_address(ip_protocol).to_interface_index().value_or(0);

            if (setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<char const*>(&mcast_req), sizeof(mcast_req)) ==
                -1)
            {
                return false;
            }

            // configure outbound multicast TTL
            if (setsockopt(
                    sock,
                    IPPROTO_IPV6,
                    IPV6_MULTICAST_HOPS,
                    reinterpret_cast<char const*>(&AnnounceScope),
                    sizeof(AnnounceScope)) == -1)
            {
                return false;
            }

            if (setsockopt(
                    sock,
                    IPPROTO_IPV6,
                    IPV6_MULTICAST_IF,
                    reinterpret_cast<char const*>(&mcast_req.ipv6mr_interface),
                    sizeof(mcast_req.ipv6mr_interface)) == -1)
            {
                return false;
            }

            // needed to announce to BT clients on the same interface
            if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, reinterpret_cast<char const*>(&opt_on), sizeof(opt_on)) ==
                -1)
            {
                return false;
            }
        }

        events_[ip_protocol].reset(event_new(event_base, sock, EV_READ | EV_PERSIST, event_callback<ip_protocol>, this));
        event_add(events_[ip_protocol].get(), nullptr);

        tr_logAddDebug(fmt::format("{} Local Peer Discovery initialised", tr_ip_protocol_to_sv(ip_protocol)));

        return true;
    }

    /**
    * @brief Processing of timeout notifications and incoming data on the socket
    * @note maximum rate of read events is limited according to @a lpd_maxAnnounceCap
    * @see DoS */
    template<tr_address_type ip_protocol>
    static void event_callback(evutil_socket_t /*s*/, short type, void* vself)
    {
        if ((type & EV_READ) != 0)
        {
            static_cast<tr_lpd_impl*>(vself)->onCanRead(ip_protocol);
        }
    }

    void onCanRead(tr_address_type ip_protocol)
    {
        TR_ASSERT(tr_address::is_valid(ip_protocol));
        if (!tr_address::is_valid(ip_protocol))
        {
            return;
        }

        if (!mediator_.allowsLPD())
        {
            return;
        }

        // process announcement from foreign peer
        struct sockaddr_storage foreign_addr = {};
        auto addr_len = socklen_t{ sizeof(foreign_addr) };
        auto foreign_msg = std::array<char, MaxDatagramLength>{};
        auto const res = recvfrom(
            mcast_sockets_[ip_protocol],
            std::data(foreign_msg),
            MaxDatagramLength,
            0,
            reinterpret_cast<sockaddr*>(&foreign_addr),
            &addr_len);

        // If we couldn't read it, discard it
        if (res < 1)
        {
            return;
        }
        TR_ASSERT(tr_af_to_ip_protocol(foreign_addr.ss_family) == ip_protocol);

        // If it doesn't look like a BEP14 message, discard it
        auto const msg = std::string_view{ std::data(foreign_msg), static_cast<size_t>(res) };
        if (static auto constexpr SearchKey = "BT-SEARCH * HTTP/"sv; msg.find(SearchKey) == std::string_view::npos)
        {
            return;
        }

        // If we're receiving too many, discard it
        if (++messages_received_since_upkeep_ > MaxIncomingPerUpkeep)
        {
            return;
        }

        // If it's an invalid message or the wrong protocol version, discard it.
        // Note this comes *after* incrementing the count since there is some
        // small CPU overhead in parsing, so don't do it for *every* message
        auto const parsed = parseAnnounceMsg(msg);
        if (!parsed || parsed->major != 1 || parsed->minor < 1 || parsed->cookie == cookie_)
        {
            tr_logAddTrace("Discarded invalid multicast message");
            return;
        }

        auto peer_sockaddr = tr_socket_address::from_sockaddr(reinterpret_cast<sockaddr*>(&foreign_addr));
        if (!peer_sockaddr)
        {
            return;
        }
        for (auto const& hash_string : parsed->info_hash_strings)
        {
            if (!mediator_.onPeerFound(hash_string, peer_sockaddr->address(), parsed->port))
            {
                tr_logAddDebug(fmt::format("Cannot serve torrent #{:s}", hash_string));
            }
        }
    }

    void announceUpkeep()
    {
        if (!mediator_.allowsLPD())
        {
            return;
        }

        auto torrents = mediator_.torrents();

        // remove torrents that don't need to be announced
        auto const now = tr_time();
        auto const needs_announce = [&now](auto& info)
        {
            return info.allows_lpd && (info.activity == TR_STATUS_DOWNLOAD || info.activity == TR_STATUS_SEED) &&
                info.announce_after < now;
        };
        torrents.erase(
            std::remove_if(std::begin(torrents), std::end(torrents), std::not_fn(needs_announce)),
            std::end(torrents));

        if (std::empty(torrents))
        {
            return;
        }

        // prioritize the remaining torrents
        static auto constexpr TorrentComparator = [](auto const& a, auto const& b)
        {
            if (a.activity != b.activity)
            {
                return a.activity < b.activity;
            }

            if (a.announce_after != b.announce_after)
            {
                return a.announce_after < b.announce_after;
            }
            return false;
        };
        std::sort(std::begin(torrents), std::end(torrents), TorrentComparator);

        auto const next_announce_after = now + TorrentAnnounceIntervalSec;
        for (ipp_t ipp = 0; ipp < NUM_TR_AF_INET_TYPES; ++ipp)
        {
            auto const ip_protocol = static_cast<tr_address_type>(ipp);

            // cram in as many as will fit in a message
            auto const baseline_size = std::size(makeAnnounceMsg(ip_protocol, cookie_, mediator_.port(), {}));
            auto const size_with_one = std::size(
                makeAnnounceMsg(ip_protocol, cookie_, mediator_.port(), { torrents.front().info_hash_str }));
            auto const size_per_hash = size_with_one - baseline_size;
            auto const max_torrents_per_announce = (MaxDatagramLength - baseline_size) / size_per_hash;
            auto const torrents_this_announce = std::min(std::size(torrents), max_torrents_per_announce);
            auto info_hash_strings = std::vector<std::string_view>{};
            info_hash_strings.reserve(torrents_this_announce);
            std::transform(
                std::begin(torrents),
                std::begin(torrents) + torrents_this_announce,
                std::back_inserter(info_hash_strings),
                [](auto const& tor) { return tor.info_hash_str; });

            if (!sendAnnounce(static_cast<tr_address_type>(ipp), info_hash_strings))
            {
                continue;
            }

            for (auto const& info_hash_string : info_hash_strings)
            {
                mediator_.setNextAnnounceTime(info_hash_string, next_announce_after);
            }
        }
    }

    void dosUpkeep()
    {
        if (messages_received_since_upkeep_ > MaxIncomingPerUpkeep)
        {
            tr_logAddTrace(fmt::format(
                "Dropped {} announces in the last interval (max. {} allowed)",
                messages_received_since_upkeep_ - MaxIncomingPerUpkeep,
                MaxIncomingPerUpkeep));
        }

        messages_received_since_upkeep_ = 0;
    }

    /**
     * @brief Announce the given torrent on the local network
     *
     * @return Returns a success flag
     *
     * Send a query for torrent t out to the LPD multicast group (or the LAN, for that
     * matter). A listening client on the same network might react by adding us to his
     * peer pool for torrent t.
     */
    bool sendAnnounce(tr_address_type ip_protocol, std::vector<std::string_view> const& info_hash_strings)
    {
        TR_ASSERT(tr_address::is_valid(ip_protocol));
        if (!tr_address::is_valid(ip_protocol))
        {
            return false;
        }

        if (mcast_sockets_[ip_protocol] == TR_BAD_SOCKET)
        {
            return true;
        }

        auto const announce = makeAnnounceMsg(ip_protocol, cookie_, mediator_.port(), info_hash_strings);
        TR_ASSERT(std::size(announce) <= MaxDatagramLength);
        auto const res = sendto(
            mcast_sockets_[ip_protocol],
            std::data(announce),
            std::size(announce),
            0,
            ip_protocol == TR_AF_INET ? reinterpret_cast<sockaddr const*>(&mcast_addr_) :
                                        reinterpret_cast<sockaddr const*>(&mcast6_addr_),
            ip_protocol == TR_AF_INET ? sizeof(mcast_addr_) : sizeof(mcast6_addr_));
        return res == static_cast<int>(std::size(announce));
    }

    std::string const cookie_ = makeCookie();
    Mediator& mediator_;
    std::array<tr_socket_t, NUM_TR_AF_INET_TYPES> mcast_sockets_ = { TR_BAD_SOCKET, TR_BAD_SOCKET }; // multicast sockets
    std::array<libtransmission::evhelpers::event_unique_ptr, NUM_TR_AF_INET_TYPES> events_;

    static auto constexpr MaxDatagramLength = size_t{ 1400 };
    sockaddr_in mcast_addr_ = {}; // initialized from the above constants in init()
    sockaddr_in6 mcast6_addr_ = {}; // initialized from the above constants in init()

    // BEP14: "To avoid causing multicast storms on large networks a
    // client should send no more than 1 announce per minute."
    static auto constexpr AnnounceInterval = 1min;
    std::unique_ptr<libtransmission::Timer> announce_timer_;

    // Flood Protection:
    // To protect against message flooding, stop processing search messages
    // after processing N per upkeep. If we hit that limit, we're either
    // in a *very* crowded multicast group or a hostile host is sending us
    // bogus data. Better to drop a few packets than get DoS'ed.
    static auto constexpr DosInterval = 5s;
    std::unique_ptr<libtransmission::Timer> dos_timer_;
    static auto constexpr MaxIncomingPerSecond = 10;
    static auto constexpr MaxIncomingPerUpkeep = std::chrono::duration_cast<std::chrono::seconds>(DosInterval).count() *
        MaxIncomingPerSecond;
    size_t messages_received_since_upkeep_ = 0U; // throw away messages after this number exceeds MaxIncomingPerUpkeep

    static auto constexpr TorrentAnnounceIntervalSec = time_t{ 240U }; // how frequently to reannounce the same torrent
    static auto constexpr TtlSameSubnet = 1;
    static auto constexpr AnnounceScope = int{ TtlSameSubnet }; // the maximum scope for LPD datagrams
};

std::unique_ptr<tr_lpd> tr_lpd::create(Mediator& mediator, struct event_base* event_base)
{
    return std::make_unique<tr_lpd_impl>(mediator, event_base);
}
