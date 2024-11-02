// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cstddef> // size_t
#include <ctime>
#include <utility>

#include <fmt/chrono.h>

#include "libtransmission/transmission.h"

#include "libtransmission/log.h"
#include "libtransmission/session-alt-speeds.h"
#include "libtransmission/variant.h"
#include "libtransmission/utils.h" // for _()

using namespace std::literals;

void tr_session_alt_speeds::load(Settings&& settings)
{
    settings_ = std::move(settings);
    update_scheduler();
    set_active(settings_.is_active, ChangeReason::LoadSettings, true);
}

void tr_session_alt_speeds::update_minutes()
{
    minutes_.reset();

    for (int day = 0; day < 7; ++day)
    {
        if ((static_cast<tr_sched_day>(settings_.use_on_these_weekdays) & (1 << day)) != 0)
        {
            auto const begin = settings_.minute_begin;
            auto const end = settings_.minute_end > settings_.minute_begin ? settings_.minute_end :
                                                                             settings_.minute_end + MinutesPerDay;
            for (auto i = begin; i < end; ++i)
            {
                minutes_.set((i + day * MinutesPerDay) % MinutesPerWeek);
            }
        }
    }
}

void tr_session_alt_speeds::update_scheduler()
{
    update_minutes();
    scheduler_set_is_active_to_.reset();
    check_scheduler();
}

void tr_session_alt_speeds::check_scheduler()
{
    if (!is_scheduler_enabled())
    {
        return;
    }

    if (auto const active = is_active_minute(mediator_.time());
        !scheduler_set_is_active_to_ || scheduler_set_is_active_to_ != active)
    {
        tr_logAddInfo(active ? _("Time to turn on turtle mode") : _("Time to turn off turtle mode"));
        scheduler_set_is_active_to_ = active;
        set_active(active, ChangeReason::Scheduler);
    }
}

void tr_session_alt_speeds::set_active(bool active, ChangeReason reason, bool force)
{
    if (auto& tgt = settings_.is_active; force || tgt != active)
    {
        tgt = active;
        mediator_.is_active_changed(tgt, reason);
    }
}

[[nodiscard]] bool tr_session_alt_speeds::is_active_minute(time_t time) const noexcept
{
    auto const tm = fmt::localtime(time);

    size_t minute_of_the_week = (tm.tm_wday * MinutesPerDay) + (tm.tm_hour * MinutesPerHour) + tm.tm_min;

    if (minute_of_the_week >= MinutesPerWeek) /* leap minutes? */
    {
        minute_of_the_week = MinutesPerWeek - 1;
    }

    return minutes_.test(minute_of_the_week);
}
