// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QSortFilterProxyModel>

class TrackerModelFilter : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit TrackerModelFilter(QObject* parent = nullptr);
    TrackerModelFilter(TrackerModelFilter&&) = delete;
    TrackerModelFilter(TrackerModelFilter const&) = delete;
    TrackerModelFilter& operator=(TrackerModelFilter&&) = delete;
    TrackerModelFilter& operator=(TrackerModelFilter const&) = delete;

    void setShowBackupTrackers(bool);

    [[nodiscard]] constexpr auto showBackupTrackers() const noexcept
    {
        return show_backups_;
    }

protected:
    // QSortFilterProxyModel
    [[nodiscard]] bool filterAcceptsRow(int source_row, QModelIndex const& source_parent) const override;

private:
    bool show_backups_ = {};
};
