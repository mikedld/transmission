// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstdint> // uint64_t

#include "Torrent.h"

class FilterMode
{
public:
    enum
    {
        SHOW_ALL,
        SHOW_ACTIVE,
        SHOW_DOWNLOADING,
        SHOW_SEEDING,
        SHOW_PAUSED,
        SHOW_FINISHED,
        SHOW_VERIFYING,
        SHOW_ERROR,
        NUM_MODES
    };

    explicit FilterMode(int mode = SHOW_ALL)
        : mode_{ mode }
    {
    }

    [[nodiscard]] int mode() const
    {
        return mode_;
    }

    /* The Torrent properties that can affect this filter.
       When one of these changes, it's time to refilter. */
    static constexpr auto TorrentFields = Torrent::fields_t{
        (uint64_t{ 1 } << Torrent::TORRENT_ERROR) | //
        (uint64_t{ 1 } << Torrent::IS_FINISHED) | //
        (uint64_t{ 1 } << Torrent::PEERS_GETTING_FROM_US) | //
        (uint64_t{ 1 } << Torrent::PEERS_SENDING_TO_US) | //
        (uint64_t{ 1 } << Torrent::STATUS) //
    };

    static bool test(Torrent const& tor, int mode);

    [[nodiscard]] bool test(Torrent const& tor) const
    {
        return test(tor, mode());
    }

private:
    int mode_;
};

Q_DECLARE_METATYPE(FilterMode)

class SortMode
{
public:
    enum
    {
        SORT_BY_ACTIVITY,
        SORT_BY_AGE,
        SORT_BY_ETA,
        SORT_BY_NAME,
        SORT_BY_PROGRESS,
        SORT_BY_QUEUE,
        SORT_BY_RATIO,
        SORT_BY_SIZE,
        SORT_BY_STATE,
        SORT_BY_ID,
        NUM_MODES
    };

    explicit SortMode(int mode = SORT_BY_ID) noexcept
        : mode_{ mode }
    {
    }

    [[nodiscard]] constexpr auto mode() const noexcept
    {
        return mode_;
    }

private:
    int mode_ = SORT_BY_ID;
};

Q_DECLARE_METATYPE(SortMode)
