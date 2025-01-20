// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <vector>

#include <QAbstractListModel>

#include "Torrent.h"
#include "Typedefs.h"

class TorrentModel;

struct TrackerInfo
{
    TrackerStat st;
    int torrent_id = {};
};

Q_DECLARE_METATYPE(TrackerInfo)

class TrackerModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        TrackerRole = Qt::UserRole
    };

    TrackerModel() = default;
    TrackerModel(TrackerModel&&) = delete;
    TrackerModel(TrackerModel const&) = delete;
    TrackerModel& operator=(TrackerModel&&) = delete;
    TrackerModel& operator=(TrackerModel const&) = delete;

    void refresh(TorrentModel const&, torrent_ids_t const& ids);
    [[nodiscard]] int find(int torrent_id, QString const& url) const;

    // QAbstractItemModel
    [[nodiscard]] int rowCount(QModelIndex const& parent = QModelIndex{}) const override;
    [[nodiscard]] QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override;

private:
    using rows_t = std::vector<TrackerInfo>;

    rows_t rows_;
};
