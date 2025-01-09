// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>

#include "libtransmission/utils-ev.h"

namespace libtransmission::evhelpers
{

void BufferDeleter::operator()(struct evbuffer* buf) const noexcept
{
    if (buf != nullptr)
    {
        evbuffer_free(buf);
    }
}

void EventBaseDeleter::operator()(struct event_base* evbase) const noexcept
{
    if (evbase != nullptr)
    {
        event_base_free(evbase);
    }
}

void EventDeleter::operator()(struct event* event) const
{
    if (event != nullptr)
    {
        event_del(event);
        event_free(event);
    }
}

void EvhttpDeleter::operator()(struct evhttp* evh) const noexcept
{
    if (evh != nullptr)
    {
        evhttp_free(evh);
    }
}

// RPC events (evhttp) will default to pri1, one level higher than pri2 events
// created here. Depends on event_base having three priority levels
struct event* event_new_pri2(
    struct event_base* base,
    evutil_socket_t fd,
    short events,
    event_callback_fn callback,
    void* callback_arg)
{
    struct event* e = event_new(base, fd, events, callback, callback_arg);
    event_priority_set(e, 2);
    return e;
}

} // namespace libtransmission::evhelpers
