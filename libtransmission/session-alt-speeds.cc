// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <chrono>
#include <cstddef> // size_t
#include <utility>

#include <fmt/chrono.h>

#include "libtransmission/transmission.h"

#include "libtransmission/log.h"
#include "libtransmission/session-alt-speeds.h"
#include "libtransmission/variant.h"
#include "libtransmission/utils.h" // for _()

using namespace std::literals;

void tr_session_alt_speeds::load(tr_variant const& src)
{
    auto const* const src_map = src.get_if<tr_variant::Map>();
    if (src_map != nullptr)
    {
#define V(key, field, type, default_value, comment) \
    if (auto const iter = src_map->find(key); iter != std::end(*src_map)) \
    { \
        if (auto val = libtransmission::VariantConverter::load<decltype(field)>(iter->second); val) \
        { \
            this->field = *val; \
        } \
    }
        ALT_SPEEDS_FIELDS(V)
#undef V
    }

    update_scheduler();
}

tr_variant tr_session_alt_speeds::settings() const
{
    auto settings = tr_variant::Map{ 6U };
#define V(key, field, type, default_value, comment) \
    settings.try_emplace(key, libtransmission::VariantConverter::save<decltype(field)>(field));
    ALT_SPEEDS_FIELDS(V)
#undef V
    return tr_variant{ std::move(settings) };
}

tr_variant tr_session_alt_speeds::default_settings()
{
    auto settings = tr_variant::Map{ 6U };
#define V(key, field, type, default_value, comment) \
    settings.try_emplace(key, libtransmission::VariantConverter::save<decltype(field)>(static_cast<type>(default_value)));
    ALT_SPEEDS_FIELDS(V)
#undef V
    return tr_variant{ std::move(settings) };
}

// --- minutes

void tr_session_alt_speeds::update_minutes()
{
    minutes_.reset();

    for (int day = 0; day < 7; ++day)
    {
        if ((static_cast<tr_sched_day>(use_on_these_weekdays_) & (1 << day)) != 0)
        {
            auto const begin = minute_begin_;
            auto const end = minute_end_ > minute_begin_ ? minute_end_ : minute_end_ + MinutesPerDay;
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

void tr_session_alt_speeds::set_active(bool active, ChangeReason reason)
{
    if (is_active_ != active)
    {
        is_active_ = active;
        mediator_.is_active_changed(is_active_, reason);
    }
}

[[nodiscard]] bool tr_session_alt_speeds::is_active_minute(time_t time) const noexcept
{
    auto const tm = fmt::localtime(time);

    size_t minute_of_the_week = tm.tm_wday * MinutesPerDay + tm.tm_hour * MinutesPerHour + tm.tm_min;

    if (minute_of_the_week >= MinutesPerWeek) /* leap minutes? */
    {
        minute_of_the_week = MinutesPerWeek - 1;
    }

    return minutes_.test(minute_of_the_week);
}
