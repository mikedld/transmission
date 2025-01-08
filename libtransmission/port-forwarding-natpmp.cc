// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cerrno>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h> // AF_INET
#endif

#include <event2/util.h> /* evutil_inet_ntop() */

#include <fmt/core.h>

#define ENABLE_STRNATPMPERR
#include "natpmp.h"

#define LIBTRANSMISSION_PORT_FORWARDING_MODULE

#include "libtransmission/transmission.h"

#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/port-forwarding-natpmp.h"
#include "libtransmission/utils.h"

namespace
{
void log_val(char const* func, int ret)
{
    if (ret == NATPMP_TRYAGAIN)
    {
        return;
    }

    if (ret >= 0)
    {
        tr_logAddDebug(fmt::format("{} succeeded ({})", func, ret));
    }
    else
    {
        tr_logAddDebug(fmt::format(
            "{} failed. Natpmp returned {} ({}); errno is {} ({})",
            func,
            ret,
            strnatpmperr(ret),
            errno,
            tr_strerror(errno)));
    }
}
} // namespace

bool tr_natpmp::canSendCommand() const
{
    return tr_time() >= command_time_;
}

void tr_natpmp::setCommandTime()
{
    command_time_ = tr_time() + CommandWaitSecs;
}

tr_natpmp::PulseResult tr_natpmp::pulse(tr_port local_port, bool is_enabled)
{
    if (is_enabled && state_ == State::Discover)
    {
        int val = initnatpmp(&natpmp_, 0, 0);
        log_val("initnatpmp", val);
        val = sendpublicaddressrequest(&natpmp_);
        log_val("sendpublicaddressrequest", val);
        state_ = val < 0 ? State::Err : State::RecvPub;
        has_discovered_ = true;
        setCommandTime();
    }

    if (state_ == State::RecvPub && canSendCommand())
    {
        natpmpresp_t response;
        auto const val = readnatpmpresponseorretry(&natpmp_, &response);
        log_val("readnatpmpresponseorretry", val);

        if (val >= 0)
        {
            auto str = std::array<char, 128>{};
            evutil_inet_ntop(AF_INET, &response.pnu.publicaddress.addr, std::data(str), std::size(str));
            tr_logAddInfo(
                fmt::format(fmt::runtime(_("Found public address '{address}'")), fmt::arg("address", std::data(str))));
            state_ = State::Idle;
        }
        else if (val != NATPMP_TRYAGAIN)
        {
            state_ = State::Err;
        }
    }

    if ((state_ == State::Idle || state_ == State::Err) && is_mapped_ && (!is_enabled || local_port_ != local_port))
    {
        state_ = State::SendUnmap;
    }

    if (state_ == State::SendUnmap && canSendCommand())
    {
        auto const val = sendnewportmappingrequest(
            &natpmp_,
            NATPMP_PROTOCOL_TCP,
            local_port_.host(),
            advertised_port_.host(),
            0);
        log_val("sendnewportmappingrequest", val);
        state_ = val < 0 ? State::Err : State::RecvUnmap;
        setCommandTime();
    }

    if (state_ == State::RecvUnmap)
    {
        auto resp = natpmpresp_t{};
        auto const val = readnatpmpresponseorretry(&natpmp_, &resp);
        log_val("readnatpmpresponseorretry", val);

        if (val >= 0)
        {
            auto const unmapped_port = tr_port::from_host(resp.pnu.newportmapping.privateport);

            tr_logAddInfo(
                fmt::format(fmt::runtime(_("Port {port} is no longer forwarded")), fmt::arg("port", unmapped_port.host())));

            if (local_port_ == unmapped_port)
            {
                local_port_.clear();
                advertised_port_.clear();
                state_ = State::Idle;
                is_mapped_ = false;
            }
        }
        else if (val != NATPMP_TRYAGAIN)
        {
            state_ = State::Err;
        }
    }

    if ((state_ == State::Idle || state_ == State::Err) &&
        (is_mapped_ ? tr_time() >= renew_time_ : is_enabled && has_discovered_))
    {
        state_ = State::SendMap;
    }

    if (state_ == State::SendMap && canSendCommand())
    {
        auto const val = sendnewportmappingrequest(
            &natpmp_,
            NATPMP_PROTOCOL_TCP,
            local_port.host(),
            local_port.host(),
            LifetimeSecs);
        log_val("sendnewportmappingrequest", val);
        state_ = val < 0 ? State::Err : State::RecvMap;
        setCommandTime();
    }

    if (state_ == State::RecvMap)
    {
        auto resp = natpmpresp_t{};
        auto const val = readnatpmpresponseorretry(&natpmp_, &resp);
        log_val("readnatpmpresponseorretry", val);

        if (val >= 0)
        {
            state_ = State::Idle;
            is_mapped_ = true;
            renew_time_ = tr_time() + (resp.pnu.newportmapping.lifetime / 2);
            local_port_ = tr_port::from_host(resp.pnu.newportmapping.privateport);
            advertised_port_ = tr_port::from_host(resp.pnu.newportmapping.mappedpublicport);
            tr_logAddInfo(
                fmt::format(fmt::runtime(_("Port {port} forwarded successfully")), fmt::arg("port", local_port_.host())));
        }
        else if (val != NATPMP_TRYAGAIN)
        {
            state_ = State::Err;
        }
    }

    switch (state_)
    {
    case State::Idle:
        return { is_mapped_ ? TR_PORT_MAPPED : TR_PORT_UNMAPPED, local_port_, advertised_port_ };

    case State::Discover:
        return { TR_PORT_UNMAPPED, {}, {} };

    case State::RecvPub:
    case State::SendMap:
    case State::RecvMap:
        return { TR_PORT_MAPPING, {}, {} };

    case State::SendUnmap:
    case State::RecvUnmap:
        return { TR_PORT_UNMAPPING, {}, {} };

    default:
        return { TR_PORT_ERROR, {}, {} };
    }
}
