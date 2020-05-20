/*
 * This file Copyright (C) 2010-2015 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include "TrackerModel.h"
#include "TrackerModelFilter.h"

TrackerModelFilter::TrackerModelFilter(QObject* parent) :
    QSortFilterProxyModel(parent),
    show_backups_(false)
{
}

void TrackerModelFilter::setShowBackupTrackers(bool b)
{
    show_backups_ = b;
    invalidateFilter();
}

bool TrackerModelFilter::filterAcceptsRow(int sourceRow, QModelIndex const& sourceParent) const
{
    QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    auto const trackerInfo = index.data(TrackerModel::TrackerRole).value<TrackerInfo>();
    return show_backups_ || !trackerInfo.st.isBackup;
}
