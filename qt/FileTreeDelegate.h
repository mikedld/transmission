// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QItemDelegate>

class FileTreeDelegate : public QItemDelegate
{
    Q_OBJECT

public:
    explicit FileTreeDelegate(QObject* parent = nullptr)
        : QItemDelegate{ parent }
    {
    }
    FileTreeDelegate(FileTreeDelegate&&) = delete;
    FileTreeDelegate(FileTreeDelegate const&) = delete;
    FileTreeDelegate& operator=(FileTreeDelegate&&) = delete;
    FileTreeDelegate& operator=(FileTreeDelegate const&) = delete;

    // QAbstractItemDelegate
    [[nodiscard]] QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const&) const override;
    void paint(QPainter*, QStyleOptionViewItem const&, QModelIndex const&) const override;
};
