// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator> // std::back_inserter
#include <optional>
#include <string>
#include <string_view>
#include <utility> // std::pair

#ifdef _WIN32
#include <winsock2.h> // must come before iphlpapi.h
#include <iphlpapi.h>
#include <ws2tcpip.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/tcp.h> /* TCP_CONGESTION */
#endif

#include <event2/util.h>

#include <fmt/core.h>

#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/peer-socket.h"
#include "libtransmission/session.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"

using namespace std::literals;

std::string tr_net_strerror(int err)
{
#ifdef _WIN32

    auto buf = std::array<char, 512>{};
    (void)FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, std::data(buf), std::size(buf), nullptr);
    return std::string{ tr_strv_strip(std::data(buf)) };

#else

    return std::string{ tr_strerror(err) };

#endif
}

std::string_view tr_ip_protocol_to_sv(tr_address_type type)
{
    using namespace std::literals;

    switch (type)
    {
    case TR_AF_INET:
        return "IPv4"sv;
    case TR_AF_INET6:
        return "IPv6"sv;
    default:
        TR_ASSERT_MSG(false, "invalid address family");
        return {};
    }
}

int tr_ip_protocol_to_af(tr_address_type type)
{
    switch (type)
    {
    case TR_AF_INET:
        return AF_INET;
    case TR_AF_INET6:
        return AF_INET6;
    default:
        TR_ASSERT_MSG(false, "invalid address family");
        return {};
    }
}

tr_address_type tr_af_to_ip_protocol(int af)
{
    switch (af)
    {
    case AF_INET:
        return TR_AF_INET;
    case AF_INET6:
        return TR_AF_INET6;
    default:
        TR_ASSERT_MSG(false, "invalid address family");
        return NUM_TR_AF_INET_TYPES;
    }
}

// - TCP Sockets

[[nodiscard]] std::optional<tr_tos_t> tr_tos_t::from_string(std::string_view name)
{
    auto const needle = tr_strlower(tr_strv_strip(name));

    for (auto const& [value, key] : Names)
    {
        if (needle == key)
        {
            return tr_tos_t(value);
        }
    }

    if (auto value = tr_num_parse<int>(needle); value)
    {
        return tr_tos_t(*value);
    }

    return {};
}

std::string tr_tos_t::toString() const
{
    for (auto const& [value, key] : Names)
    {
        if (value_ == value)
        {
            return std::string{ key };
        }
    }

    return std::to_string(value_);
}

void tr_netSetTOS([[maybe_unused]] tr_socket_t s, [[maybe_unused]] int tos, tr_address_type type)
{
    if (s == TR_BAD_SOCKET)
    {
        return;
    }

    if (type == TR_AF_INET)
    {
#if defined(IP_TOS) && !defined(_WIN32)

        if (setsockopt(s, IPPROTO_IP, IP_TOS, (void const*)&tos, sizeof(tos)) == -1)
        {
            tr_logAddDebug(fmt::format("Can't set TOS '{}': {}", tos, tr_net_strerror(sockerrno)));
        }
#endif
    }
    else if (type == TR_AF_INET6)
    {
#if defined(IPV6_TCLASS) && !defined(_WIN32)
        if (setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, (void const*)&tos, sizeof(tos)) == -1)
        {
            tr_logAddDebug(fmt::format("Can't set IPv6 QoS '{}': {}", tos, tr_net_strerror(sockerrno)));
        }
#endif
    }
    else
    {
        /* program should never reach here! */
        tr_logAddDebug("Something goes wrong while setting TOS/Traffic-Class");
    }
}

void tr_netSetCongestionControl([[maybe_unused]] tr_socket_t s, [[maybe_unused]] char const* algorithm)
{
#ifdef TCP_CONGESTION

    if (setsockopt(s, IPPROTO_TCP, TCP_CONGESTION, (void const*)algorithm, strlen(algorithm) + 1) == -1)
    {
        tr_logAddDebug(fmt::format("Can't set congestion control algorithm '{}': {}", algorithm, tr_net_strerror(sockerrno)));
    }

#endif
}

namespace
{
tr_socket_t createSocket(int domain, int type)
{
    auto const sockfd = socket(domain, type, 0);
    if (sockfd == TR_BAD_SOCKET)
    {
        if (sockerrno != EAFNOSUPPORT)
        {
            tr_logAddWarn(fmt::format(
                _("Couldn't create socket: {error} ({error_code})"),
                fmt::arg("error", tr_net_strerror(sockerrno)),
                fmt::arg("error_code", sockerrno)));
        }

        return TR_BAD_SOCKET;
    }

    if (evutil_make_socket_nonblocking(sockfd) == -1)
    {
        tr_net_close_socket(sockfd);
        return TR_BAD_SOCKET;
    }

    if (static bool buf_logged = false; !buf_logged)
    {
        int i = 0;
        socklen_t size = sizeof(i);

        if (getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&i), &size) != -1)
        {
            tr_logAddTrace(fmt::format("SO_SNDBUF size is {}", i));
        }

        i = 0;
        size = sizeof(i);

        if (getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&i), &size) != -1)
        {
            tr_logAddTrace(fmt::format("SO_RCVBUF size is {}", i));
        }

        buf_logged = true;
    }

    return sockfd;
}
} // namespace

tr_socket_t tr_net_open_peer_socket(tr_session* session, tr_socket_address const& socket_address, bool client_is_seed)
{
    auto const& [addr, port] = socket_address;

    TR_ASSERT(addr.is_valid());
    TR_ASSERT(!tr_peer_socket::limit_reached(session));

    if (tr_peer_socket::limit_reached(session) || !session->allowsTCP() || !socket_address.is_valid())
    {
        return TR_BAD_SOCKET;
    }

    auto const s = createSocket(tr_ip_protocol_to_af(addr.type), SOCK_STREAM);
    if (s == TR_BAD_SOCKET)
    {
        return TR_BAD_SOCKET;
    }

    // seeds don't need a big read buffer, so make it smaller
    if (client_is_seed)
    {
        int n = 8192;

        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&n), sizeof(n)) == -1)
        {
            tr_logAddDebug(fmt::format("Unable to set SO_RCVBUF on socket {}: {}", s, tr_net_strerror(sockerrno)));
        }
    }

    auto const [sock, addrlen] = socket_address.to_sockaddr();

    // set source address
    auto const source_addr = session->bind_address(addr.type);
    auto const [source_sock, sourcelen] = tr_socket_address::to_sockaddr(source_addr, {});

    if (bind(s, reinterpret_cast<sockaddr const*>(&source_sock), sourcelen) == -1)
    {
        tr_logAddWarn(fmt::format(
            _("Couldn't set source address {address} on {socket}: {error} ({error_code})"),
            fmt::arg("address", source_addr.display_name()),
            fmt::arg("socket", s),
            fmt::arg("error", tr_net_strerror(sockerrno)),
            fmt::arg("error_code", sockerrno)));
        tr_net_close_socket(s);
        return TR_BAD_SOCKET;
    }

    if (connect(s, reinterpret_cast<sockaddr const*>(&sock), addrlen) == -1 &&
#ifdef _WIN32
        sockerrno != WSAEWOULDBLOCK &&
#endif
        sockerrno != EINPROGRESS)
    {
        if (auto const tmperrno = sockerrno;
            (tmperrno != ECONNREFUSED && tmperrno != ENETUNREACH && tmperrno != EHOSTUNREACH) || addr.is_ipv4())
        {
            tr_logAddWarn(fmt::format(
                _("Couldn't connect socket {socket} to {address}:{port}: {error} ({error_code})"),
                fmt::arg("socket", s),
                fmt::arg("address", addr.display_name()),
                fmt::arg("port", port.host()),
                fmt::arg("error", tr_net_strerror(tmperrno)),
                fmt::arg("error_code", tmperrno)));
        }

        tr_net_close_socket(s);
        return TR_BAD_SOCKET;
    }

    tr_logAddTrace(fmt::format("New OUTGOING connection {} ({})", s, socket_address.display_name()));

    return s;
}

namespace
{
tr_socket_t tr_netBindTCPImpl(tr_address const& addr, tr_port port, bool suppress_msgs, int* err_out)
{
    TR_ASSERT(addr.is_valid());

    auto const fd = socket(tr_ip_protocol_to_af(addr.type), SOCK_STREAM, 0);
    if (fd == TR_BAD_SOCKET)
    {
        *err_out = sockerrno;
        return TR_BAD_SOCKET;
    }

    if (evutil_make_socket_nonblocking(fd) == -1)
    {
        *err_out = sockerrno;
        tr_net_close_socket(fd);
        return TR_BAD_SOCKET;
    }

    int optval = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char const*>(&optval), sizeof(optval));
    (void)evutil_make_listen_socket_reuseable(fd);

    if (addr.is_ipv6() && evutil_make_listen_socket_ipv6only(fd) != 0 &&
        sockerrno != ENOPROTOOPT) // if the kernel doesn't support it, ignore it
    {
        *err_out = sockerrno;
        tr_net_close_socket(fd);
        return TR_BAD_SOCKET;
    }

    auto const [sock, addrlen] = tr_socket_address::to_sockaddr(addr, port);

    if (bind(fd, reinterpret_cast<sockaddr const*>(&sock), addrlen) == -1)
    {
        int const err = sockerrno;

        if (!suppress_msgs)
        {
            tr_logAddError(fmt::format(
                err == EADDRINUSE ?
                    _("Couldn't bind port {port} on {address}: {error} ({error_code}) -- Is another copy of Transmission already running?") :
                    _("Couldn't bind port {port} on {address}: {error} ({error_code})"),
                fmt::arg("address", addr.display_name()),
                fmt::arg("port", port.host()),
                fmt::arg("error", tr_net_strerror(err)),
                fmt::arg("error_code", err)));
        }

        tr_net_close_socket(fd);
        *err_out = err;
        return TR_BAD_SOCKET;
    }

    if (!suppress_msgs)
    {
        tr_logAddDebug(fmt::format("Bound socket {:d} to port {:d} on {:s}", fd, port.host(), addr.display_name()));
    }

#ifdef TCP_FASTOPEN

#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

    optval = 5;
    (void)setsockopt(fd, SOL_TCP, TCP_FASTOPEN, reinterpret_cast<char const*>(&optval), sizeof(optval));

#endif

#ifdef _WIN32
    if (listen(fd, SOMAXCONN) == -1)
#else /* _WIN32 */
    /* Listen queue backlog will be capped to the operating system's limit. */
    if (listen(fd, INT_MAX) == -1)
#endif /* _WIN32 */
    {
        *err_out = sockerrno;
        tr_net_close_socket(fd);
        return TR_BAD_SOCKET;
    }

    return fd;
}
} // namespace

tr_socket_t tr_netBindTCP(tr_address const& addr, tr_port port, bool suppress_msgs)
{
    int unused = 0;
    return tr_netBindTCPImpl(addr, port, suppress_msgs, &unused);
}

std::optional<std::pair<tr_socket_address, tr_socket_t>> tr_netAccept(tr_session* session, tr_socket_t listening_sockfd)
{
    TR_ASSERT(session != nullptr);

    // accept the incoming connection
    auto sock = sockaddr_storage{};
    socklen_t len = sizeof(struct sockaddr_storage);
    auto const sockfd = accept(listening_sockfd, reinterpret_cast<sockaddr*>(&sock), &len);
    if (sockfd == TR_BAD_SOCKET)
    {
        return {};
    }

    // get the address and port,
    // make the socket unblocking,
    // and confirm we don't have too many peers
    auto const addrport = tr_socket_address::from_sockaddr(reinterpret_cast<struct sockaddr*>(&sock));
    if (!addrport || evutil_make_socket_nonblocking(sockfd) == -1 || tr_peer_socket::limit_reached(session))
    {
        tr_net_close_socket(sockfd);
        return {};
    }

    return std::pair{ *addrport, sockfd };
}

void tr_net_close_socket(tr_socket_t sockfd)
{
    evutil_closesocket(sockfd);
}

// ---

namespace
{
namespace is_valid_for_peers_helpers
{

/* isMartianAddr was written by Juliusz Chroboczek,
   and is covered under the same license as third-party/dht/dht.c. */
[[nodiscard]] auto is_martian_addr(tr_address const& addr, tr_peer_from from)
{
    static auto constexpr Zeroes = std::array<unsigned char, 16>{};
    auto const loopback_allowed = from == TR_PEER_FROM_INCOMING || from == TR_PEER_FROM_LPD || from == TR_PEER_FROM_RESUME;

    switch (addr.type)
    {
    case TR_AF_INET:
        {
            auto const* const address = reinterpret_cast<unsigned char const*>(&addr.addr.addr4);
            return address[0] == 0 || // 0.x.x.x
                (!loopback_allowed && address[0] == 127) || // 127.x.x.x
                (address[0] & 0xE0) == 0xE0; // multicast address
        }

    case TR_AF_INET6:
        {
            auto const* const address = reinterpret_cast<unsigned char const*>(&addr.addr.addr6);
            return address[0] == 0xFF || // multicast address
                (std::memcmp(address, std::data(Zeroes), 15) == 0 &&
                 (address[15] == 0 || // ::
                  (!loopback_allowed && address[15] == 1)) // ::1
                );
        }

    default:
        return true;
    }
}

} // namespace is_valid_for_peers_helpers
} // namespace

// --- tr_port

std::pair<tr_port, std::byte const*> tr_port::from_compact(std::byte const* compact) noexcept
{
    static auto constexpr PortLen = size_t{ 2 };

    static_assert(PortLen == sizeof(uint16_t));
    auto nport = uint16_t{};
    std::copy_n(compact, PortLen, reinterpret_cast<std::byte*>(&nport));
    compact += PortLen;

    return std::make_pair(tr_port::from_network(nport), compact);
}

// --- tr_address

std::optional<tr_address> tr_address::from_string(std::string_view address_sv)
{
    auto const address_sz = tr_strbuf<char, TR_ADDRSTRLEN>{ address_sv };

    auto ss = sockaddr_storage{};
    auto sslen = int{ sizeof(ss) };
    if (evutil_parse_sockaddr_port(address_sz, reinterpret_cast<sockaddr*>(&ss), &sslen) != 0)
    {
        return {};
    }

    auto addr = tr_address{};
    switch (ss.ss_family)
    {
    case AF_INET:
        addr.addr.addr4 = reinterpret_cast<sockaddr_in*>(&ss)->sin_addr;
        addr.type = TR_AF_INET;
        return addr;

    case AF_INET6:
        addr.addr.addr6 = reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr;
        addr.type = TR_AF_INET6;
        return addr;

    default:
        return {};
    }
}

std::string_view tr_address::display_name(char* out, size_t outlen) const
{
    TR_ASSERT(is_valid());
    if (auto* name = evutil_inet_ntop(tr_ip_protocol_to_af(type), &addr, out, outlen))
    {
        return name;
    }
    return "Invalid address"sv;
}

[[nodiscard]] std::string tr_address::display_name() const
{
    auto buf = std::array<char, std::max(INET_ADDRSTRLEN, INET6_ADDRSTRLEN)>{};
    return std::string{ display_name(std::data(buf), std::size(buf)) };
}

std::pair<tr_address, std::byte const*> tr_address::from_compact_ipv4(std::byte const* compact) noexcept
{
    static auto constexpr Addr4Len = tr_address::CompactAddrBytes[TR_AF_INET];

    auto address = tr_address{};
    static_assert(sizeof(address.addr.addr4) == Addr4Len);
    address.type = TR_AF_INET;
    std::copy_n(compact, Addr4Len, reinterpret_cast<std::byte*>(&address.addr));
    compact += Addr4Len;

    return { address, compact };
}

std::pair<tr_address, std::byte const*> tr_address::from_compact_ipv6(std::byte const* compact) noexcept
{
    static auto constexpr Addr6Len = tr_address::CompactAddrBytes[TR_AF_INET6];

    auto address = tr_address{};
    address.type = TR_AF_INET6;
    std::copy_n(compact, Addr6Len, reinterpret_cast<std::byte*>(&address.addr.addr6.s6_addr));
    compact += Addr6Len;

    return { address, compact };
}

std::optional<unsigned> tr_address::to_interface_index() const noexcept
{
    if (!is_valid())
    {
        tr_logAddDebug("Invalid target address to find interface index");
        return {};
    }

    tr_logAddDebug(fmt::format("Find interface index for {}", display_name()));

#ifdef _WIN32
    auto p_addresses = std::unique_ptr<void, void (*)(void*)>{ nullptr, operator delete };

    // The recommended method of calling the GetAdaptersAddresses function is to
    // pre-allocate a 15KB working buffer pointed to by the AdapterAddresses parameter.
    // On typical computers, this dramatically reduces the chances that the
    // GetAdaptersAddresses function returns ERROR_BUFFER_OVERFLOW, which would require
    // calling GetAdaptersAddresses function multiple times.
    // https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersaddresses
    for (auto p_addresses_size = ULONG{ 15000 } /* 15KB */;;)
    {
        p_addresses.reset(operator new(p_addresses_size, std::nothrow));
        if (!p_addresses)
        {
            tr_logAddDebug("Could not allocate memory for interface list");
            return {};
        }

        if (auto ret = GetAdaptersAddresses(
                AF_UNSPEC,
                GAA_FLAG_SKIP_FRIENDLY_NAME,
                nullptr,
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(p_addresses.get()),
                &p_addresses_size);
            ret != ERROR_BUFFER_OVERFLOW)
        {
            if (ret != ERROR_SUCCESS)
            {
                tr_logAddDebug(fmt::format("Failed to retrieve interface list: {} ({})", ret, tr_win32_format_message(ret)));
                return {};
            }
            break;
        }
    }

    for (auto const* cur = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(p_addresses.get()); cur != nullptr; cur = cur->Next)
    {
        if (cur->OperStatus != IfOperStatusUp)
        {
            continue;
        }

        for (auto const* sa_p = cur->FirstUnicastAddress; sa_p != nullptr; sa_p = sa_p->Next)
        {
            if (auto if_addr = tr_socket_address::from_sockaddr(sa_p->Address.lpSockaddr);
                if_addr && if_addr->address() == *this)
            {
                auto const ret = type == TR_AF_INET ? cur->IfIndex : cur->Ipv6IfIndex;
                tr_logAddDebug(fmt::format("Found interface index for {}: {}", display_name(), ret));
                return ret;
            }
        }
    }
#else
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0)
    {
        auto err = errno;
        tr_logAddDebug(fmt::format("Failed to retrieve interface list: {} ({})", err, tr_strerror(err)));
        return {};
    }
    auto const ifa_uniq = std::unique_ptr<ifaddrs, void (*)(struct ifaddrs*)>{ ifa, freeifaddrs };

    for (; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr || (ifa->ifa_flags & IFF_UP) == 0U)
        {
            continue;
        }

        if (auto if_addr = tr_socket_address::from_sockaddr(ifa->ifa_addr); if_addr && if_addr->address() == *this)
        {
            auto const ret = if_nametoindex(ifa->ifa_name);
            tr_logAddDebug(fmt::format("Found interface index for {}: {}", display_name(), ret));
            return ret;
        }
    }
#endif

    tr_logAddDebug(fmt::format("Could not find interface index for {}", display_name()));
    return {};
}

int tr_address::compare(tr_address const& that) const noexcept // <=>
{
    // IPv6 addresses are always "greater than" IPv4
    if (auto const val = tr_compare_3way(this->type, that.type); val != 0)
    {
        return val;
    }

    return this->is_ipv4() ? memcmp(&this->addr.addr4, &that.addr.addr4, sizeof(this->addr.addr4)) :
                             memcmp(&this->addr.addr6.s6_addr, &that.addr.addr6.s6_addr, sizeof(this->addr.addr6.s6_addr));
}

// https://en.wikipedia.org/wiki/Reserved_IP_addresses
[[nodiscard]] bool tr_address::is_global_unicast_address() const noexcept
{
    if (is_ipv4())
    {
        auto const* const a = reinterpret_cast<uint8_t const*>(&addr.addr4.s_addr);

        // [0.0.0.0–0.255.255.255]
        // Current network.
        if (a[0] == 0)
        {
            return false;
        }

        // [10.0.0.0 – 10.255.255.255]
        // Used for local communications within a private network.
        if (a[0] == 10)
        {
            return false;
        }

        // [100.64.0.0–100.127.255.255]
        // Shared address space for communications between a service provider
        // and its subscribers when using a carrier-grade NAT.
        if ((a[0] == 100) && (64 <= a[1] && a[1] <= 127))
        {
            return false;
        }

        // [169.254.0.0–169.254.255.255]
        // Used for link-local addresses[5] between two hosts on a single link
        // when no IP address is otherwise specified, such as would have
        // normally been retrieved from a DHCP server.
        if (a[0] == 169 && a[1] == 254)
        {
            return false;
        }

        // [172.16.0.0–172.31.255.255]
        // Used for local communications within a private network.
        if ((a[0] == 172) && (16 <= a[1] && a[1] <= 31))
        {
            return false;
        }

        // [192.0.0.0–192.0.0.255]
        // IETF Protocol Assignments.
        if (a[0] == 192 && a[1] == 0 && a[2] == 0)
        {
            return false;
        }

        // [192.0.2.0–192.0.2.255]
        // Assigned as TEST-NET-1, documentation and examples.
        if (a[0] == 192 && a[1] == 0 && a[2] == 2)
        {
            return false;
        }

        // [192.88.99.0–192.88.99.255]
        // Reserved. Formerly used for IPv6 to IPv4 relay.
        if (a[0] == 192 && a[1] == 88 && a[2] == 99)
        {
            return false;
        }

        // [192.168.0.0–192.168.255.255]
        // Used for local communications within a private network.
        if (a[0] == 192 && a[1] == 168)
        {
            return false;
        }

        // [198.18.0.0–198.19.255.255]
        // Used for benchmark testing of inter-network communications
        // between two separate subnets.
        if (a[0] == 198 && (18 <= a[1] && a[1] <= 19))
        {
            return false;
        }

        // [198.51.100.0–198.51.100.255]
        // Assigned as TEST-NET-2, documentation and examples.
        if (a[0] == 198 && a[1] == 51 && a[2] == 100)
        {
            return false;
        }

        // [203.0.113.0–203.0.113.255]
        // Assigned as TEST-NET-3, documentation and examples.
        if (a[0] == 203 && a[1] == 0 && a[2] == 113)
        {
            return false;
        }

        // [224.0.0.0–239.255.255.255]
        // In use for IP multicast. (Former Class D network.)
        if (224 <= a[0] && a[0] <= 230)
        {
            return false;
        }

        // [233.252.0.0-233.252.0.255]
        // Assigned as MCAST-TEST-NET, documentation and examples.
        if (a[0] == 233 && a[1] == 252 && a[2] == 0)
        {
            return false;
        }

        // [240.0.0.0–255.255.255.254]
        // Reserved for future use. (Former Class E network.)
        // [255.255.255.255]
        // Reserved for the "limited broadcast" destination address.
        if (240 <= a[0])
        {
            return false;
        }

        return true;
    }

    if (is_ipv6())
    {
        auto const* const a = addr.addr6.s6_addr;

        // TODO: 2000::/3 is commonly used for global unicast but technically
        // other spaces would be allowable too, so we should test those here.
        // See RFC 4291 in the Section 2.4 listing global unicast as everything
        // that's not link-local, multicast, loopback, or unspecified.
        return (a[0] & 0xE0) == 0x20;
    }

    return false;
}

// --- tr_socket_addrses

std::string tr_socket_address::display_name(tr_address const& address, tr_port port) noexcept
{
    return fmt::format(address.is_ipv6() ? "[{:s}]:{:d}" : "{:s}:{:d}", address.display_name(), port.host());
}

bool tr_socket_address::is_valid_for_peers(tr_peer_from from) const noexcept
{
    using namespace is_valid_for_peers_helpers;

    return is_valid() && !std::empty(port_) && !address_.is_ipv6_link_local_address() && !address_.is_ipv4_mapped_address() &&
        !is_martian_addr(address_, from);
}

std::optional<tr_socket_address> tr_socket_address::from_string(std::string_view sockaddr_sv)
{
    auto ss = sockaddr_storage{};
    auto sslen = int{ sizeof(ss) };
    if (evutil_parse_sockaddr_port(tr_strbuf<char, TR_ADDRSTRLEN>{ sockaddr_sv }, reinterpret_cast<sockaddr*>(&ss), &sslen) !=
        0)
    {
        return {};
    }

    return from_sockaddr(reinterpret_cast<struct sockaddr const*>(&ss));
}

std::optional<tr_socket_address> tr_socket_address::from_sockaddr(struct sockaddr const* from)
{
    if (from == nullptr)
    {
        return {};
    }

    if (from->sa_family == AF_INET)
    {
        auto const* const sin = reinterpret_cast<struct sockaddr_in const*>(from);
        auto addr = tr_address{};
        addr.type = TR_AF_INET;
        addr.addr.addr4 = sin->sin_addr;
        return tr_socket_address{ addr, tr_port::from_network(sin->sin_port) };
    }

    if (from->sa_family == AF_INET6)
    {
        auto const* const sin6 = reinterpret_cast<struct sockaddr_in6 const*>(from);
        auto addr = tr_address{};
        addr.type = TR_AF_INET6;
        addr.addr.addr6 = sin6->sin6_addr;
        return tr_socket_address{ addr, tr_port::from_network(sin6->sin6_port) };
    }

    tr_logAddDebug(fmt::format("Unsupported address family {:d}", from->sa_family));
    return {};
}

std::pair<sockaddr_storage, socklen_t> tr_socket_address::to_sockaddr(tr_address const& addr, tr_port port) noexcept
{
    auto ss = sockaddr_storage{};

    if (addr.is_ipv4())
    {
        auto* const ss4 = reinterpret_cast<sockaddr_in*>(&ss);
        ss4->sin_addr = addr.addr.addr4;
        ss4->sin_family = AF_INET;
        ss4->sin_port = port.network();
        return { ss, sizeof(sockaddr_in) };
    }

    auto* const ss6 = reinterpret_cast<sockaddr_in6*>(&ss);
    ss6->sin6_addr = addr.addr.addr6;
    ss6->sin6_family = AF_INET6;
    ss6->sin6_flowinfo = 0;
    ss6->sin6_port = port.network();
    return { ss, sizeof(sockaddr_in6) };
}
