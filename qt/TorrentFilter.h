// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <array>

#include <QSortFilterProxyModel>
#include <QTimer>

#include "Filters.h"

class QString;

class FilterMode;
class Prefs;
class Torrent;

class TorrentFilter : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    enum TextMode
    {
        FILTER_BY_NAME,
        FILTER_BY_FILES,
        FILTER_BY_TRACKER
    };

    explicit TorrentFilter(Prefs const& prefs);
    TorrentFilter(TorrentFilter&&) = delete;
    TorrentFilter(TorrentFilter const&) = delete;
    TorrentFilter& operator=(TorrentFilter&&) = delete;
    TorrentFilter& operator=(TorrentFilter const&) = delete;

    [[nodiscard]] std::array<int, FilterMode::NUM_MODES> countTorrentsPerMode() const;

protected:
    // QSortFilterProxyModel
    [[nodiscard]] bool filterAcceptsRow(int, QModelIndex const&) const override;
    [[nodiscard]] bool lessThan(QModelIndex const&, QModelIndex const&) const override;

private slots:
    void onPrefChanged(int key);
    void refilter();

private:
    QTimer refilter_timer_;
    Prefs const& prefs_;
};
