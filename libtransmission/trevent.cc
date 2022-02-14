// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <condition_variable>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include <csignal>

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <event2/dns.h>
#include <event2/event.h>
#include <event2/thread.h>

#include "transmission.h"

#include "log.h"
#include "net.h"
#include "session.h"
#include "tr-assert.h"
#include "trevent.h"
#include "utils.h"

/***
****
***/

#include <iostream>

namespace
{
namespace impl
{
void* lock_alloc(unsigned /*locktype*/)
{
    return new std::recursive_mutex{};
}

void lock_free(void* lock_, unsigned /*locktype*/)
{
    delete static_cast<std::recursive_mutex*>(lock_);
}

int lock_lock(unsigned mode, void* lock_)
{
    auto* lock = static_cast<std::recursive_mutex*>(lock_);
    if ((mode & EVTHREAD_TRY) != 0U)
    {
        auto const success = lock->try_lock();
        return success ? 0 : -1;
    }
    lock->lock();
    return 0;
}

int lock_unlock(unsigned /*mode*/, void* lock_)
{
    static_cast<std::recursive_mutex*>(lock_)->unlock();
    return 0;
}

void* cond_alloc(unsigned /*condflags*/)
{
    return new std::condition_variable_any();
}

void cond_free(void* cond_)
{
    delete static_cast<std::condition_variable_any*>(cond_);
}

int cond_signal(void* cond_, int broadcast)
{
    auto* cond = static_cast<std::condition_variable_any*>(cond_);
    if (broadcast != 0)
    {
        cond->notify_all();
    }
    else
    {
        cond->notify_one();
    }
    return 0;
}

int cond_wait(void* cond_, void* lock_, struct timeval const* tv)
{
    auto* cond = static_cast<std::condition_variable_any*>(cond_);
    auto* lock = static_cast<std::recursive_mutex*>(lock_);
    if (tv == nullptr)
    {
        cond->wait(*lock);
        return 0;
    }

    auto const duration = std::chrono::seconds(tv->tv_sec) + std::chrono::microseconds(tv->tv_usec);
    auto const success = cond->wait_for(*lock, duration);
    return success == std::cv_status::timeout ? 1 : 0;
}

unsigned long thread_current_id()
{
    return std::hash<std::thread::id>()(std::this_thread::get_id());
}

} // namespace impl

void tr_evthread_init()
{
    // evthread_enable_lock_debugging();

    evthread_lock_callbacks constexpr lock_cbs{ EVTHREAD_LOCK_API_VERSION, EVTHREAD_LOCKTYPE_RECURSIVE,
                                                impl::lock_alloc,          impl::lock_free,
                                                impl::lock_lock,           impl::lock_unlock };
    evthread_set_lock_callbacks(&lock_cbs);

    evthread_condition_callbacks constexpr cond_cbs{ EVTHREAD_CONDITION_API_VERSION,
                                                     impl::cond_alloc,
                                                     impl::cond_free,
                                                     impl::cond_signal,
                                                     impl::cond_wait };
    evthread_set_condition_callbacks(&cond_cbs);

    evthread_set_id_callback(impl::thread_current_id);
}

} // namespace

/***
****
***/

struct tr_event_handle
{
    // would it be more expensive to use std::function here?
    struct callback
    {
        callback(void (*func)(void*) = nullptr, void* user_data = nullptr)
            : func_{ func }
            , user_data_{ user_data }
        {
        }

        void invoke() const
        {
            if (func_ != nullptr)
            {
                func_(user_data_);
            }
        }

        void (*func_)(void*);
        void* user_data_;
    };

    using work_queue_t = std::list<callback>;
    work_queue_t work_queue;
    std::mutex work_queue_mutex;
    event* work_queue_event = nullptr;

    event_base* base = nullptr;
    tr_session* session = nullptr;
    std::thread::id thread_id;
};

static void onWorkAvailable(evutil_socket_t /*fd*/, short /*flags*/, void* vsession)
{
    // invariant
    auto* const session = static_cast<tr_session*>(vsession);
    TR_ASSERT(tr_amInEventThread(session));

    // steal the work queue
    auto* events = session->events;
    auto work_queue_lock = std::unique_lock(events->work_queue_mutex);
    auto work_queue = tr_event_handle::work_queue_t{};
    std::swap(work_queue, events->work_queue);
    work_queue_lock.unlock();

    // process the work queue
    for (auto const& work : work_queue)
    {
        work.invoke();
    }
}

static void libeventThreadFunc(tr_event_handle* events)
{
#ifndef _WIN32
    /* Don't exit when writing on a broken socket */
    signal(SIGPIPE, SIG_IGN);
#endif

    tr_evthread_init();

    // create the libevent base
    auto* const base = event_base_new();
    auto* const dns_base = evdns_base_new(base, EVDNS_BASE_INITIALIZE_NAMESERVERS);

    // initialize the session struct's event fields
    events->base = base;
    events->work_queue_event = event_new(base, -1, 0, onWorkAvailable, events->session);
    events->session->event_base = base;
    events->session->evdns_base = dns_base;
    events->session->events = events;

    // loop until `tr_eventClose()` kills the loop
    event_base_loop(base, EVLOOP_NO_EXIT_ON_EMPTY);

    // shut down the thread
    if (dns_base != nullptr)
    {
        evdns_base_free(dns_base, 0);
    }
    event_base_free(base);
    events->session->event_base = nullptr;
    events->session->evdns_base = nullptr;
    events->session->events = nullptr;
    delete events;
    tr_logAddDebug("Closing libevent thread");
}

void tr_eventInit(tr_session* session)
{
    session->events = nullptr;

    auto* const events = new tr_event_handle();
    events->session = session;

    auto thread = std::thread(libeventThreadFunc, events);
    events->thread_id = thread.get_id();
    thread.detach();

    // wait until the libevent thread is running
    while (session->events == nullptr)
    {
        tr_wait_msec(100);
    }
}

void tr_eventClose(tr_session* session)
{
    TR_ASSERT(tr_isSession(session));

    auto* events = session->events;
    if (events == nullptr)
    {
        return;
    }

    event_base_loopexit(events->base, nullptr);

    if (tr_logGetDeepEnabled())
    {
        tr_logAddDeep(__FILE__, __LINE__, nullptr, "closing trevent pipe");
    }
}

/**
***
**/

bool tr_amInEventThread(tr_session const* session)
{
    TR_ASSERT(tr_isSession(session));
    TR_ASSERT(session->events != nullptr);

    return std::this_thread::get_id() == session->events->thread_id;
}

/**
***
**/

void tr_runInEventThread(tr_session* session, void (*func)(void*), void* user_data)
{
    TR_ASSERT(tr_isSession(session));
    auto* events = session->events;
    TR_ASSERT(events != nullptr);

    if (tr_amInEventThread(session))
    {
        (*func)(user_data);
    }
    else
    {
        auto lock = std::unique_lock(events->work_queue_mutex);
        events->work_queue.emplace_back(func, user_data);
        lock.unlock();

        event_active(events->work_queue_event, 0, {});
    }
}
