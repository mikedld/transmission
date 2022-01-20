// This file Copyright © 2010-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QSortFilterProxyModel>

#include <libtransmission/tr-macros.h>

class TrackerModelFilter : public QSortFilterProxyModel
{
    Q_OBJECT
    TR_DISABLE_COPY_MOVE(TrackerModelFilter)

public:
    explicit TrackerModelFilter(QObject* parent = nullptr);

    void setShowBackupTrackers(bool);

    bool showBackupTrackers() const
    {
        return show_backups_;
    }

protected:
    // QSortFilterProxyModel
    virtual bool filterAcceptsRow(int source_row, QModelIndex const& source_parent) const override;

private:
    bool show_backups_ = {};
};
