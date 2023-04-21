// This file Copyright © 2012-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <QAbstractItemView>
#include <QStandardItemModel>
#include <QStyle>

#include "StyleHelper.h"
#include "Utils.h"
#include "FilterUIDelegate.h"
#include "FilterUI.h"

namespace
{

int getHSpacing(QWidget const* w)
{
    return qMax(3, w->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing, nullptr, w));
}

} // namespace

FilterUIDelegate::FilterUIDelegate(QObject* parent, QWidget* widget)
    : QItemDelegate(parent)
    , widget_(widget)
{
}

bool FilterUIDelegate::isSeparator(QModelIndex const& index)
{
    return index.data(Qt::AccessibleDescriptionRole).toString() == QStringLiteral("separator");
}

void FilterUIDelegate::setSeparator(QAbstractItemModel* model, QModelIndex const& index)
{
    model->setData(index, QStringLiteral("separator"), Qt::AccessibleDescriptionRole);

    if (auto const* const m = qobject_cast<QStandardItemModel*>(model))
    {
        if (QStandardItem* item = m->itemFromIndex(index))
        {
            item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        }
    }
}

void FilterUIDelegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    if (isSeparator(index))
    {
        QRect rect = option.rect;

        if (auto const* view = qobject_cast<QAbstractItemView const*>(option.widget))
        {
            rect.setWidth(view->viewport()->width());
        }

        QStyleOption opt;
        opt.rect = rect;
        widget_->style()->drawPrimitive(QStyle::PE_IndicatorToolBarSeparator, &opt, painter, widget_);
    }
    else
    {
        QStyleOptionViewItem disabled_option = option;
        QPalette::ColorRole const disabled_color_role = (disabled_option.state & QStyle::State_Selected) != 0 ?
            QPalette::HighlightedText :
            QPalette::Text;
        disabled_option.palette.setColor(
            disabled_color_role,
            Utils::getFadedColor(disabled_option.palette.color(disabled_color_role)));

        QRect bounding_box = option.rect;

        int const hmargin = getHSpacing(widget_);
        bounding_box.adjust(hmargin, 0, -hmargin, 0);

        QRect decoration_rect = rect(option, index, Qt::DecorationRole);
        decoration_rect.setSize(QSize(16, 16));
        decoration_rect = QStyle::alignedRect(
            option.direction,
            Qt::AlignLeft | Qt::AlignVCenter,
            decoration_rect.size(),
            bounding_box);
        Utils::narrowRect(bounding_box, decoration_rect.width() + hmargin, 0, option.direction);

        QRect count_rect = rect(option, index, FilterUI::CountStringRole);
        count_rect = QStyle::alignedRect(option.direction, Qt::AlignRight | Qt::AlignVCenter, count_rect.size(), bounding_box);
        Utils::narrowRect(bounding_box, 0, count_rect.width() + hmargin, option.direction);
        QRect const display_rect = bounding_box;

        QIcon const icon = Utils::getIconFromIndex(index);

        drawBackground(painter, option, index);
        icon.paint(painter, decoration_rect, Qt::AlignCenter, StyleHelper::getIconMode(option.state), QIcon::Off);
        drawDisplay(painter, option, display_rect, index.data(Qt::DisplayRole).toString());
        drawDisplay(painter, disabled_option, count_rect, index.data(FilterUI::CountStringRole).toString());
        drawFocus(painter, option, display_rect | count_rect);
    }
}

QSize FilterUIDelegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    if (isSeparator(index))
    {
        int const pm = widget_->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, widget_);
        return { pm, pm + 10 };
    }

    QStyle const* const s = widget_->style();
    int const hmargin = getHSpacing(widget_);

    QSize size = QItemDelegate::sizeHint(option, index);
    size.setHeight(qMax(size.height(), 22));
    size.rwidth() += s->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget_);
    size.rwidth() += rect(option, index, FilterUI::CountStringRole).width();
    size.rwidth() += hmargin * 4;
    return size;
}
