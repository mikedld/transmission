// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::all_of
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility> // std::move

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <sys/socket.h>
#endif

#include <fmt/core.h>

#include "libtransmission/ip-cache.h"
#include "libtransmission/log.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h"
#include "libtransmission/web.h"

namespace
{

using namespace std::literals;

static_assert(TR_AF_INET == 0);
static_assert(TR_AF_INET6 == 1);

auto constexpr IPQueryServices = std::array{ std::array{ "https://ip4.transmissionbt.com/"sv },
                                             std::array{ "https://ip6.transmissionbt.com/"sv } };

auto constexpr UpkeepInterval = 30min;
auto constexpr RetryUpkeepInterval = 30s;

// Normally there should only ever be 1 cache instance during the entire program execution
// This is a counter only to cater for SessionTest.honorsSettings
std::size_t cache_exists = 0;

} // namespace

namespace
{
// Functions contained in global_source_ip_helpers are modified from code
// by Juliusz Chroboczek and is covered under the same license as dht.cc.
// Please feel free to copy them into your software if it can help
// unbreaking the double-stack Internet.
namespace global_source_ip_helpers
{

// Get the source address used for a given destination address.
// Since there is no official interface to get this information,
// we create a connected UDP socket (connected UDP... hmm...)
// and check its source address.
//
// Since it's a UDP socket, this doesn't actually send any packets
[[nodiscard]] std::optional<tr_address> get_source_address(
    tr_address const& dst_addr,
    tr_port dst_port,
    tr_address const& bind_addr,
    int& err_out) noexcept
{
    TR_ASSERT(dst_addr.type == bind_addr.type);

    auto const save = errno;

    auto const [dst_ss, dst_sslen] = tr_socket_address::to_sockaddr(dst_addr, dst_port);
    auto const [bind_ss, bind_sslen] = tr_socket_address::to_sockaddr(bind_addr, {});
    if (auto const sock = socket(dst_ss.ss_family, SOCK_DGRAM, 0); sock != TR_BAD_SOCKET)
    {
        if (bind(sock, reinterpret_cast<sockaddr const*>(&bind_ss), bind_sslen) == 0)
        {
            if (connect(sock, reinterpret_cast<sockaddr const*>(&dst_ss), dst_sslen) == 0)
            {
                auto src_ss = sockaddr_storage{};
                auto src_sslen = socklen_t{ sizeof(src_ss) };
                if (getsockname(sock, reinterpret_cast<sockaddr*>(&src_ss), &src_sslen) == 0)
                {
                    if (auto const addrport = tr_socket_address::from_sockaddr(reinterpret_cast<sockaddr*>(&src_ss)); addrport)
                    {
                        tr_net_close_socket(sock);
                        errno = save;
                        return addrport->address();
                    }
                }
            }
        }

        tr_net_close_socket(sock);
    }

    err_out = errno;
    errno = save;
    return {};
}

[[nodiscard]] std::optional<tr_address> get_global_source_address(tr_address const& bind_addr, int& err_out) noexcept
{
    // Pick some destination address to pretend to send a packet to
    static auto constexpr DstIP = std::array{ "91.121.74.28"sv, "2001:1890:1112:1::20"sv };
    auto const dst_addr = tr_address::from_string(DstIP[bind_addr.type]);
    auto const dst_port = tr_port::from_host(6969);

    // In order for address selection to work right,
    // this should be a global unicast address, not Teredo or 6to4
    TR_ASSERT(dst_addr && dst_addr->is_global_unicast_address());

    if (dst_addr)
    {
        return get_source_address(*dst_addr, dst_port, bind_addr, err_out);
    }

    return {};
}

} // namespace global_source_ip_helpers
} // namespace

tr_ip_cache::tr_ip_cache(Mediator& mediator_in)
    : mediator_{ mediator_in }
    , upkeep_timers_{ mediator_in.timer_maker().create(), mediator_in.timer_maker().create() }
{
    static_assert(TR_AF_INET == 0);
    static_assert(TR_AF_INET6 == 1);
    static_assert(NUM_TR_AF_INET_TYPES == 2);

    for (std::size_t i = 0; i < NUM_TR_AF_INET_TYPES; ++i)
    {
        auto const type = static_cast<tr_address_type>(i);
        auto const cb = [this, type]()
        {
            update_addr(type);
        };
        upkeep_timers_[i]->set_callback(cb);
        start_timer(type, UpkeepInterval);
    }

    ++cache_exists;
}

tr_ip_cache::~tr_ip_cache()
{
    // Destroying mutex while someone owns it is undefined behaviour, so we acquire it first
    auto const locks = std::scoped_lock{ global_addr_mutex_[TR_AF_INET],
                                         global_addr_mutex_[TR_AF_INET6],
                                         source_addr_mutex_[TR_AF_INET],
                                         source_addr_mutex_[TR_AF_INET6] };

    if (!std::all_of(
            std::begin(is_updating_),
            std::end(is_updating_),
            [](is_updating_t const& v) { return v == is_updating_t::ABORT; }))
    {
        tr_logAddDebug("Destructed while some global IP queries were pending.");
    }

    --cache_exists;
}

bool tr_ip_cache::try_shutdown() noexcept
{
    for (auto& timer : upkeep_timers_)
    {
        timer->stop();
    }

    for (std::size_t i = 0; i < NUM_TR_AF_INET_TYPES; ++i)
    {
        if (is_updating_[i] == is_updating_t::YES)
        {
            return false;
        }
        is_updating_[i] = is_updating_t::ABORT; // Abort any future updates
    }
    return true;
}

tr_address tr_ip_cache::bind_addr(tr_address_type type) const noexcept
{
    if (tr_address::is_valid(type))
    {
        if (auto const addr = tr_address::from_string(mediator_.settings_bind_addr(type)); addr && type == addr->type)
        {
            return *addr;
        }
        return tr_address::any(type);
    }

    TR_ASSERT_MSG(false, "invalid type");
    return {};
}

bool tr_ip_cache::set_global_addr(tr_address_type type, tr_address const& addr) noexcept
{
    if (type == addr.type && addr.is_global_unicast_address())
    {
        auto const lock = std::scoped_lock{ global_addr_mutex_[addr.type] };
        global_addr_[addr.type] = addr;
        tr_logAddTrace(fmt::format("Cached global address {}", addr.display_name()));
        return true;
    }
    return false;
}

void tr_ip_cache::update_addr(tr_address_type type) noexcept
{
    update_source_addr(type);
    if (global_source_addr(type))
    {
        update_global_addr(type);
    }
}

void tr_ip_cache::update_global_addr(tr_address_type type) noexcept
{
    auto const ix_service = ix_service_[type];
    TR_ASSERT(has_ip_protocol_[type]);
    TR_ASSERT(ix_service < std::size(IPQueryServices[type]));

    if (ix_service == 0U && !set_is_updating(type))
    {
        return;
    }
    TR_ASSERT(is_updating_[type] == is_updating_t::YES);

    // Update global address
    static auto constexpr IPProtocolMap = std::array{ tr_web::FetchOptions::IPProtocol::V4,
                                                      tr_web::FetchOptions::IPProtocol::V6 };
    auto options = tr_web::FetchOptions{ IPQueryServices[type][ix_service],
                                         [this, type](tr_web::FetchResponse const& response)
                                         {
                                             // Check to avoid segfault
                                             if (cache_exists != 0)
                                             {
                                                 this->on_response_ip_query(type, response);
                                             }
                                         },
                                         nullptr };
    options.ip_proto = IPProtocolMap[type];
    options.sndbuf = 4096;
    options.rcvbuf = 4096;
    mediator_.fetch(std::move(options));
}

void tr_ip_cache::update_source_addr(tr_address_type type) noexcept
{
    using namespace global_source_ip_helpers;

    auto& has_ip_protocol = has_ip_protocol_[type];
    TR_ASSERT(has_ip_protocol);

    if (!set_is_updating(type))
    {
        return;
    }
    TR_ASSERT(is_updating_[type] == is_updating_t::YES);

    auto const protocol = tr_ip_protocol_to_sv(type);

    auto err = 0;
    auto const& source_addr = get_global_source_address(bind_addr(type), err);
    if (source_addr)
    {
        set_source_addr(*source_addr);
        tr_logAddDebug(fmt::format(
            _("Successfully updated source {protocol} address to {ip}"),
            fmt::arg("protocol", protocol),
            fmt::arg("ip", source_addr->display_name())));
    }
    else
    {
        // Stop the update process since we have no public internet connectivity
        unset_addr(type);
        upkeep_timers_[type]->set_interval(RetryUpkeepInterval);
        tr_logAddDebug(fmt::format("Couldn't obtain source {} address", protocol));
        if (err == EAFNOSUPPORT)
        {
            stop_timer(type); // No point in retrying
            has_ip_protocol = false;
            tr_logAddInfo(fmt::format(_("Your machine does not support {protocol}"), fmt::arg("protocol", protocol)));
        }
    }

    unset_is_updating(type);
}

void tr_ip_cache::on_response_ip_query(tr_address_type type, tr_web::FetchResponse const& response) noexcept
{
    auto& ix_service = ix_service_[type];
    TR_ASSERT(is_updating_[type] == is_updating_t::YES);
    TR_ASSERT(ix_service < std::size(IPQueryServices[type]));

    auto const protocol = tr_ip_protocol_to_sv(type);
    auto success = false;

    if (response.status == 200 /* HTTP_OK */)
    {
        // Update member
        if (auto const addr = tr_address::from_string(tr_strv_strip(response.body)); addr && set_global_addr(type, *addr))
        {
            success = true;
            upkeep_timers_[type]->set_interval(UpkeepInterval);

            tr_logAddDebug(fmt::format(
                _("Successfully updated global {type} address to {ip} using {url}"),
                fmt::arg("type", protocol),
                fmt::arg("ip", addr->display_name()),
                fmt::arg("url", IPQueryServices[type][ix_service])));
        }
    }

    // Try next IP query URL
    if (!success)
    {
        if (++ix_service < std::size(IPQueryServices[type]))
        {
            update_global_addr(type);
            return;
        }

        tr_logAddDebug(fmt::format("Couldn't obtain global {} address", protocol));
        unset_global_addr(type);
        upkeep_timers_[type]->set_interval(RetryUpkeepInterval);
    }

    ix_service = 0U;
    unset_is_updating(type);
}

void tr_ip_cache::unset_global_addr(tr_address_type type) noexcept
{
    auto const lock = std::scoped_lock{ global_addr_mutex_[type] };
    global_addr_[type].reset();
    tr_logAddTrace(fmt::format("Unset {} global address cache", tr_ip_protocol_to_sv(type)));
}

void tr_ip_cache::set_source_addr(tr_address const& addr) noexcept
{
    auto const lock = std::scoped_lock{ source_addr_mutex_[addr.type] };
    source_addr_[addr.type] = addr;
    tr_logAddTrace(fmt::format("Cached source address {}", addr.display_name()));
}

void tr_ip_cache::unset_addr(tr_address_type type) noexcept
{
    auto const lock = std::scoped_lock{ source_addr_mutex_[type] };
    source_addr_[type].reset();
    tr_logAddTrace(fmt::format("Unset {} source address cache", tr_ip_protocol_to_sv(type)));

    // No public internet connectivity means no global IP address
    unset_global_addr(type);
}

bool tr_ip_cache::set_is_updating(tr_address_type type) noexcept
{
    if (is_updating_[type] != is_updating_t::NO)
    {
        return false;
    }
    is_updating_[type] = is_updating_t::YES;
    return true;
}

void tr_ip_cache::unset_is_updating(tr_address_type type) noexcept
{
    TR_ASSERT(is_updating_[type] == is_updating_t::YES);
    is_updating_[type] = is_updating_t::NO;
}
