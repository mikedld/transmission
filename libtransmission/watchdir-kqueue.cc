// This file Copyright © 2015-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno> // for errno
#include <string>
#include <unordered_set>

#include <fcntl.h> // for open()
#include <unistd.h> // for close()

#include <sys/types.h>
#include <sys/event.h>

#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif

#include <event2/event.h>

#include <fmt/core.h>

#define LIBTRANSMISSION_WATCHDIR_MODULE
#include "transmission.h"

#include "log.h"
#include "tr-assert.h"
#include "tr-strbuf.h"
#include "utils.h"
#include "watchdir-base.h"

class tr_watchdir_kqueue final : public tr_watchdir_base
{
public:
    tr_watchdir_kqueue(
        std::string_view dirname,
        Callback callback,
        libtransmission::TimerMaker& timer_maker,
        event_base* evbase)
        : tr_watchdir_base{ dirname, std::move(callback), timer_maker }
    {
        init(evbase);
        scan();
    }

    tr_watchdir_kqueue(tr_watchdir_kqueue&&) = delete;
    tr_watchdir_kqueue(tr_watchdir_kqueue const&) = delete;
    tr_watchdir_kqueue& operator=(tr_watchdir_kqueue&&) = delete;
    tr_watchdir_kqueue& operator=(tr_watchdir_kqueue const&) = delete;

    ~tr_watchdir_kqueue() override
    {
        event_del(event_);
        event_free(event_);

        if (kq_ != -1)
        {
            close(kq_);
        }

        if (dirfd_ != -1)
        {
            close(dirfd_);
        }
    }

private:
    void init(struct event_base* evbase)
    {
        kq_ = kqueue();
        if (kq_ == -1)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                _("Couldn't watch '{path}': {error} ({error_code})"),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        // open fd for watching
        auto const szdirname = tr_pathbuf{ dirname() };
        dirfd_ = open(szdirname, O_RDONLY | O_EVTONLY);
        if (dirfd_ == -1)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                _("Couldn't watch '{path}': {error} ({error_code})"),
                fmt::arg("path", dirname()),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        // register kevent filter with kqueue descriptor
        struct kevent ke;
        static auto constexpr KqueueWatchMask = (NOTE_WRITE | NOTE_EXTEND);
        EV_SET(&ke, dirfd_, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, KqueueWatchMask, 0, NULL);
        if (kevent(kq_, &ke, 1, nullptr, 0, nullptr) == -1)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                _("Couldn't watch '{path}': {error} ({error_code})"),
                fmt::arg("path", dirname()),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        // create libevent task for event descriptor
        event_ = event_new(evbase, kq_, EV_READ | EV_ET | EV_PERSIST, &onKqueueEvent, this);
        if (event_ == nullptr)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                _("Couldn't create event: {error} ({error_code})"),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        if (event_add(event_, nullptr) == -1)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                _("Couldn't add event: {error} ({error_code})"),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        // trigger one event for the initial scan
        // event_active(event_, EV_READ, 0);
    }

    static void onKqueueEvent(evutil_socket_t /*fd*/, short /*type*/, void* vself)
    {
        static_cast<tr_watchdir_kqueue*>(vself)->handleKqueueEvent();
    }

    void handleKqueueEvent()
    {
        struct kevent ke;
        auto ts = timespec{};
        if (kevent(kq_, nullptr, 0, &ke, 1, &ts) == -1)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                _("Couldn't read event: {error} ({error_code})"),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        scan();
    }

    int kq_ = -1;
    int dirfd_ = -1;
    struct event* event_ = nullptr;
};

std::unique_ptr<tr_watchdir> tr_watchdir::create(
    std::string_view dirname,
    Callback callback,
    libtransmission::TimerMaker& timer_maker,
    event_base* evbase)
{
    return std::make_unique<tr_watchdir_kqueue>(dirname, std::move(callback), timer_maker, evbase);
}
