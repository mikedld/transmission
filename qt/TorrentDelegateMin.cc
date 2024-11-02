// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <utility>

#include <QApplication>
#include <QBrush>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QModelIndex>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QStyleOptionProgressBar>

#include <libtransmission/utils.h>

#include "StyleHelper.h"
#include "Torrent.h"
#include "TorrentDelegateMin.h"
#include "TorrentModel.h"
#include "Utils.h"

enum
{
    GUI_PAD = 6,
    BAR_WIDTH = 50,
    BAR_HEIGHT = 16,
    LINE_SPACING = 4
};

/***
****
****   +---------+-----------------------------------------------+
****   |  Icon   |   Title      shortStatusString [Progressbar]  |
****   +-------- +-----------------------------------------------+
****
***/

namespace
{

class ItemLayout
{
public:
    QFont name_font;
    QFont status_font;

    QRect icon_rect;
    QRect emblem_rect;
    QRect name_rect;
    QRect status_rect;
    QRect bar_rect;

    ItemLayout(
        QString name_text,
        QString status_text,
        QIcon const& emblem_icon,
        QFont const& base_font,
        Qt::LayoutDirection direction,
        QPoint const& top_left,
        int width);

    [[nodiscard]] QSize size() const
    {
        return (icon_rect | name_rect | status_rect | bar_rect).size();
    }

    [[nodiscard]] QString nameText() const
    {
        return elidedText(name_font, name_text_, name_rect.width());
    }

    [[nodiscard]] QString statusText() const
    {
        return status_text_;
    }

private:
    QString name_text_;
    QString status_text_;

    [[nodiscard]] QString elidedText(QFont const& font, QString const& text, int width) const
    {
        return QFontMetrics{ font }.elidedText(text, Qt::ElideRight, width);
    }
};

ItemLayout::ItemLayout(
    QString name_text,
    QString status_text,
    QIcon const& emblem_icon,
    QFont const& base_font,
    Qt::LayoutDirection direction,
    QPoint const& top_left,
    int width)
    : name_font{ base_font }
    , status_font{ base_font }
    , name_text_{ std::move(name_text) }
    , status_text_{ std::move(status_text) }
{
    auto const* style = QApplication::style();
    int const icon_size = style->pixelMetric(QStyle::PM_SmallIconSize);

    auto const name_fm = QFontMetrics{ name_font };
    auto const name_size = name_fm.size(0, name_text_);

    status_font.setPointSize(static_cast<int>(status_font.pointSize() * 0.85));
    auto const status_fm = QFontMetrics{ status_font };
    QSize const status_size(status_fm.size(0, status_text_));

    QStyleOptionProgressBar bar_style;
    bar_style.rect = QRect{ 0, 0, BAR_WIDTH, BAR_HEIGHT };
    bar_style.maximum = 100;
    bar_style.progress = 100;
    bar_style.textVisible = true;
    QSize const bar_size(
        (bar_style.rect.width() * 2) - style->subElementRect(QStyle::SE_ProgressBarGroove, &bar_style).width(),
        bar_style.rect.height());

    QRect base_rect{ top_left,
                     QSize{ width, std::max({ icon_size, name_size.height(), status_size.height(), bar_size.height() }) } };

    icon_rect = QStyle::alignedRect(direction, Qt::AlignLeft | Qt::AlignVCenter, QSize{ icon_size, icon_size }, base_rect);
    emblem_rect = QStyle::alignedRect(
        direction,
        Qt::AlignRight | Qt::AlignBottom,
        emblem_icon.actualSize(icon_rect.size() / 2, QIcon::Normal, QIcon::On),
        icon_rect);
    bar_rect = QStyle::alignedRect(direction, Qt::AlignRight | Qt::AlignVCenter, bar_size, base_rect);
    Utils::narrowRect(base_rect, icon_rect.width() + GUI_PAD, bar_rect.width() + GUI_PAD, direction);
    status_rect = QStyle::alignedRect(
        direction,
        Qt::AlignRight | Qt::AlignVCenter,
        QSize{ status_size.width(), base_rect.height() },
        base_rect);
    Utils::narrowRect(base_rect, 0, status_rect.width() + GUI_PAD, direction);
    name_rect = base_rect;
}

} // namespace

QSize TorrentDelegateMin::sizeHint(QStyleOptionViewItem const& option, Torrent const& tor) const
{
    auto const m = margin(*QApplication::style());
    auto const layout = ItemLayout{ tor.name(),
                                    shortStatusString(tor),
                                    QIcon{},
                                    option.font,
                                    option.direction,
                                    QPoint{},
                                    option.rect.width() - (m.width() * 2) };
    return layout.size() + m * 2;
}

void TorrentDelegateMin::drawTorrent(QPainter* painter, QStyleOptionViewItem const& option, Torrent const& tor) const
{
    auto const* style = QApplication::style();

    bool const is_paused(tor.isPaused());

    bool const is_item_selected((option.state & QStyle::State_Selected) != 0);
    bool const is_item_enabled((option.state & QStyle::State_Enabled) != 0);
    bool const is_item_active((option.state & QStyle::State_Active) != 0);

    painter->save();

    if (is_item_selected)
    {
        auto color_group = is_item_enabled ? QPalette::Normal : QPalette::Disabled;

        if (color_group == QPalette::Normal && !is_item_active)
        {
            color_group = QPalette::Inactive;
        }

        painter->fillRect(option.rect, option.palette.brush(color_group, QPalette::Highlight));
    }

    auto icon_mode = QIcon::Mode{};

    if (is_paused || !is_item_enabled)
    {
        icon_mode = QIcon::Disabled;
    }
    else if (is_item_selected)
    {
        icon_mode = QIcon::Selected;
    }
    else
    {
        icon_mode = QIcon::Normal;
    }

    auto const icon_state = is_paused ? QIcon::Off : QIcon::On;
    auto const color_group = is_item_active ? QPalette::Normal : QPalette::Inactive;
    auto const color_role = is_item_selected ? QPalette::HighlightedText : QPalette::Text;

    auto text_color = (tor.hasError() && !is_item_selected) ? QColor{ Qt::GlobalColor::red } :
                                                              option.palette.color(color_group, color_role);
    if (is_paused || !is_item_enabled)
    {
        text_color.setAlphaF(0.5);
    }

    QStyle::State progress_bar_state(option.state);

    if (is_paused)
    {
        progress_bar_state = QStyle::State_None;
    }

    progress_bar_state |= QStyle::State_Small | QStyle::State_Horizontal;

    QIcon::Mode const emblem_im = is_item_selected ? QIcon::Selected : QIcon::Normal;
    QIcon const emblem_icon = tor.hasError() ? getWarningEmblem() : QIcon{};

    // layout
    QSize const m(margin(*style));
    QRect const content_rect(option.rect.adjusted(m.width(), m.height(), -m.width(), -m.height()));
    auto const layout = ItemLayout{ tor.name(), //
                                    shortStatusString(tor),
                                    emblem_icon,
                                    option.font,
                                    option.direction,
                                    content_rect.topLeft(),
                                    content_rect.width() };

    // render
    painter->setPen(text_color);

    tor.getMimeTypeIcon().paint(painter, layout.icon_rect, Qt::AlignCenter, icon_mode, icon_state);

    if (!emblem_icon.isNull())
    {
        emblem_icon.paint(painter, layout.emblem_rect, Qt::AlignCenter, emblem_im, icon_state);
    }

    painter->setFont(layout.name_font);
    painter->drawText(layout.name_rect, Qt::AlignLeft | Qt::AlignVCenter, layout.nameText());
    painter->setFont(layout.status_font);
    painter->drawText(layout.status_rect, Qt::AlignLeft | Qt::AlignVCenter, layout.statusText());
    progress_bar_style_.rect = layout.bar_rect;

    if (tor.isDownloading())
    {
        progress_bar_style_.palette.setBrush(QPalette::Highlight, BlueBrush);
        progress_bar_style_.palette.setColor(QPalette::Base, BlueBack);
        progress_bar_style_.palette.setColor(QPalette::Window, BlueBack);
    }
    else if (tor.isSeeding())
    {
        progress_bar_style_.palette.setBrush(QPalette::Highlight, GreenBrush);
        progress_bar_style_.palette.setColor(QPalette::Base, GreenBack);
        progress_bar_style_.palette.setColor(QPalette::Window, GreenBack);
    }
    else
    {
        progress_bar_style_.palette.setBrush(QPalette::Highlight, SilverBrush);
        progress_bar_style_.palette.setColor(QPalette::Base, SilverBack);
        progress_bar_style_.palette.setColor(QPalette::Window, SilverBack);
    }

    progress_bar_style_.state = progress_bar_state;
    progress_bar_style_.text = QStringLiteral("%1%").arg(static_cast<int>(tr_truncd(100.0 * tor.percentDone(), 0)));
    progress_bar_style_.textVisible = true;
    progress_bar_style_.textAlignment = Qt::AlignCenter;
    setProgressBarPercentDone(option, tor);
    StyleHelper::drawProgressBar(*style, *painter, progress_bar_style_);

    painter->restore();
}
