// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <functional>
#include <memory>
#include <utility>

struct event_base;

class tr_session_thread
{
public:
    static void tr_evthread_init();

    static std::unique_ptr<tr_session_thread> create();
    virtual ~tr_session_thread() = default;

    [[nodiscard]] virtual struct event_base* event_base() noexcept = 0;

    [[nodiscard]] virtual bool am_in_session_thread() const noexcept = 0;

    virtual void queue(std::function<void(void)>&& func) = 0;

    virtual void run(std::function<void(void)>&& func) = 0;

    template<typename Func, typename... Args>
    void queue(Func&& func, Args&&... args)
    {
        queue(std::function<void(void)>{ std::bind(std::forward<Func>(func), std::forward<Args>(args)...) });
    }

    template<typename Func, typename... Args>
    void run(Func&& func, Args&&... args)
    {
        run(std::function<void(void)>{ std::bind(std::forward<Func>(func), std::forward<Args>(args)...) });
    }
};
