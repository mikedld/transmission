// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QItemDelegate>

class QAbstractItemModel;
class QComboBox;

class FilterBarComboBoxDelegate : public QItemDelegate
{
    Q_OBJECT

public:
    FilterBarComboBoxDelegate(QObject* parent, QComboBox* combo);
    FilterBarComboBoxDelegate(FilterBarComboBoxDelegate&&) = delete;
    FilterBarComboBoxDelegate(FilterBarComboBoxDelegate const&) = delete;
    FilterBarComboBoxDelegate& operator=(FilterBarComboBoxDelegate&&) = delete;
    FilterBarComboBoxDelegate& operator=(FilterBarComboBoxDelegate const&) = delete;

    static bool isSeparator(QModelIndex const& index);
    static void setSeparator(QAbstractItemModel* model, QModelIndex const& index);

protected:
    // QAbstractItemDelegate
    void paint(QPainter*, QStyleOptionViewItem const&, QModelIndex const&) const override;
    [[nodiscard]] QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const&) const override;

private:
    QComboBox* const combo_ = {};
};
