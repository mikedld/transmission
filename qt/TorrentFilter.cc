// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <optional>

#include "libtransmission/utils.h"

#include "Filters.h"
#include "Prefs.h"
#include "Torrent.h"
#include "TorrentFilter.h"
#include "TorrentModel.h"
#include "Utils.h"

TorrentFilter::TorrentFilter(Prefs const& prefs)
    : prefs_{ prefs }
{
    connect(&prefs_, &Prefs::changed, this, &TorrentFilter::onPrefChanged);
    connect(&refilter_timer_, &QTimer::timeout, this, &TorrentFilter::refilter);

    setDynamicSortFilter(true);

    refilter();
}

/***
****
***/

void TorrentFilter::onPrefChanged(int key)
{
    // For refiltering nearly immediately. Used to debounce batched prefs changes.
    static int const FastMSec = 50;

    // For waiting a little longer. Useful when user is typing the filter text.
    static int const SlowMSec = 500;

    std::optional<int> msec;
    switch (key)
    {
    case Prefs::FILTER_TEXT:
        // special case for isEmpty: user probably hit the 'clear' button
        msec = prefs_.getString(key).isEmpty() ? FastMSec : SlowMSec;
        break;

    case Prefs::FILTER_MODE:
    case Prefs::FILTER_TRACKERS:
    case Prefs::SORT_MODE:
    case Prefs::SORT_REVERSED:
        msec = FastMSec;
        break;

    default:
        break;
    }

    // if this pref change affects filtering, ensure that a refilter is queued
    if (msec && !refilter_timer_.isActive())
    {
        refilter_timer_.setSingleShot(true);
        refilter_timer_.start(*msec);
    }
}

void TorrentFilter::refilter()
{
    invalidate();
    sort(0, prefs_.getBool(Prefs::SORT_REVERSED) ? Qt::AscendingOrder : Qt::DescendingOrder);
}

/***
****
***/

namespace
{
int compareState(Torrent const* left, Torrent const* right)
{
    if (auto const val = tr_compare_3way(left->hasError(), right->hasError()); val != 0)
    {
        return val;
    }
    if (auto const val = tr_compare_3way(left->isFinished(), right->isFinished()); val != 0)
    {
        return val;
    }
    if (auto const val = -tr_compare_3way(left->isPaused(), right->isPaused()); val != 0)
    {
        return val;
    }
    return -tr_compare_3way(!left->hasMetadata(), !right->hasMetadata());
}
} // namespace

bool TorrentFilter::lessThan(QModelIndex const& left, QModelIndex const& right) const
{
    int val = 0;
    auto const* a = sourceModel()->data(left, TorrentModel::TorrentRole).value<Torrent const*>();
    auto const* b = sourceModel()->data(right, TorrentModel::TorrentRole).value<Torrent const*>();

    switch (prefs_.get<SortMode>(Prefs::SORT_MODE).mode())
    {
    case SortMode::SORT_BY_QUEUE:
        val = -tr_compare_3way(a->queuePosition(), b->queuePosition());

        break;

    case SortMode::SORT_BY_SIZE:
        val = tr_compare_3way(a->sizeWhenDone(), b->sizeWhenDone());

        break;

    case SortMode::SORT_BY_AGE:
        val = tr_compare_3way(a->dateAdded(), b->dateAdded());

        break;

    case SortMode::SORT_BY_ID:
        val = tr_compare_3way(a->id(), b->id());

        break;

    case SortMode::SORT_BY_ETA:
        val = a->compareETA(*b);

        [[fallthrough]];

    case SortMode::SORT_BY_ACTIVITY:
        if (val == 0)
        {
            val = tr_compare_3way(a->downloadSpeed() + a->uploadSpeed(), b->downloadSpeed() + b->uploadSpeed());
        }

        if (val == 0)
        {
            val = tr_compare_3way(
                a->peersWeAreUploadingTo() + a->peersWeAreDownloadingFrom() + a->webseedsWeAreDownloadingFrom(),
                b->peersWeAreUploadingTo() + b->peersWeAreDownloadingFrom() + b->webseedsWeAreDownloadingFrom());
        }

        [[fallthrough]];

    case SortMode::SORT_BY_STATE:
        if (val == 0)
        {
            val = compareState(a, b);
        }

        [[fallthrough]];

    case SortMode::SORT_BY_PROGRESS:
        if (val == 0)
        {
            val = tr_compare_3way(a->metadataPercentDone(), b->metadataPercentDone());
        }

        if (val == 0)
        {
            val = tr_compare_3way(a->percentComplete(), b->percentComplete());
        }

        if (val == 0)
        {
            val = a->compareSeedProgress(*b);
        }

        if (val == 0)
        {
            val = tr_compare_3way(a->getActivity(), b->getActivity());
        }

        if (val == 0)
        {
            val = -tr_compare_3way(a->queuePosition(), b->queuePosition());
        }

        break;

    case SortMode::SORT_BY_RATIO:
        val = -tr_compare_3way(!a->hasMetadata(), !b->hasMetadata());
        if (val == 0)
        {
            val = a->compareRatio(*b);
        }

        break;

    case SortMode::SORT_BY_NAME:
        // nothing to do: sorting by name is done after the switch
        break;

        // TODO(coeur): SORT_BY_TRACKER

    default:
        break;
    }

    if (val == 0)
    {
        val = -a->name().compare(b->name(), Qt::CaseInsensitive);
    }

    if (val == 0)
    {
        val = tr_compare_3way(a->hash(), b->hash());
    }

    return val < 0;
}

/***
****
***/

bool TorrentFilter::filterAcceptsRow(int source_row, QModelIndex const& source_parent) const
{
    QModelIndex const child_index = sourceModel()->index(source_row, 0, source_parent);
    auto const& tor = *child_index.model()->data(child_index, TorrentModel::TorrentRole).value<Torrent const*>();
    bool accepts = true;

    if (accepts)
    {
        auto const m = prefs_.get<FilterMode>(Prefs::FILTER_MODE);
        accepts = m.test(tor);
    }

    if (accepts)
    {
        auto const display_name = prefs_.getString(Prefs::FILTER_TRACKERS);
        accepts = display_name.isEmpty() || tor.includesTracker(display_name.toLower());
    }

    if (accepts)
    {
        auto const text = prefs_.getString(Prefs::FILTER_TEXT);
        accepts = text.isEmpty() || tor.name().contains(text, Qt::CaseInsensitive) ||
            tor.hash().toString().contains(text, Qt::CaseInsensitive);
    }

    return accepts;
}

std::array<int, FilterMode::NUM_MODES> TorrentFilter::countTorrentsPerMode() const
{
    auto* const torrent_model = dynamic_cast<TorrentModel*>(sourceModel());
    if (torrent_model == nullptr)
    {
        return {};
    }

    auto torrent_counts = std::array<int, FilterMode::NUM_MODES>{};

    for (auto const& tor : torrent_model->torrents())
    {
        for (int mode = 0; mode < FilterMode::NUM_MODES; ++mode)
        {
            if (FilterMode::test(*tor, mode))
            {
                ++torrent_counts[mode];
            }
        }
    }

    return torrent_counts;
}
