// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#pragma once

#include <bitset>
#include <cstddef> // size_t
#include <ctime> // for time_t
#include <optional>

#include "libtransmission/transmission.h" // for TR_SCHED_ALL

#include "libtransmission/quark.h"
#include "libtransmission/settings.h"
#include "libtransmission/values.h"

struct tr_variant;

/** Manages alternate speed limits and a scheduler to auto-toggle them. */
class tr_session_alt_speeds
{
    using Speed = libtransmission::Values::Speed;

public:
    class Settings final : public libtransmission::Settings
    {
    public:
        Settings() = default;

        explicit Settings(tr_variant const& src)
        {
            load(src);
        }

        // NB: When adding a field here, you must also add it to
        // fields() if you want it to be in session-settings.json
        bool is_active = false;
        bool scheduler_enabled = false; // whether alt speeds toggle on and off on schedule
        size_t minute_begin = 540U; // minutes past midnight; 9AM
        size_t minute_end = 1020U; // minutes past midnight; 5PM
        size_t speed_down_kbyps = 50U;
        size_t speed_up_kbyps = 50U;
        size_t use_on_these_weekdays = TR_SCHED_ALL;

    private:
        [[nodiscard]] Fields fields() override
        {
            return {
                { TR_KEY_alt_speed_enabled, &is_active },
                { TR_KEY_alt_speed_up, &speed_up_kbyps },
                { TR_KEY_alt_speed_down, &speed_down_kbyps },
                { TR_KEY_alt_speed_time_enabled, &scheduler_enabled },
                { TR_KEY_alt_speed_time_day, &use_on_these_weekdays },
                { TR_KEY_alt_speed_time_begin, &minute_begin },
                { TR_KEY_alt_speed_time_end, &minute_end },
            };
        }
    };

    enum class ChangeReason : uint8_t
    {
        User,
        Scheduler,
        LoadSettings
    };

    class Mediator
    {
    public:
        virtual ~Mediator() noexcept = default;

        using ChangeReason = tr_session_alt_speeds::ChangeReason;
        virtual void is_active_changed(bool is_active, ChangeReason reason) = 0;

        [[nodiscard]] virtual time_t time() = 0;
    };

    explicit tr_session_alt_speeds(Mediator& mediator) noexcept
        : mediator_{ mediator }
    {
    }

    void load(Settings&& settings);

    [[nodiscard]] constexpr auto const& settings() const noexcept
    {
        return settings_;
    }

    [[nodiscard]] constexpr bool is_active() const noexcept
    {
        return settings().is_active;
    }

    void check_scheduler();

    void set_scheduler_enabled(bool enabled)
    {
        settings_.scheduler_enabled = enabled;
        update_scheduler();
    }

    // return true iff the scheduler will turn alt speeds on/off
    [[nodiscard]] constexpr auto is_scheduler_enabled() const noexcept
    {
        return settings().scheduler_enabled;
    }

    void set_start_minute(size_t minute)
    {
        settings_.minute_begin = minute;
        update_scheduler();
    }

    [[nodiscard]] constexpr auto start_minute() const noexcept
    {
        return settings().minute_begin;
    }

    void set_end_minute(size_t minute)
    {
        settings_.minute_end = minute;
        update_scheduler();
    }

    [[nodiscard]] constexpr auto end_minute() const noexcept
    {
        return settings().minute_end;
    }

    void set_weekdays(tr_sched_day days)
    {
        settings_.use_on_these_weekdays = days;
        update_scheduler();
    }

    [[nodiscard]] constexpr tr_sched_day weekdays() const noexcept
    {
        return static_cast<tr_sched_day>(settings().use_on_these_weekdays);
    }

    [[nodiscard]] auto speed_limit(tr_direction const dir) const noexcept
    {
        auto const kbyps = dir == TR_DOWN ? settings().speed_down_kbyps : settings().speed_up_kbyps;
        return Speed{ kbyps, Speed::Units::KByps };
    }

    constexpr void set_speed_limit(tr_direction dir, Speed const limit) noexcept
    {
        if (dir == TR_DOWN)
        {
            settings_.speed_down_kbyps = limit.count(Speed::Units::KByps);
        }
        else
        {
            settings_.speed_up_kbyps = limit.count(Speed::Units::KByps);
        }
    }

    void set_active(bool active, ChangeReason reason)
    {
        set_active(active, reason, false);
    }

private:
    Mediator& mediator_;

    Settings settings_;

    void update_scheduler();
    void update_minutes();
    void set_active(bool active, ChangeReason reason, bool force);

    // whether `time` hits in one of the `minutes_` that is true
    [[nodiscard]] bool is_active_minute(time_t time) const noexcept;

    static auto constexpr MinutesPerHour = int{ 60 };
    static auto constexpr MinutesPerDay = int{ MinutesPerHour * 24 };
    static auto constexpr MinutesPerWeek = int{ MinutesPerDay * 7 };

    // bitfield of all the minutes in a week.
    // Each bit's value indicates whether the scheduler wants
    // alt speeds on or off at that given minute.
    std::bitset<10080> minutes_{};

    // recent change that was made by the scheduler
    std::optional<bool> scheduler_set_is_active_to_;
};
