// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <utility>

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QModelIndex>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QStyleOptionProgressBar>

#include "Formatter.h"
#include "IconCache.h"
#include "StyleHelper.h"
#include "Torrent.h"
#include "TorrentDelegate.h"
#include "TorrentModel.h"
#include "Utils.h"

enum
{
    GUI_PAD = 6,
    BAR_HEIGHT = 12
};

namespace
{

class ItemLayout
{
private:
    QString name_text_;
    QString status_text_;
    QString progress_text_;

public:
    QFont name_font;
    QFont status_font;
    QFont progress_font;

    QRect icon_rect;
    QRect emblem_rect;
    QRect name_rect;
    QRect status_rect;
    QRect bar_rect;
    QRect progress_rect;

    ItemLayout(
        QString name_text,
        QString status_text,
        QString progress_text,
        QIcon const& emblem_icon,
        QFont const& base_font,
        Qt::LayoutDirection direction,
        QPoint const& top_left,
        int width);

    [[nodiscard]] QSize size() const
    {
        return (icon_rect | name_rect | status_rect | bar_rect | progress_rect).size();
    }

    [[nodiscard]] QString nameText() const
    {
        return elidedText(name_font, name_text_, name_rect.width());
    }

    [[nodiscard]] QString statusText() const
    {
        return elidedText(status_font, status_text_, status_rect.width());
    }

    [[nodiscard]] QString progressText() const
    {
        return elidedText(progress_font, progress_text_, progress_rect.width());
    }

private:
    [[nodiscard]] QString elidedText(QFont const& font, QString const& text, int width) const
    {
        return QFontMetrics{ font }.elidedText(text, Qt::ElideRight, width);
    }
};

ItemLayout::ItemLayout(
    QString name_text,
    QString status_text,
    QString progress_text,
    QIcon const& emblem_icon,
    QFont const& base_font,
    Qt::LayoutDirection direction,
    QPoint const& top_left,
    int width)
    : name_text_{ std::move(name_text) }
    , status_text_{ std::move(status_text) }
    , progress_text_{ std::move(progress_text) }
    , name_font{ base_font }
    , status_font{ base_font }
    , progress_font{ base_font }
{
    QStyle const* style = QApplication::style();
    int const icon_size(style->pixelMetric(QStyle::PM_LargeIconSize));

    name_font.setWeight(QFont::Bold);
    auto const name_fm = QFontMetrics{ name_font };
    auto const name_size = name_fm.size(0, name_text_);

    status_font.setPointSize(static_cast<int>(status_font.pointSize() * 0.9));
    auto const status_fm = QFontMetrics{ status_font };
    auto const status_size = status_fm.size(0, status_text_);

    progress_font.setPointSize(static_cast<int>(progress_font.pointSize() * 0.9));
    auto const progress_fm = QFontMetrics{ progress_font };
    auto const progress_size = progress_fm.size(0, progress_text_);

    auto base_rect = QRect{ top_left, QSize{ width, 0 } };
    Utils::narrowRect(base_rect, icon_size + GUI_PAD, 0, direction);

    name_rect = base_rect.adjusted(0, 0, 0, name_size.height());
    status_rect = name_rect.adjusted(0, name_rect.height() + 1, 0, status_size.height() + 1);
    bar_rect = status_rect.adjusted(0, status_rect.height() + 1, 0, BAR_HEIGHT + 1);
    progress_rect = bar_rect.adjusted(0, bar_rect.height() + 1, 0, progress_size.height() + 1);
    icon_rect = QStyle::alignedRect(
        direction,
        Qt::AlignLeft | Qt::AlignVCenter,
        QSize{ icon_size, icon_size },
        QRect{ top_left, QSize{ width, progress_rect.bottom() - name_rect.top() } });
    emblem_rect = QStyle::alignedRect(
        direction,
        Qt::AlignRight | Qt::AlignBottom,
        emblem_icon.actualSize(icon_rect.size() / 2, QIcon::Normal, QIcon::On),
        icon_rect);
}

} // namespace

TorrentDelegate::TorrentDelegate(QObject* parent)
    : QStyledItemDelegate{ parent }
{
    progress_bar_style_.minimum = 0;
    progress_bar_style_.maximum = 1000;
}

/***
****
***/

QSize TorrentDelegate::margin(QStyle const& style) const
{
    Q_UNUSED(style)

    return { 4, 4 };
}

QString TorrentDelegate::progressString(Torrent const& tor)
{
    bool const is_magnet(!tor.hasMetadata());
    bool const is_done(tor.isDone());
    bool const is_seed(tor.isSeed());
    uint64_t const have_total(tor.haveTotal());
    QString str;
    auto const seed_ratio_limit = tor.getSeedRatioLimit();

    if (is_magnet) // magnet link with no metadata
    {
        //: First part of torrent progress string,
        //: %1 is the percentage of torrent metadata downloaded
        str = tr("Magnetized transfer - retrieving metadata (%1%)")
                  .arg(Formatter::percent_to_string(tor.metadataPercentDone() * 100.0));
    }
    else if (!is_done) // downloading
    {
        //: First part of torrent progress string,
        //: %1 is how much we've got,
        //: %2 is how much we'll have when done,
        //: %3 is a percentage of the two
        str = tr("%1 of %2 (%3%)")
                  .arg(Formatter::storage_to_string(have_total))
                  .arg(Formatter::storage_to_string(tor.sizeWhenDone()))
                  .arg(Formatter::percent_to_string(tor.percentDone() * 100.0));
    }
    else if (!is_seed) // partial seed
    {
        if (seed_ratio_limit)
        {
            //: First part of torrent progress string,
            //: %1 is how much we've got,
            //: %2 is the torrent's total size,
            //: %3 is a percentage of the two,
            //: %4 is how much we've uploaded,
            //: %5 is our upload-to-download ratio,
            //: %6 is the ratio we want to reach before we stop uploading
            str = tr("%1 of %2 (%3%), uploaded %4 (Ratio: %5 Goal: %6)")
                      .arg(Formatter::storage_to_string(have_total))
                      .arg(Formatter::storage_to_string(tor.totalSize()))
                      .arg(Formatter::percent_to_string(tor.percentComplete() * 100.0))
                      .arg(Formatter::storage_to_string(tor.uploadedEver()))
                      .arg(Formatter::ratio_to_string(tor.ratio()))
                      .arg(Formatter::ratio_to_string(*seed_ratio_limit));
        }
        else
        {
            //: First part of torrent progress string,
            //: %1 is how much we've got,
            //: %2 is the torrent's total size,
            //: %3 is a percentage of the two,
            //: %4 is how much we've uploaded,
            //: %5 is our upload-to-download ratio
            str = tr("%1 of %2 (%3%), uploaded %4 (Ratio: %5)")
                      .arg(Formatter::storage_to_string(have_total))
                      .arg(Formatter::storage_to_string(tor.totalSize()))
                      .arg(Formatter::percent_to_string(tor.percentComplete() * 100.0))
                      .arg(Formatter::storage_to_string(tor.uploadedEver()))
                      .arg(Formatter::ratio_to_string(tor.ratio()));
        }
    }
    else // seeding
    {
        if (seed_ratio_limit)
        {
            //: First part of torrent progress string,
            //: %1 is the torrent's total size,
            //: %2 is how much we've uploaded,
            //: %3 is our upload-to-download ratio,
            //: %4 is the ratio we want to reach before we stop uploading
            str = tr("%1, uploaded %2 (Ratio: %3 Goal: %4)")
                      .arg(Formatter::storage_to_string(have_total))
                      .arg(Formatter::storage_to_string(tor.uploadedEver()))
                      .arg(Formatter::ratio_to_string(tor.ratio()))
                      .arg(Formatter::ratio_to_string(*seed_ratio_limit));
        }
        else // seeding w/o a ratio
        {
            //: First part of torrent progress string,
            //: %1 is the torrent's total size,
            //: %2 is how much we've uploaded,
            //: %3 is our upload-to-download ratio
            str = tr("%1, uploaded %2 (Ratio: %3)")
                      .arg(Formatter::storage_to_string(have_total))
                      .arg(Formatter::storage_to_string(tor.uploadedEver()))
                      .arg(Formatter::ratio_to_string(tor.ratio()));
        }
    }

    // add time when downloading
    if ((seed_ratio_limit && tor.isSeeding()) || tor.isDownloading())
    {
        if (tor.hasETA())
        {
            //: Second (optional) part of torrent progress string,
            //: %1 is duration,
            //: notice that leading space (before the dash) is included here
            str += tr(" - %1 left").arg(Formatter::time_to_string(tor.getETA()));
        }
        else
        {
            //: Second (optional) part of torrent progress string,
            //: notice that leading space (before the dash) is included here
            str += tr(" - Remaining time unknown");
        }
    }

    return str.trimmed();
}

QString TorrentDelegate::shortTransferString(Torrent const& tor)
{
    QString str;
    bool const have_meta(tor.hasMetadata());
    bool const have_down(have_meta && ((tor.webseedsWeAreDownloadingFrom() > 0) || (tor.peersWeAreDownloadingFrom() > 0)));
    bool const have_up(have_meta && tor.peersWeAreUploadingTo() > 0);

    if (have_down)
    {
        str = tor.downloadSpeed().to_download_qstring() + QStringLiteral("   ") + tor.uploadSpeed().to_upload_qstring();
    }
    else if (have_up)
    {
        str = tor.uploadSpeed().to_upload_qstring();
    }

    return str.trimmed();
}

QString TorrentDelegate::shortStatusString(Torrent const& tor)
{
    QString str;

    switch (tor.getActivity())
    {
    case TR_STATUS_CHECK:
        str = tr("Verifying local data (%1% tested)").arg(Formatter::percent_to_string(tor.getVerifyProgress() * 100.0));
        break;

    case TR_STATUS_DOWNLOAD:
    case TR_STATUS_SEED:
        str = shortTransferString(tor) + QStringLiteral("    ") + tr("Ratio: %1").arg(Formatter::ratio_to_string(tor.ratio()));
        break;

    default:
        str = tor.activityString();
        break;
    }

    // add time when downloading
    auto const seed_ratio_limit = tor.getSeedRatioLimit();
    if ((seed_ratio_limit && tor.isSeeding()) || tor.isDownloading())
    {
        if (tor.hasETA())
        {
            //: Second (optional) part of torrent progress string,
            //: %1 is duration,
            //: notice that leading space (before the dash) is included here
            str += QStringLiteral("    ");
            str += tr("%1 left").arg(Formatter::time_to_string(tor.getETA()));
        }
        else
        {
            //: Second (optional) part of torrent progress string,
            //: notice that leading space (before the dash) is included here
            str += QStringLiteral("    ");
            str += tr("Remaining time unknown");
        }
    }

    return str.trimmed();
}

QString TorrentDelegate::statusString(Torrent const& tor)
{
    QString str;

    if (tor.hasError())
    {
        str = tor.getError();
    }
    else
    {
        switch (tor.getActivity())
        {
        case TR_STATUS_STOPPED:
        case TR_STATUS_CHECK_WAIT:
        case TR_STATUS_CHECK:
        case TR_STATUS_DOWNLOAD_WAIT:
        case TR_STATUS_SEED_WAIT:
            str = shortStatusString(tor);
            break;

        case TR_STATUS_DOWNLOAD:
            if (!tor.hasMetadata())
            {
                str = tr("Downloading metadata from %Ln peer(s) (%1% done)", nullptr, tor.peersWeAreDownloadingFrom())
                          .arg(Formatter::percent_to_string(100.0 * tor.metadataPercentDone()));
            }
            else
            {
                /* it would be nicer for translation if this was all one string, but I don't see how to do multiple %n's in
                 * tr() */
                if (tor.connectedPeersAndWebseeds() == 0)
                {
                    //: First part of phrase "Downloading from ... peer(s) and ... web seed(s)"
                    str = tr("Downloading from %Ln peer(s)", nullptr, tor.peersWeAreDownloadingFrom());
                }
                else
                {
                    //: First part of phrase "Downloading from ... of ... connected peer(s) and ... web seed(s)"
                    str = tr("Downloading from %1 of %Ln connected peer(s)", nullptr, tor.connectedPeersAndWebseeds())
                              .arg(tor.peersWeAreDownloadingFrom());
                }

                if (tor.webseedsWeAreDownloadingFrom())
                {
                    //: Second (optional) part of phrase "Downloading from ... of ... connected peer(s) and ... web seed(s)",
                    //: notice that leading space (before "and") is included here
                    str += tr(" and %Ln web seed(s)", nullptr, tor.webseedsWeAreDownloadingFrom());
                }
            }

            break;

        case TR_STATUS_SEED:
            if (tor.connectedPeers() == 0)
            {
                str = tr("Seeding to %Ln peer(s)", nullptr, tor.peersWeAreUploadingTo());
            }
            else
            {
                str = tr("Seeding to %1 of %Ln connected peer(s)", nullptr, tor.connectedPeers())
                          .arg(tor.peersWeAreUploadingTo());
            }

            break;
        }
    }

    if (tor.isReadyToTransfer())
    {
        QString const s = shortTransferString(tor);

        if (!s.isEmpty())
        {
            str += tr(" - ") + s;
        }
    }

    return str.trimmed();
}

/***
****
***/

QSize TorrentDelegate::sizeHint(QStyleOptionViewItem const& option, Torrent const& tor) const
{
    auto const m = margin(*QApplication::style());
    auto const layout = ItemLayout{
        tor.name(),  progressString(tor), statusString(tor), QIcon{},
        option.font, option.direction,    QPoint{ 0, 0 },    option.rect.width() - (m.width() * 2)
    };
    return layout.size() + m * 2;
}

QSize TorrentDelegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    // if the font changed, invalidate the height cache
    if (height_font_ != option.font)
    {
        height_font_ = option.font;
        height_hint_.reset();
    }

    // ensure the height is cached
    if (!height_hint_)
    {
        auto const* tor = index.data(TorrentModel::TorrentRole).value<Torrent const*>();
        height_hint_ = sizeHint(option, *tor).height();
    }

    return { option.rect.width(), *height_hint_ };
}

QIcon& TorrentDelegate::getWarningEmblem() const
{
    auto& icon = warning_emblem_;

    if (icon.isNull())
    {
        icon = IconCache::get().getThemeIcon(QStringLiteral("emblem-important"), QStyle::SP_MessageBoxWarning);
    }

    return icon;
}

void TorrentDelegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto const* tor(index.data(TorrentModel::TorrentRole).value<Torrent const*>());
    painter->save();
    painter->setClipRect(option.rect);
    drawTorrent(painter, option, *tor);
    painter->restore();
}

void TorrentDelegate::setProgressBarPercentDone(QStyleOptionViewItem const& option, Torrent const& tor) const
{
    auto const seed_ratio_limit = tor.getSeedRatioLimit();

    if (tor.isSeeding() && seed_ratio_limit)
    {
        auto const seed_rate_ratio = tor.ratio() / *seed_ratio_limit;
        auto const scaled_progress = static_cast<int>(
            seed_rate_ratio * (progress_bar_style_.maximum - progress_bar_style_.minimum));
        progress_bar_style_.progress = progress_bar_style_.minimum + scaled_progress;
    }
    else
    {
        bool const is_magnet(!tor.hasMetadata());
        progress_bar_style_.direction = option.direction;
        progress_bar_style_.progress = static_cast<int>(
            progress_bar_style_.minimum +
            ((is_magnet ? tor.metadataPercentDone() : tor.percentDone()) *
             (progress_bar_style_.maximum - progress_bar_style_.minimum)));
    }
}

void TorrentDelegate::drawTorrent(QPainter* painter, QStyleOptionViewItem const& option, Torrent const& tor) const
{
    auto const* style = QApplication::style();

    bool const is_paused(tor.isPaused());

    bool const is_item_selected((option.state & QStyle::State_Selected) != 0);
    bool const is_item_enabled((option.state & QStyle::State_Enabled) != 0);
    bool const is_item_active((option.state & QStyle::State_Active) != 0);

    painter->save();

    if (is_item_selected)
    {
        QPalette::ColorGroup cg = is_item_enabled ? QPalette::Normal : QPalette::Disabled;

        if (cg == QPalette::Normal && !is_item_active)
        {
            cg = QPalette::Inactive;
        }

        painter->fillRect(option.rect, option.palette.brush(cg, QPalette::Highlight));
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

    auto const emblem_im = is_item_selected ? QIcon::Selected : QIcon::Normal;
    auto const emblem_icon = tor.hasError() ? getWarningEmblem() : QIcon{};

    // layout
    auto const m = margin(*style);
    auto const content_rect = QRect{ option.rect.adjusted(m.width(), m.height(), -m.width(), -m.height()) };
    auto const layout = ItemLayout{ tor.name(),  progressString(tor), statusString(tor),      emblem_icon,
                                    option.font, option.direction,    content_rect.topLeft(), content_rect.width() };

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
    painter->setFont(layout.progress_font);
    painter->drawText(layout.progress_rect, Qt::AlignLeft | Qt::AlignVCenter, layout.progressText());
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
    setProgressBarPercentDone(option, tor);

    StyleHelper::drawProgressBar(*style, *painter, progress_bar_style_);

    painter->restore();
}
