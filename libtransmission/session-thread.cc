// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <utility> // for std::move(), std::swap()

#include <csignal>

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <event2/event.h>
#include <event2/thread.h>

#include "libtransmission/session-thread.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils-ev.h"

using namespace std::literals;

// ---

namespace
{
namespace tr_evthread_init_helpers
{
void* lock_alloc(unsigned /*locktype*/)
{
    return new std::recursive_mutex{};
}

void lock_free(void* vlock, unsigned /*locktype*/)
{
    delete static_cast<std::recursive_mutex*>(vlock);
}

int lock_lock(unsigned mode, void* vlock)
{
    auto* lock = static_cast<std::recursive_mutex*>(vlock);
    if ((mode & EVTHREAD_TRY) != 0U)
    {
        auto const success = lock->try_lock();
        return success ? 0 : -1;
    }
    lock->lock();
    return 0;
}

int lock_unlock(unsigned /*mode*/, void* vlock)
{
    static_cast<std::recursive_mutex*>(vlock)->unlock();
    return 0;
}

void* cond_alloc(unsigned /*condflags*/)
{
    return new std::condition_variable_any();
}

void cond_free(void* vcond)
{
    delete static_cast<std::condition_variable_any*>(vcond);
}

int cond_signal(void* vcond, int broadcast)
{
    auto* cond = static_cast<std::condition_variable_any*>(vcond);
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

int cond_wait(void* vcond, void* vlock, struct timeval const* tv)
{
    auto* cond = static_cast<std::condition_variable_any*>(vcond);
    auto* lock = static_cast<std::recursive_mutex*>(vlock);
    if (tv == nullptr)
    {
        cond->wait(*lock);
        return 0;
    }

    auto const duration = std::chrono::seconds(tv->tv_sec) + std::chrono::microseconds(tv->tv_usec);
    auto const success = cond->wait_for(*lock, duration);
    return success == std::cv_status::timeout ? 1 : 0;
}

#if defined(_WIN32) && defined(__clang_analyzer__)
// See https://github.com/llvm/llvm-project/issues/98823
#define WORKAROUND_CLANG_TIDY_GH98823
#endif

unsigned long thread_current_id()
{
#ifndef WORKAROUND_CLANG_TIDY_GH98823
    thread_local auto const hashed = std::hash<std::thread::id>()(std::this_thread::get_id());
    return hashed;
#else
    return 0;
#endif
}

void init_evthreads_once()
{
    evthread_lock_callbacks constexpr LockCbs{
        EVTHREAD_LOCK_API_VERSION, EVTHREAD_LOCKTYPE_RECURSIVE, lock_alloc, lock_free, lock_lock, lock_unlock
    };
    evthread_set_lock_callbacks(&LockCbs);

    evthread_condition_callbacks constexpr CondCbs{ EVTHREAD_CONDITION_API_VERSION,
                                                    cond_alloc,
                                                    cond_free,
                                                    cond_signal,
                                                    cond_wait };
    evthread_set_condition_callbacks(&CondCbs);

    evthread_set_id_callback(thread_current_id);
}

} // namespace tr_evthread_init_helpers

auto make_event_base()
{
    tr_session_thread::tr_evthread_init();

    return libtransmission::evhelpers::evbase_unique_ptr{ event_base_new() };
}

} // namespace

// ---

void tr_session_thread::tr_evthread_init()
{
    using namespace tr_evthread_init_helpers;

    static auto evthread_flag = std::once_flag{};
    std::call_once(evthread_flag, init_evthreads_once);
}

class tr_session_thread_impl final : public tr_session_thread
{
public:
    explicit tr_session_thread_impl()
    {
        auto lock = std::unique_lock(is_looping_mutex_);

        thread_ = std::thread(&tr_session_thread_impl::session_thread_func, this, event_base());
        thread_id_ = thread_.get_id();

        // wait for the session thread's main loop to start
        is_looping_cv_.wait(lock, [this]() { return is_looping_.load(); });
    }

    tr_session_thread_impl(tr_session_thread_impl&&) = delete;
    tr_session_thread_impl(tr_session_thread_impl const&) = delete;
    tr_session_thread_impl& operator=(tr_session_thread_impl&&) = delete;
    tr_session_thread_impl& operator=(tr_session_thread_impl const&) = delete;

    ~tr_session_thread_impl() override
    {
        TR_ASSERT(!am_in_session_thread());
        TR_ASSERT(is_looping_);

        // Stop the first event loop. This is the steady-state loop that runs
        // continuously, even when there are no events. See: session_thread_func()
        is_shutting_down_ = true;
        event_base_loopexit(event_base(), nullptr);

        // Wait on the second event loop. This is the shutdown loop that exits
        // as soon as there are no events. This step is to give pending tasks
        // a chance to finish.
        auto lock = std::unique_lock(is_looping_mutex_);
        is_looping_cv_.wait_for(lock, Deadline, [this]() { return !is_looping_; });
        event_base_loopexit(event_base(), nullptr);
        thread_.join();
    }

    [[nodiscard]] struct event_base* event_base() noexcept override
    {
        return evbase_.get();
    }

    [[nodiscard]] bool am_in_session_thread() const noexcept override
    {
        return thread_id_ == std::this_thread::get_id();
    }

    void queue(std::function<void(void)>&& func) override
    {
        work_queue_mutex_.lock();
        work_queue_.emplace_back(std::move(func));
        work_queue_mutex_.unlock();

        event_active(work_queue_event_.get(), 0, {});
    }

    void run(std::function<void(void)>&& func) override
    {
        if (am_in_session_thread())
        {
            func();
        }
        else
        {
            queue(std::move(func));
        }
    }

private:
    using callback = std::function<void(void)>;
    using work_queue_t = std::list<callback>;

    void session_thread_func(struct event_base* evbase)
    {
#ifndef _WIN32
        /* Don't exit when writing on a broken socket */
        (void)signal(SIGPIPE, SIG_IGN);
#endif
        tr_evthread_init();

        constexpr auto ToggleLooping = [](evutil_socket_t, short /*evtype*/, void* vself)
        {
            auto* const self = static_cast<tr_session_thread_impl*>(vself);
            self->is_looping_mutex_.lock();
            self->is_looping_ = !self->is_looping_;
            self->is_looping_mutex_.unlock();

            self->is_looping_cv_.notify_one();
        };

        event_base_once(evbase, -1, EV_TIMEOUT, ToggleLooping, this, nullptr);

        // Start the first event loop. This is the steady-state loop that runs
        // continuously until `this` is destroyed. See: ~tr_session_thread_impl()
        TR_ASSERT(!is_shutting_down_);
        event_base_loop(evbase, EVLOOP_NO_EXIT_ON_EMPTY);

        // Start the second event loop. This is the shutdown loop that exits as
        // soon as there are no events. It's used to give any remaining events
        // a chance to finish up before we exit.
        TR_ASSERT(is_shutting_down_);
        event_base_loop(evbase, 0);

        ToggleLooping({}, {}, this);
    }

    static void on_work_available_static(evutil_socket_t /*fd*/, short /*flags*/, void* vself)
    {
        static_cast<tr_session_thread_impl*>(vself)->on_work_available();
    }
    void on_work_available()
    {
        TR_ASSERT(am_in_session_thread());

        // steal the work queue
        auto work_queue_lock = std::unique_lock(work_queue_mutex_);
        auto work_queue = work_queue_t{};
        std::swap(work_queue, work_queue_);
        work_queue_lock.unlock();

        // process the work queue
        for (auto const& func : work_queue)
        {
            func();
        }
    }

    libtransmission::evhelpers::evbase_unique_ptr const evbase_{ make_event_base() };
    libtransmission::evhelpers::event_unique_ptr const work_queue_event_{
        event_new(evbase_.get(), -1, 0, on_work_available_static, this)
    };

    work_queue_t work_queue_;
    std::mutex work_queue_mutex_;

    std::thread thread_;
    std::thread::id thread_id_;

    std::mutex is_looping_mutex_;
    std::condition_variable is_looping_cv_;
    std::atomic<bool> is_looping_ = false;

    std::atomic<bool> is_shutting_down_ = false;
    static constexpr std::chrono::seconds Deadline = 5s;
};

std::unique_ptr<tr_session_thread> tr_session_thread::create()
{
    return std::make_unique<tr_session_thread_impl>();
}
