// This file Copyright © 2009-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <utility>

#include <QFileInfo>
#include <QPushButton>

#include <libtransmission/transmission.h>

#include <libtransmission/variant.h>
#include <libtransmission/torrent-metainfo.h>

#include "AddData.h"
#include "FileTreeModel.h"
#include "FreeSpaceLabel.h"
#include "OptionsDialog.h"
#include "Prefs.h"
#include "Session.h"
#include "Torrent.h"
#include "Utils.h"
#include "VariantHelpers.h"

using ::trqt::variant_helpers::dictAdd;
using ::trqt::variant_helpers::listAdd;

/***
****
***/

OptionsDialog::OptionsDialog(Session& session, Prefs const& prefs, AddData addme, QWidget* parent)
    : BaseDialog{ parent }
    , add_{ std::move(addme) }
    , session_{ session }
    , is_local_{ session_.isLocal() }
{
    ui_.setupUi(this);

    setWindowTitle(add_.type == AddData::FILENAME ? tr("Open Torrent from File") : tr("Open Torrent from URL or Magnet Link"));

    edit_timer_.setInterval(2000);
    edit_timer_.setSingleShot(true);
    connect(&edit_timer_, &QTimer::timeout, this, &OptionsDialog::onDestinationChanged);

    if (add_.type == AddData::FILENAME)
    {
        ui_.sourceStack->setCurrentWidget(ui_.sourceButton);
        ui_.sourceButton->setMode(PathButton::FileMode);
        ui_.sourceButton->setTitle(tr("Open Torrent"));
        ui_.sourceButton->setNameFilter(tr("Torrent Files (*.torrent);;All Files (*.*)"));
        ui_.sourceButton->setPath(add_.filename);
        connect(ui_.sourceButton, &PathButton::pathChanged, this, &OptionsDialog::onSourceChanged);
    }
    else
    {
        ui_.sourceStack->setCurrentWidget(ui_.sourceEdit);
        ui_.sourceEdit->setText(add_.readableName());
        ui_.sourceEdit->selectAll();
        connect(ui_.sourceEdit, &QLineEdit::editingFinished, this, &OptionsDialog::onSourceChanged);
    }

    ui_.sourceStack->setFixedHeight(ui_.sourceStack->currentWidget()->sizeHint().height());
    ui_.sourceLabel->setBuddy(ui_.sourceStack->currentWidget());

    auto const font_metrics = QFontMetrics{ font() };
    int const width = font_metrics.size(0, QStringLiteral("This is a pretty long torrent filename indeed.torrent")).width();
    ui_.sourceStack->setMinimumWidth(width);

    auto const download_dir = Utils::removeTrailingDirSeparator(prefs.getString(Prefs::DOWNLOAD_DIR));
    ui_.freeSpaceLabel->setSession(session_);
    ui_.freeSpaceLabel->setPath(download_dir);

    ui_.destinationButton->setMode(PathButton::DirectoryMode);
    ui_.destinationButton->setTitle(tr("Select Destination"));
    ui_.destinationButton->setPath(download_dir);
    ui_.destinationEdit->setText(download_dir);

    if (is_local_)
    {
        local_destination_.setPath(download_dir);
    }

    connect(ui_.destinationButton, &PathButton::pathChanged, this, &OptionsDialog::onDestinationChanged);
    connect(ui_.destinationEdit, &QLineEdit::textEdited, &edit_timer_, qOverload<>(&QTimer::start));
    connect(ui_.destinationEdit, &QLineEdit::editingFinished, this, &OptionsDialog::onDestinationChanged);

    ui_.filesView->setEditable(false);
    ui_.priorityCombo->addItem(tr("High"), TR_PRI_HIGH);
    ui_.priorityCombo->addItem(tr("Normal"), TR_PRI_NORMAL);
    ui_.priorityCombo->addItem(tr("Low"), TR_PRI_LOW);
    ui_.priorityCombo->setCurrentIndex(1); // Normal

    ui_.startCheck->setChecked(prefs.getBool(Prefs::START));
    ui_.trashCheck->setChecked(prefs.getBool(Prefs::TRASH_ORIGINAL));

    connect(ui_.dialogButtons, &QDialogButtonBox::rejected, this, &QObject::deleteLater);
    connect(ui_.dialogButtons, &QDialogButtonBox::accepted, this, &OptionsDialog::onAccepted);

    connect(ui_.filesView, &FileTreeView::priorityChanged, this, &OptionsDialog::onPriorityChanged);
    connect(ui_.filesView, &FileTreeView::wantedChanged, this, &OptionsDialog::onWantedChanged);

    connect(&session_, &Session::sessionUpdated, this, &OptionsDialog::onSessionUpdated);

    updateWidgetsLocality();
    reload();
}

OptionsDialog::~OptionsDialog()
{
    clearInfo();
}

/***
****
***/

void OptionsDialog::clearInfo()
{
    metainfo_.reset();
    files_.clear();
}

void OptionsDialog::reload()
{
    clearInfo();

    auto metainfo = tr_torrent_metainfo{};
    auto ok = bool{};

    switch (add_.type)
    {
    case AddData::MAGNET:
        ok = metainfo.parseMagnet(add_.magnet.toStdString());
        break;

    case AddData::FILENAME:
        ok = metainfo.parse_torrent_file(add_.filename.toStdString());
        break;

    case AddData::METAINFO:
        ok = metainfo.parse_benc(add_.metainfo.toStdString());
        break;

    default:
        break;
    }

    metainfo_.reset();
    ui_.filesView->clear();
    files_.clear();
    priorities_.clear();
    wanted_.clear();

    if (ok)
    {
        metainfo_ = metainfo;
    }

    bool const have_files_to_show = metainfo_ && !std::empty(*metainfo_);

    ui_.filesView->setVisible(have_files_to_show);
    layout()->setSizeConstraint(have_files_to_show ? QLayout::SetDefaultConstraint : QLayout::SetFixedSize);

    if (metainfo_)
    {
        auto const n_files = metainfo_->file_count();
        priorities_.assign(n_files, TR_PRI_NORMAL);
        wanted_.assign(n_files, true);

        for (tr_file_index_t i = 0; i < n_files; ++i)
        {
            auto f = TorrentFile{};
            f.index = i;
            f.priority = priorities_[i];
            f.wanted = wanted_[i];
            f.size = metainfo_->file_size(i);
            f.have = 0;
            f.filename = QString::fromStdString(metainfo_->file_subpath(i));
            files_.push_back(f);
        }
    }

    ui_.filesView->update(files_);
    ui_.filesView->hideColumn(FileTreeModel::COL_PROGRESS);
}

void OptionsDialog::updateWidgetsLocality()
{
    ui_.destinationStack->setCurrentWidget(is_local_ ? static_cast<QWidget*>(ui_.destinationButton) : ui_.destinationEdit);
    ui_.destinationStack->setFixedHeight(ui_.destinationStack->currentWidget()->sizeHint().height());
    ui_.destinationLabel->setBuddy(ui_.destinationStack->currentWidget());
}

void OptionsDialog::onSessionUpdated()
{
    bool const is_local = session_.isLocal();

    if (is_local_ != is_local)
    {
        is_local_ = is_local;
        updateWidgetsLocality();
    }
}

void OptionsDialog::onPriorityChanged(QSet<int> const& file_indices, int priority)
{
    for (int const i : file_indices)
    {
        priorities_[i] = priority;
    }
}

void OptionsDialog::onWantedChanged(QSet<int> const& file_indices, bool is_wanted)
{
    for (int const i : file_indices)
    {
        wanted_[i] = is_wanted;
    }
}

void OptionsDialog::onAccepted()
{
    // rpc spec section 3.4 "adding a torrent"

    auto args = tr_variant::make_map(10U);
    QString download_dir;

    // "download-dir"
    if (ui_.destinationStack->currentWidget() == ui_.destinationButton)
    {
        download_dir = local_destination_.absolutePath();
    }
    else
    {
        download_dir = ui_.destinationEdit->text();
    }

    dictAdd(&args, TR_KEY_download_dir, download_dir);

    // paused
    dictAdd(&args, TR_KEY_paused, !ui_.startCheck->isChecked());

    // priority
    int const index = ui_.priorityCombo->currentIndex();
    int const priority = ui_.priorityCombo->itemData(index).toInt();
    dictAdd(&args, TR_KEY_bandwidthPriority, priority);

    // files-unwanted
    auto count = std::count(wanted_.begin(), wanted_.end(), false);

    if (count > 0)
    {
        tr_variant* l = tr_variantDictAddList(&args, TR_KEY_files_unwanted, count);

        for (int i = 0, n = wanted_.size(); i < n; ++i)
        {
            if (!wanted_.at(i))
            {
                listAdd(l, i);
            }
        }
    }

    // priority-low
    count = std::count(priorities_.begin(), priorities_.end(), TR_PRI_LOW);

    if (count > 0)
    {
        tr_variant* l = tr_variantDictAddList(&args, TR_KEY_priority_low, count);

        for (int i = 0, n = priorities_.size(); i < n; ++i)
        {
            if (priorities_.at(i) == TR_PRI_LOW)
            {
                listAdd(l, i);
            }
        }
    }

    // priority-high
    count = std::count(priorities_.begin(), priorities_.end(), TR_PRI_HIGH);

    if (count > 0)
    {
        tr_variant* l = tr_variantDictAddList(&args, TR_KEY_priority_high, count);

        for (int i = 0, n = priorities_.size(); i < n; ++i)
        {
            if (priorities_.at(i) == TR_PRI_HIGH)
            {
                listAdd(l, i);
            }
        }
    }

    session_.addTorrent(add_, &args, ui_.trashCheck->isChecked());

    deleteLater();
}

void OptionsDialog::onSourceChanged()
{
    if (ui_.sourceStack->currentWidget() == ui_.sourceButton)
    {
        add_.set(ui_.sourceButton->path());
    }
    else if (auto const text = ui_.sourceEdit->text(); text != add_.readableName())
    {
        add_.set(text);
    }

    reload();
}

void OptionsDialog::onDestinationChanged()
{
    if (ui_.destinationStack->currentWidget() == ui_.destinationButton)
    {
        local_destination_.setPath(ui_.destinationButton->path());
        ui_.freeSpaceLabel->setPath(local_destination_.absolutePath());
    }
    else
    {
        ui_.freeSpaceLabel->setPath(ui_.destinationEdit->text());
    }
}
