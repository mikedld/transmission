// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno> // for errno
#include <memory>
#include <utility>

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
#include "libtransmission/transmission.h"

#include "libtransmission/log.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h" // for _()
#include "libtransmission/utils-ev.h"
#include "libtransmission/watchdir-base.h"

namespace libtransmission
{
namespace
{
class KQueueWatchdir final : public impl::BaseWatchdir
{
public:
    KQueueWatchdir(std::string_view dirname, Callback callback, libtransmission::TimerMaker& timer_maker, event_base* evbase)
        : BaseWatchdir{ dirname, std::move(callback), timer_maker }
    {
        init(evbase);
        scan();
    }

    KQueueWatchdir(KQueueWatchdir&&) = delete;
    KQueueWatchdir(KQueueWatchdir const&) = delete;
    KQueueWatchdir& operator=(KQueueWatchdir&&) = delete;
    KQueueWatchdir& operator=(KQueueWatchdir const&) = delete;

    ~KQueueWatchdir() override
    {
        event_.reset();

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
                fmt::runtime(_("Couldn't watch '{path}': {error} ({error_code})")),
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
                fmt::runtime(_("Couldn't watch '{path}': {error} ({error_code})")),
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
                fmt::runtime(_("Couldn't watch '{path}': {error} ({error_code})")),
                fmt::arg("path", dirname()),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        // create libevent task for event descriptor
        event_.reset(event_new(evbase, kq_, EV_READ | EV_ET | EV_PERSIST, &onKqueueEvent, this));
        if (!event_)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                fmt::runtime(_("Couldn't create event: {error} ({error_code})")),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        if (event_add(event_.get(), nullptr) == -1)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                fmt::runtime(_("Couldn't add event: {error} ({error_code})")),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }
    }

    static void onKqueueEvent(evutil_socket_t /*fd*/, short /*type*/, void* vself)
    {
        static_cast<KQueueWatchdir*>(vself)->handleKqueueEvent();
    }

    void handleKqueueEvent()
    {
        struct kevent ke;
        auto ts = timespec{};
        if (kevent(kq_, nullptr, 0, &ke, 1, &ts) == -1)
        {
            auto const error_code = errno;
            tr_logAddError(fmt::format(
                fmt::runtime(_("Couldn't read event: {error} ({error_code})")),
                fmt::arg("error", tr_strerror(error_code)),
                fmt::arg("error_code", error_code)));
            return;
        }

        scan();
    }

    int kq_ = -1;
    int dirfd_ = -1;
    libtransmission::evhelpers::event_unique_ptr event_;
};

} // namespace

std::unique_ptr<Watchdir> Watchdir::create(
    std::string_view dirname,
    Callback callback,
    TimerMaker& timer_maker,
    event_base* evbase)
{
    return std::make_unique<KQueueWatchdir>(dirname, std::move(callback), timer_maker, evbase);
}

} // namespace libtransmission
