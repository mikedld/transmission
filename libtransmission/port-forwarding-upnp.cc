// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <fmt/core.h>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

#define LIBTRANSMISSION_PORT_FORWARDING_MODULE

#include "libtransmission/transmission.h"

#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/port-forwarding-upnp.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h" // TR_ADDRSTRLEN
#include "libtransmission/utils.h" // for _(), tr_strerror()

#ifndef MINIUPNPC_API_VERSION
#error miniupnpc >= 1.7 is required
#endif

namespace
{
enum class UpnpState : uint8_t
{
    Idle,
    WillDiscover, // next action is upnpDiscover()
    Discovering, // currently making blocking upnpDiscover() call in a worker thread
    WillMap, // next action is UPNP_AddPortMapping()
    WillUnmap // next action is UPNP_DeletePortMapping()
};
} // namespace

struct tr_upnp
{
    tr_upnp() = default;
    tr_upnp(tr_upnp&&) = delete;
    tr_upnp(tr_upnp const&) = delete;
    tr_upnp& operator=(tr_upnp&&) = delete;
    tr_upnp& operator=(tr_upnp const&) = delete;

    ~tr_upnp()
    {
        TR_ASSERT(!isMapped);
        TR_ASSERT(state == UpnpState::Idle || state == UpnpState::WillDiscover || state == UpnpState::Discovering);

        FreeUPNPUrls(&urls);
    }

    UPNPUrls urls = {};
    IGDdatas data = {};
    tr_port advertised_port;
    tr_port local_port;
    std::string lanaddr;
    bool isMapped = false;
    UpnpState state = UpnpState::WillDiscover;

    // Used to return the results of upnpDiscover() from a worker thread
    // to be processed without blocking in tr_upnpPulse().
    // This will be pending while the state is UpnpState::DISCOVERING.
    std::optional<std::future<UPNPDev*>> discover_future;
};

namespace
{
constexpr auto port_fwd_state(UpnpState upnp_state, bool is_mapped)
{
    switch (upnp_state)
    {
    case UpnpState::WillDiscover:
    case UpnpState::Discovering:
        return TR_PORT_UNMAPPED;

    case UpnpState::WillMap:
        return TR_PORT_MAPPING;

    case UpnpState::WillUnmap:
        return TR_PORT_UNMAPPING;

    case UpnpState::Idle:
        return is_mapped ? TR_PORT_MAPPED : TR_PORT_UNMAPPED;

    default: // UpnpState::FAILED:
        return TR_PORT_ERROR;
    }
}

[[nodiscard]] UPNPDev* upnp_discover(int msec, char const* bindaddr)
{
    UPNPDev* ret = nullptr;
    auto have_err = bool{};

    // MINIUPNPC_API_VERSION >= 8 (adds ipv6 and error args)
    int err = UPNPDISCOVER_SUCCESS;

#if (MINIUPNPC_API_VERSION >= 14) // adds ttl
    ret = upnpDiscover(msec, bindaddr, nullptr, 0, 0, 2, &err);
#else
    ret = upnpDiscover(msec, bindaddr, nullptr, 0, 0, &err);
#endif

    have_err = err != UPNPDISCOVER_SUCCESS;

    if (have_err)
    {
        tr_logAddDebug(fmt::format("upnpDiscover failed: {} ({})", tr_strerror(errno), errno));
    }

    return ret;
}

[[nodiscard]] int get_specific_port_mapping_entry(tr_upnp const* handle, char const* proto)
{
    auto int_client = std::array<char, 16>{};
    auto int_port = std::array<char, 6>{};

    auto const port_str = std::to_string(handle->advertised_port.host());

#if (MINIUPNPC_API_VERSION >= 10) /* adds remoteHost arg */
    int const err = UPNP_GetSpecificPortMappingEntry(
        handle->urls.controlURL,
        handle->data.first.servicetype,
        port_str.c_str(),
        proto,
        nullptr /*remoteHost*/,
        std::data(int_client),
        std::data(int_port),
        nullptr /*desc*/,
        nullptr /*enabled*/,
        nullptr /*duration*/);
#else // MINIUPNPC_API_VERSION >= 8 (adds desc, enabled and leaseDuration args)
    int const err = UPNP_GetSpecificPortMappingEntry(
        handle->urls.controlURL,
        handle->data.first.servicetype,
        port_str.c_str(),
        proto,
        std::data(int_client),
        std::data(int_port),
        nullptr /*desc*/,
        nullptr /*enabled*/,
        nullptr /*duration*/);
#endif

    return err;
}

[[nodiscard]] int upnp_add_port_mapping(
    tr_upnp const* handle,
    char const* proto,
    tr_port advertised_port,
    tr_port local_port,
    char const* desc)
{
    int const old_errno = errno;
    errno = 0;

    auto const advertised_port_str = std::to_string(advertised_port.host());
    auto const local_port_str = std::to_string(local_port.host());

    // MINIUPNPC_API_VERSION >= 8
    int const err = UPNP_AddPortMapping(
        handle->urls.controlURL,
        handle->data.first.servicetype,
        advertised_port_str.c_str(),
        local_port_str.c_str(),
        handle->lanaddr.c_str(),
        desc,
        proto,
        nullptr,
        nullptr);

    if (err != 0)
    {
        tr_logAddDebug(fmt::format("{} Port forwarding failed with error {}: {} ({})", proto, err, tr_strerror(errno), errno));
    }

    errno = old_errno;
    return err;
}

void tr_upnpDeletePortMapping(tr_upnp const* handle, char const* proto, tr_port advertised_port)
{
    auto const port_str = std::to_string(advertised_port.host());

    UPNP_DeletePortMapping(handle->urls.controlURL, handle->data.first.servicetype, port_str.c_str(), proto, nullptr);
}

enum : uint8_t
{
    UPNP_IGD_NONE = 0,
    UPNP_IGD_VALID_CONNECTED = 1,
    UPNP_IGD_VALID_NOT_CONNECTED = 2,
    UPNP_IGD_INVALID = 3
};

auto* discover_thread_func(std::string bindaddr) // NOLINT performance-unnecessary-value-param
{
    // If multicastif is not NULL, it will be used instead of the default
    // multicast interface for sending SSDP discover packets.
    char const* multicastif = std::empty(bindaddr) ? nullptr : bindaddr.c_str();
    return upnp_discover(2000, multicastif);
}

template<typename T>
bool is_future_ready(std::future<T> const& future)
{
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
} // namespace

// ---

tr_upnp* tr_upnpInit()
{
    return new tr_upnp();
}

void tr_upnpClose(tr_upnp* handle)
{
    delete handle;
}

tr_port_forwarding_state tr_upnpPulse(
    tr_upnp* handle,
    tr_port advertised_port,
    tr_port local_port,
    bool is_enabled,
    bool do_port_check,
    std::string bindaddr)
{
    if (is_enabled && handle->state == UpnpState::WillDiscover)
    {
        TR_ASSERT(!handle->discover_future);

        auto task = std::packaged_task<UPNPDev*(std::string)>{ discover_thread_func };
        handle->discover_future = task.get_future();
        handle->state = UpnpState::Discovering;

        std::thread(std::move(task), std::move(bindaddr)).detach();
    }

    if (is_enabled && handle->state == UpnpState::Discovering && handle->discover_future &&
        is_future_ready(*handle->discover_future))
    {
        auto* const devlist = handle->discover_future->get();
        handle->discover_future.reset();

        FreeUPNPUrls(&handle->urls);
        auto lanaddr = std::array<char, TR_ADDRSTRLEN>{};
        if (
#if (MINIUPNPC_API_VERSION >= 18)
            UPNP_GetValidIGD(devlist, &handle->urls, &handle->data, std::data(lanaddr), std::size(lanaddr) - 1, nullptr, 0)
#else
            UPNP_GetValidIGD(devlist, &handle->urls, &handle->data, std::data(lanaddr), std::size(lanaddr) - 1)
#endif
            == UPNP_IGD_VALID_CONNECTED)
        {
            tr_logAddInfo(fmt::format(
                fmt::runtime(_("Found Internet Gateway Device '{url}'")),
                fmt::arg("url", handle->urls.controlURL)));
            tr_logAddInfo(fmt::format(fmt::runtime(_("Local Address is '{address}'")), fmt::arg("address", lanaddr.data())));
            handle->state = UpnpState::Idle;
            handle->lanaddr = std::data(lanaddr);
        }
        else
        {
            handle->state = UpnpState::WillDiscover;
            tr_logAddDebug(fmt::format("UPNP_GetValidIGD failed: {} ({})", tr_strerror(errno), errno));
            tr_logAddDebug("If your router supports UPnP, please make sure UPnP is enabled!");
        }

        freeUPNPDevlist(devlist);
    }

    if (handle->state == UpnpState::Idle && handle->isMapped &&
        (!is_enabled || handle->advertised_port != advertised_port || handle->local_port != local_port))
    {
        handle->state = UpnpState::WillUnmap;
    }

    if (is_enabled && handle->isMapped && do_port_check &&
        (get_specific_port_mapping_entry(handle, "TCP") != UPNPCOMMAND_SUCCESS ||
         get_specific_port_mapping_entry(handle, "UDP") != UPNPCOMMAND_SUCCESS))
    {
        tr_logAddInfo(fmt::format(
            fmt::runtime(_("Local port {local_port} is not forwarded to {advertised_port}")),
            fmt::arg("local_port", handle->local_port.host()),
            fmt::arg("advertised_port", handle->advertised_port.host())));
        handle->isMapped = false;
    }

    if (handle->state == UpnpState::WillUnmap)
    {
        tr_upnpDeletePortMapping(handle, "TCP", handle->advertised_port);
        tr_upnpDeletePortMapping(handle, "UDP", handle->advertised_port);

        tr_logAddInfo(fmt::format(
            fmt::runtime(_("Stopping port forwarding through '{url}', service '{type}'")),
            fmt::arg("url", handle->urls.controlURL),
            fmt::arg("type", handle->data.first.servicetype)));

        handle->isMapped = false;
        handle->state = UpnpState::Idle;
        handle->advertised_port = {};
        handle->local_port = {};
    }

    if (handle->state == UpnpState::Idle && is_enabled && !handle->isMapped)
    {
        handle->state = UpnpState::WillMap;
    }

    if (handle->state == UpnpState::WillMap)
    {
        errno = 0;

        if (handle->urls.controlURL == nullptr)
        {
            handle->isMapped = false;
        }
        else
        {
            auto const desc = fmt::format("Transmission at {:d}", local_port.host());
            int const err_tcp = upnp_add_port_mapping(handle, "TCP", advertised_port, local_port, desc.c_str());
            int const err_udp = upnp_add_port_mapping(handle, "UDP", advertised_port, local_port, desc.c_str());

            handle->isMapped = err_tcp == 0 || err_udp == 0;
        }

        tr_logAddDebug(fmt::format(
            fmt::runtime(_("Port forwarding through '{url}', service '{type}'. (local address: {address}:{port})")),
            fmt::arg("url", handle->urls.controlURL),
            fmt::arg("type", handle->data.first.servicetype),
            fmt::arg("address", handle->lanaddr),
            fmt::arg("port", local_port.host())));

        if (handle->isMapped)
        {
            tr_logAddInfo(fmt::format(
                fmt::runtime(_("Forwarded local port {local_port} to {advertised_port}")),
                fmt::arg("local_port", local_port.host()),
                fmt::arg("advertised_port", advertised_port.host())));
            handle->advertised_port = advertised_port;
            handle->local_port = local_port;
        }
        else
        {
            tr_logAddInfo(_("If your router supports UPnP, please make sure UPnP is enabled!"));
            handle->advertised_port = {};
            handle->local_port = {};
        }
        handle->state = UpnpState::Idle;
    }

    return port_fwd_state(handle->state, handle->isMapped);
}
