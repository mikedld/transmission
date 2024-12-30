// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QItemDelegate>

class QStyle;

class Session;
struct TrackerInfo;

class TrackerDelegate : public QItemDelegate
{
    Q_OBJECT

public:
    explicit TrackerDelegate(QObject* parent = nullptr)
        : QItemDelegate{ parent }
    {
    }
    TrackerDelegate(TrackerDelegate&&) = delete;
    TrackerDelegate(TrackerDelegate const&) = delete;
    TrackerDelegate& operator=(TrackerDelegate&&) = delete;
    TrackerDelegate& operator=(TrackerDelegate const&) = delete;

    void setShowMore(bool b);

    // QAbstractItemDelegate
    [[nodiscard]] QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;

protected:
    [[nodiscard]] QString getText(TrackerInfo const&) const;

    [[nodiscard]] QSize sizeHint(QStyleOptionViewItem const&, TrackerInfo const&) const;
    void drawTracker(QPainter*, QStyleOptionViewItem const&, TrackerInfo const&) const;

private:
    bool show_more_ = false;
};
