// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cassert>
#include <ctime>
#include <map>
#include <set>
#include <utility>

#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHeaderView>
#include <QHostAddress>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QStringList>
#include <QString>
#include <QStyle>
#include <QTreeWidgetItem>

#include <libtransmission/transmission.h>
#include <libtransmission/announce-list.h>
#include <libtransmission/utils.h> // tr_getRatio()

#include "BaseDialog.h"
#include "ColumnResizer.h"
#include "DetailsDialog.h"
#include "Formatter.h"
#include "IconCache.h"
#include "Prefs.h"
#include "Session.h"
#include "SqueezeLabel.h"
#include "Torrent.h"
#include "TorrentModel.h"
#include "TrackerDelegate.h"
#include "TrackerModel.h"
#include "TrackerModelFilter.h"
#include "Utils.h"

#include "ui_TrackersDialog.h"

class Prefs;
class Session;

// ---

namespace
{

class TrackersDialog : public BaseDialog
{
    Q_OBJECT

public:
    explicit TrackersDialog(QString tracker_list, QWidget* parent = nullptr)
        : BaseDialog{ parent }
    {
        ui_.setupUi(this);
        ui_.trackerList->setPlainText(tracker_list);
        connect(ui_.dialogButtons, &QDialogButtonBox::clicked, this, &TrackersDialog::onButtonBoxClicked);
    }

signals:
    void trackerListEdited(QString /*tracker_list*/);

private slots:
    void onButtonBoxClicked(QAbstractButton* button)
    {
        if (ui_.dialogButtons->standardButton(button) == QDialogButtonBox::Ok)
        {
            emit trackerListEdited(ui_.trackerList->toPlainText());
        }

        close();
    }

private:
    Ui::TrackersDialog ui_{};
    QTimer timer_;
};

constexpr tr_quark priorityKey(int priority)
{
    switch (priority)
    {
    case TR_PRI_LOW:
        return TR_KEY_priority_low_kebab;

    case TR_PRI_HIGH:
        return TR_KEY_priority_high_kebab;

    default:
        return TR_KEY_priority_normal_kebab;
    }
}

int constexpr DebounceIntervalMSec = 100;
int constexpr RefreshIntervalMSec = 4000;

char constexpr const* const PrefKey = "pref_key";

enum // peer columns
{
    COL_LOCK,
    COL_UP,
    COL_DOWN,
    COL_PERCENT,
    COL_STATUS,
    COL_ADDRESS,
    COL_CLIENT,
    N_COLUMNS
};

int measureViewItem(QTreeWidget const* view, int column, QString const& text)
{
    QTreeWidgetItem const* header_item = view->headerItem();

    int const item_width = Utils::measureViewItem(view, text);
    int const header_width = Utils::measureHeaderItem(view->header(), header_item->text(column));

    return std::max(item_width, header_width);
}

} // namespace

// ---

class PeerItem : public QTreeWidgetItem
{
    Peer peer_;
    QString mutable collated_address_;
    QString status_;

public:
    explicit PeerItem(Peer p)
        : peer_{ std::move(p) }
    {
    }

    void refresh(Peer const& p)
    {
        if (p.address != peer_.address)
        {
            collated_address_.clear();
        }

        peer_ = p;
    }

    void setStatus(QString const& s)
    {
        status_ = s;
    }

    bool operator<(QTreeWidgetItem const& other) const override
    {
        auto const* i = dynamic_cast<PeerItem const*>(&other);
        auto const* tw = treeWidget();
        int const column = tw != nullptr ? tw->sortColumn() : 0;

        assert(i != nullptr);

        switch (column)
        {
        case COL_UP:
            return peer_.rate_to_peer < i->peer_.rate_to_peer;

        case COL_DOWN:
            return peer_.rate_to_client < i->peer_.rate_to_client;

        case COL_PERCENT:
            return peer_.progress < i->peer_.progress;

        case COL_STATUS:
            return status_ < i->status_;

        case COL_CLIENT:
            return peer_.client_name < i->peer_.client_name;

        case COL_LOCK:
            return peer_.is_encrypted && !i->peer_.is_encrypted;

        default:
            return address() < i->address();
        }
    }

private:
    QString const& address() const
    {
        if (collated_address_.isEmpty())
        {
            collated_address_ = collateAddress(peer_.address);
        }

        return collated_address_;
    }

    [[nodiscard]] static QString collateAddress(QString const& address)
    {
        auto collated = QString{};

        if (auto ip_address = QHostAddress{}; ip_address.setAddress(address))
        {
            if (ip_address.protocol() == QAbstractSocket::IPv4Protocol)
            {
                quint32 const ipv4_address = ip_address.toIPv4Address();
                collated = QStringLiteral("1-") +
                    QString::fromUtf8(QByteArray::number(ipv4_address, 16).rightJustified(8, '0'));
            }
            else if (ip_address.protocol() == QAbstractSocket::IPv6Protocol)
            {
                Q_IPV6ADDR const ipv6_address = ip_address.toIPv6Address();
                QByteArray tmp(16, '\0');

                for (int i = 0; i < 16; ++i)
                {
                    tmp[i] = ipv6_address[i];
                }

                collated = QStringLiteral("2-") + QString::fromUtf8(tmp.toHex());
            }
        }

        if (collated.isEmpty())
        {
            collated = QStringLiteral("3-") + address.toLower();
        }

        return collated;
    }
};

// ---

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int DetailsDialog::prev_tab_index_ = 0;

DetailsDialog::DetailsDialog(Session& session, Prefs& prefs, TorrentModel const& model, QWidget* parent)
    : BaseDialog{ parent }
    , session_{ session }
    , prefs_{ prefs }
    , model_{ model }
{
    ui_.setupUi(this);

    initInfoTab();
    initPeersTab();
    initTrackerTab();
    initFilesTab();
    initOptionsTab();

    adjustSize();
    ui_.commentTextEdit->setMaximumHeight(QWIDGETSIZE_MAX);
    ui_.tabs->setCurrentIndex(prev_tab_index_);

    static std::array<int, 2> constexpr InitKeys = {
        Prefs::SHOW_TRACKER_SCRAPES,
        Prefs::SHOW_BACKUP_TRACKERS,
    };

    for (int const key : InitKeys)
    {
        refreshPref(key);
    }

    connect(&model_, &TorrentModel::torrentsChanged, this, &DetailsDialog::onTorrentsChanged);
    connect(&model_, &TorrentModel::torrentsEdited, this, &DetailsDialog::onTorrentsEdited);
    connect(&prefs_, &Prefs::changed, this, &DetailsDialog::refreshPref);

    // call refreshModel periodically
    connect(&model_timer_, &QTimer::timeout, this, &DetailsDialog::refreshModel);
    model_timer_.setSingleShot(false);
    model_timer_.start(RefreshIntervalMSec);
    refreshModel();

    // set up the debounce timer
    connect(&ui_debounce_timer_, &QTimer::timeout, this, &DetailsDialog::refreshUI);
    ui_debounce_timer_.setSingleShot(true);

    // set labels
    connect(ui_.dialogButtons, &QDialogButtonBox::clicked, this, &DetailsDialog::onButtonBoxClicked);
}

DetailsDialog::~DetailsDialog()
{
    prev_tab_index_ = ui_.tabs->currentIndex();
}

void DetailsDialog::setIds(torrent_ids_t const& ids)
{
    if (ids != ids_)
    {
        setEnabled(false);
        ui_.filesView->clear();

        ids_ = ids;
        session_.refreshDetailInfo(ids_);
        tracker_model_->refresh(model_, ids_);

        labels_need_refresh_ = true;

        refreshModel();
        refreshUI();
    }
}

void DetailsDialog::refreshPref(int key)
{
    if (key == Prefs::SHOW_TRACKER_SCRAPES)
    {
        auto* selection_model = ui_.trackersView->selectionModel();
        tracker_delegate_->setShowMore(prefs_.getBool(key));
        selection_model->clear();
        ui_.trackersView->reset();
        selection_model->select(selection_model->selection(), QItemSelectionModel::Select);
        selection_model->setCurrentIndex(selection_model->currentIndex(), QItemSelectionModel::NoUpdate);
    }
    else if (key == Prefs::SHOW_BACKUP_TRACKERS)
    {
        tracker_filter_->setShowBackupTrackers(prefs_.getBool(key));
    }
}

// ---

void DetailsDialog::refreshModel()
{
    if (!ids_.empty())
    {
        session_.refreshExtraStats(ids_);
    }
}

void DetailsDialog::onTorrentsEdited(torrent_ids_t const& ids)
{
    // std::set_intersection requires sorted inputs
    auto a = std::vector<tr_torrent_id_t>{ ids.begin(), ids.end() };
    std::sort(std::begin(a), std::end(a));
    auto b = std::vector<tr_torrent_id_t>{ ids_.begin(), ids_.end() };
    std::sort(std::begin(b), std::end(b));

    // are any of the edited torrents on display here?
    torrent_ids_t interesting_ids;
    std::set_intersection(
        std::begin(a),
        std::end(a),
        std::begin(b),
        std::end(b),
        std::inserter(interesting_ids, std::begin(interesting_ids)));

    if (!interesting_ids.empty())
    {
        session_.refreshDetailInfo(interesting_ids);
    }
}

void DetailsDialog::onTorrentsChanged(torrent_ids_t const& ids, Torrent::fields_t const& fields)
{
    Q_UNUSED(fields)

    if (ui_debounce_timer_.isActive())
    {
        return;
    }

    if (!std::any_of(ids.begin(), ids.end(), [this](auto const& id) { return ids_.count(id) != 0; }))
    {
        return;
    }

    ui_debounce_timer_.start(DebounceIntervalMSec);
}

void DetailsDialog::onSessionCalled(Session::Tag tag)
{
    if ((pending_changes_tags_.erase(tag) > 0) && canEdit())
    {
        // no pending changes left, so stop listening
        disconnect(pending_changes_connection_);
        pending_changes_connection_ = {};

        refreshModel();
    }
}

void DetailsDialog::onButtonBoxClicked(QAbstractButton* button)
{
    if (ui_.dialogButtons->standardButton(button) == QDialogButtonBox::Close)
    {
        if (ui_.labelsTextEdit->isReadOnly()) // no edits could have been made
        {
            return;
        }

        QString const labels_text = ui_.labelsTextEdit->toPlainText().trimmed();

        if (labels_text == labels_baseline_) // no edits have been made
        {
            return;
        }

        QString const re = QStringLiteral("((,|;)\\s*)");

//see https://doc.qt.io/qt-5/qt.html#SplitBehaviorFlags-enum
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
        QStringList const labels_list = labels_text.split(QRegularExpression(re), QString::SkipEmptyParts);
#else
        QStringList const labels_list = labels_text.split(QRegularExpression(re), Qt::SkipEmptyParts);
#endif

        torrentSet(TR_KEY_labels, labels_list);

        if (!ids_.empty())
        {
            session_.refreshDetailInfo(ids_);
        }
    }
}

namespace
{

void setIfIdle(QComboBox* box, int i)
{
    if (!box->hasFocus())
    {
        box->blockSignals(true);
        box->setCurrentIndex(i);
        box->blockSignals(false);
    }
}

void setIfIdle(QDoubleSpinBox* spin, double value)
{
    if (!spin->hasFocus())
    {
        spin->blockSignals(true);
        spin->setValue(value);
        spin->blockSignals(false);
    }
}

void setIfIdle(QSpinBox* spin, int value)
{
    if (!spin->hasFocus())
    {
        spin->blockSignals(true);
        spin->setValue(value);
        spin->blockSignals(false);
    }
}

} // namespace

void DetailsDialog::refreshUI()
{
    bool const single = ids_.size() == 1;
    auto const blank = QString{};
    auto const fm = fontMetrics();
    auto const none = tr("None");
    auto const mixed = tr("Mixed");
    auto const unknown = tr("Unknown");
    auto const now = time(nullptr);

    // build a list of torrents
    auto torrents = QList<Torrent const*>{};
    for (int const id : ids_)
    {
        Torrent const* tor = model_.getTorrentFromId(id);

        if (tor != nullptr)
        {
            torrents << tor;
        }
    }

    ///
    ///  activity tab
    ///

    // myStateLabel
    auto string = QString{};
    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        bool is_mixed = false;
        bool all_paused = true;
        bool all_finished = true;
        tr_torrent_activity const baseline = torrents[0]->getActivity();

        for (Torrent const* const t : torrents)
        {
            tr_torrent_activity const activity = t->getActivity();

            if (activity != baseline)
            {
                is_mixed = true;
            }

            if (activity != TR_STATUS_STOPPED)
            {
                all_paused = all_finished = false;
            }

            if (!t->isFinished())
            {
                all_finished = false;
            }
        }

        if (is_mixed)
        {
            string = mixed;
        }
        else if (all_finished)
        {
            string = tr("Finished");
        }
        else if (all_paused)
        {
            string = tr("Paused");
        }
        else
        {
            string = torrents[0]->activityString();
        }
    }

    ui_.stateValueLabel->setText(string);
    auto const state_string = string;

    // myHaveLabel
    uint64_t size_when_done = 0;
    uint64_t available = 0;

    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        uint64_t left_until_done = 0;
        int64_t have_verified = 0;
        int64_t have_unverified = 0;

        for (Torrent const* const t : torrents)
        {
            if (t->hasMetadata())
            {
                have_unverified += t->haveUnverified();
                uint64_t const v = t->haveVerified();
                have_verified += v;
                size_when_done += t->sizeWhenDone();
                left_until_done += t->leftUntilDone();
                available += t->sizeWhenDone() - t->leftUntilDone() + t->haveUnverified() + t->desiredAvailable();
            }
        }

        double const d = size_when_done == 0 ?
            100.0 :
            100.0 * static_cast<double>(size_when_done - left_until_done) / static_cast<double>(size_when_done);
        auto const pct = Formatter::percent_to_string(d);
        auto const size_when_done_str = Formatter::storage_to_string(size_when_done);

        if (have_unverified == 0 && left_until_done == 0)
        {
            //: Text following the "Have:" label in torrent properties dialog;
            //: %1 is amount of downloaded and verified data
            string = tr("%1 (100%)").arg(Formatter::storage_to_string(have_verified));
        }
        else if (have_unverified == 0)
        {
            //: Text following the "Have:" label in torrent properties dialog;
            //: %1 is amount of downloaded and verified data,
            //: %2 is overall size of torrent data,
            //: %3 is percentage (%1/%2*100)
            string = tr("%1 of %2 (%3%)").arg(Formatter::storage_to_string(have_verified)).arg(size_when_done_str).arg(pct);
        }
        else
        {
            //: Text following the "Have:" label in torrent properties dialog;
            //: %1 is amount of downloaded data (both verified and unverified),
            //: %2 is overall size of torrent data,
            //: %3 is percentage (%1/%2*100),
            //: %4 is amount of downloaded but not yet verified data
            string = tr("%1 of %2 (%3%), %4 Unverified")
                         .arg(Formatter::storage_to_string(have_verified + have_unverified))
                         .arg(size_when_done_str)
                         .arg(pct)
                         .arg(Formatter::storage_to_string(have_unverified));
        }
    }

    ui_.haveValueLabel->setText(string);

    // myAvailabilityLabel
    if (torrents.empty() || size_when_done == 0)
    {
        string = none;
    }
    else
    {
        auto const percent = 100.0 * static_cast<double>(available) / static_cast<double>(size_when_done);
        string = QStringLiteral("%1%").arg(Formatter::percent_to_string(percent));
    }

    ui_.availabilityValueLabel->setText(string);

    // myDownloadedLabel
    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        auto d = uint64_t{};
        auto f = uint64_t{};

        for (Torrent const* const t : torrents)
        {
            d += t->downloadedEver();
            f += t->failedEver();
        }

        auto const dstr = Formatter::storage_to_string(d);
        auto const fstr = Formatter::storage_to_string(f);

        if (f != 0)
        {
            string = tr("%1 (+%2 discarded after failed checksum)").arg(dstr).arg(fstr);
        }
        else
        {
            string = dstr;
        }
    }

    ui_.downloadedValueLabel->setText(string);

    //  myUploadedLabel
    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        auto uploaded = uint64_t{};
        auto denominator = uint64_t{};

        for (Torrent const* const t : torrents)
        {
            uploaded += t->uploadedEver();
            denominator += t->sizeWhenDone();
        }

        string = tr("%1 (Ratio: %2)")
                     .arg(Formatter::storage_to_string(uploaded))
                     .arg(Formatter::ratio_to_string(tr_getRatio(uploaded, denominator)));
    }

    ui_.uploadedValueLabel->setText(string);

    // myRunTimeLabel
    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        bool all_paused = true;
        auto baseline = torrents[0]->lastStarted();

        for (Torrent const* const t : torrents)
        {
            if (baseline != t->lastStarted())
            {
                baseline = 0;
            }

            if (!t->isPaused())
            {
                all_paused = false;
            }
        }

        if (all_paused)
        {
            string = state_string; // paused || finished
        }
        else if (baseline == 0)
        {
            string = mixed;
        }
        else
        {
            auto const seconds = static_cast<int>(std::difftime(now, baseline));
            string = Formatter::time_to_string(seconds);
        }
    }

    ui_.runningTimeValueLabel->setText(string);

    // myETALabel
    string.clear();

    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        int const baseline = torrents[0]->getETA();

        for (Torrent const* const t : torrents)
        {
            if (baseline != t->getETA())
            {
                string = mixed;
                break;
            }
        }

        if (string.isEmpty())
        {
            if (baseline < 0)
            {
                string = tr("Unknown");
            }
            else
            {
                string = Formatter::time_to_string(baseline);
            }
        }
    }

    ui_.remainingTimeValueLabel->setText(string);

    // myLastActivityLabel
    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        auto latest = torrents[0]->lastActivity();

        for (Torrent const* const tor : torrents)
        {
            latest = std::max(latest, tor->lastActivity());
        }

        auto const seconds = static_cast<int>(std::difftime(now, latest));

        if (latest == 0)
        {
            string = none;
        }
        else if (seconds < 5)
        {
            string = tr("Active now");
        }
        else
        {
            string = tr("%1 ago").arg(Formatter::time_to_string(seconds));
        }
    }

    ui_.lastActivityValueLabel->setText(string);

    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        string = torrents[0]->getError();

        for (Torrent const* const t : torrents)
        {
            if (string != t->getError())
            {
                string = mixed;
                break;
            }
        }
    }

    if (string.isEmpty())
    {
        string = none;
    }

    ui_.errorValueLabel->setText(string);

    ///
    /// information tab
    ///

    // mySizeLabel
    if (torrents.empty())
    {
        string = none;
    }
    else
    {
        int pieces = 0;
        auto size = uint64_t{};
        uint32_t piece_size = torrents[0]->pieceSize();

        for (Torrent const* const t : torrents)
        {
            pieces += t->pieceCount();
            size += t->totalSize();

            if (piece_size != t->pieceSize())
            {
                piece_size = 0;
            }
        }

        if (size == 0)
        {
            string = none;
        }
        else if (piece_size > 0)
        {
            string = tr("%1 (%Ln pieces @ %2)", "", pieces)
                         .arg(Formatter::storage_to_string(size))
                         .arg(Formatter::memory_to_string(piece_size));
        }
        else
        {
            string = tr("%1 (%Ln pieces)", "", pieces).arg(Formatter::storage_to_string(size));
        }
    }

    ui_.sizeValueLabel->setText(string);

    // myHashLabel
    if (torrents.empty())
    {
        string = none;
    }
    else if (torrents.size() > 1)
    {
        string = mixed;
    }
    else
    {
        string = torrents.front()->hash().toString();
    }

    ui_.hashValueLabel->setText(string);

    // myPrivacyLabel
    string = none;

    if (!torrents.empty())
    {
        bool const b = torrents[0]->isPrivate();
        string = b ? tr("Private to this tracker -- DHT and PEX disabled") : tr("Public torrent");

        for (Torrent const* const t : torrents)
        {
            if (b != t->isPrivate())
            {
                string = mixed;
                break;
            }
        }
    }

    ui_.privacyValueLabel->setText(string);

    // myLabelsTextEdit
    if (labels_need_refresh_)
    {
        labels_need_refresh_ = false;

        if (torrents.empty())
        {
            labels_baseline_.clear();
            ui_.labelsTextEdit->setPlainText({});
            ui_.labelsTextEdit->setPlaceholderText(none);
            ui_.labelsTextEdit->setReadOnly(true);
            ui_.labelsTextEdit->setEnabled(true);
        }
        else if (auto const& baseline = torrents[0]->labels(); std::all_of(
                     std::begin(torrents),
                     std::end(torrents),
                     [&baseline](auto const* tor) { return tor->labels() == baseline; }))
        {
            labels_baseline_ = baseline.join(QStringLiteral(", "));
            ui_.labelsTextEdit->setPlainText(labels_baseline_);
            ui_.labelsTextEdit->setPlaceholderText(none);
            ui_.labelsTextEdit->setReadOnly(false);
            ui_.labelsTextEdit->setEnabled(true);
        }
        else // mixed
        {
            labels_baseline_.clear();
            ui_.labelsTextEdit->setPlainText({});
            ui_.labelsTextEdit->setPlaceholderText(mixed);
            ui_.labelsTextEdit->setEnabled(false);
        }
    }

    // myCommentBrowser
    string = none;
    bool is_comment_mixed = false;

    if (!torrents.empty())
    {
        string = torrents[0]->comment();

        for (Torrent const* const t : torrents)
        {
            if (string != t->comment())
            {
                string = mixed;
                is_comment_mixed = true;
                break;
            }
        }
    }

    if (ui_.commentTextEdit->toPlainText() != string)
    {
        ui_.commentTextEdit->setPlainText(string);
    }

    ui_.commentTextEdit->setEnabled(!is_comment_mixed && !string.isEmpty());

    // myOriginLabel
    string = none;

    if (!torrents.empty())
    {
        bool mixed_creator = false;
        bool mixed_date = false;
        auto const creator = torrents[0]->creator();
        auto const date = torrents[0]->dateCreated();

        for (Torrent const* const t : torrents)
        {
            mixed_creator |= (creator != t->creator());
            mixed_date |= (date != t->dateCreated());
        }

        bool const empty_creator = creator.isEmpty();
        bool const empty_date = date <= 0;

        if (mixed_creator || mixed_date)
        {
            string = mixed;
        }
        else if (empty_creator && empty_date)
        {
            string = tr("N/A");
        }
        else if (empty_date && !empty_creator)
        {
            string = tr("Created by %1").arg(creator);
        }
        else if (empty_creator && !empty_date)
        {
            auto const date_str = QDateTime::fromSecsSinceEpoch(date).toString();
            string = tr("Created on %1").arg(date_str);
        }
        else
        {
            auto const date_str = QDateTime::fromSecsSinceEpoch(date).toString();
            string = tr("Created by %1 on %2").arg(creator).arg(date_str);
        }
    }

    ui_.originValueLabel->setText(string);

    // myLocationLabel
    string = none;

    if (!torrents.empty())
    {
        string = torrents[0]->getPath();

        for (Torrent const* const t : torrents)
        {
            if (string != t->getPath())
            {
                string = mixed;
                break;
            }
        }
    }

    ui_.locationValueLabel->setText(string);
    // myAddedLabel
    string = none;

    if (!torrents.empty())
    {
        auto const date = torrents[0]->dateAdded();
        bool mixed_date = false;

        for (Torrent const* const t : torrents)
        {
            mixed_date |= (date != t->dateAdded());
        }

        bool const empty_date = date <= 0;

        if (empty_date)
        {
            string = tr("N/A");
        }
        else if (mixed_date)
        {
            string = mixed;
        }
        else
        {
            auto const date_str = QDateTime::fromSecsSinceEpoch(date).toString();
            string = date_str;
        }
    }

    ui_.addedValueLabel->setText(string);

    ///
    ///  Options Tab
    ///

    if (canEdit() && !torrents.empty())
    {
        Torrent const& baseline = *torrents.front();

        // mySessionLimitCheck
        bool uniform = true;
        bool baseline_flag = baseline.honorsSessionLimits();

        for (Torrent const* const tor : torrents)
        {
            if (baseline_flag != tor->honorsSessionLimits())
            {
                uniform = false;
                break;
            }
        }

        ui_.sessionLimitCheck->setChecked(uniform && baseline_flag);

        // mySingleDownCheck
        uniform = true;
        baseline_flag = baseline.downloadIsLimited();

        for (Torrent const* const tor : torrents)
        {
            if (baseline_flag != tor->downloadIsLimited())
            {
                uniform = false;
                break;
            }
        }

        ui_.singleDownCheck->setChecked(uniform && baseline_flag);

        // mySingleUpCheck
        uniform = true;
        baseline_flag = baseline.uploadIsLimited();

        for (Torrent const* const tor : torrents)
        {
            if (baseline_flag != tor->uploadIsLimited())
            {
                uniform = false;
                break;
            }
        }

        ui_.singleUpCheck->setChecked(uniform && baseline_flag);

        // myBandwidthPriorityCombo
        uniform = true;
        int const baseline_int = baseline.getBandwidthPriority();

        for (Torrent const* const tor : torrents)
        {
            if (baseline_int != tor->getBandwidthPriority())
            {
                uniform = false;
                break;
            }
        }

        int const i = uniform ? ui_.bandwidthPriorityCombo->findData(baseline_int) : -1;

        setIfIdle(ui_.bandwidthPriorityCombo, i);

        setIfIdle(ui_.singleDownSpin, static_cast<int>(baseline.downloadLimit().count(Speed::Units::KByps)));
        setIfIdle(ui_.singleUpSpin, static_cast<int>(baseline.uploadLimit().count(Speed::Units::KByps)));
        setIfIdle(ui_.peerLimitSpin, baseline.peerLimit());
    }

    if (!torrents.empty())
    {
        Torrent const& baseline = *torrents.front();

        // ratio
        bool uniform = true;
        int baseline_int = baseline.seedRatioMode();

        for (Torrent const* const tor : torrents)
        {
            if (baseline_int != tor->seedRatioMode())
            {
                uniform = false;
                break;
            }
        }

        setIfIdle(ui_.ratioCombo, uniform ? ui_.ratioCombo->findData(baseline_int) : -1);
        ui_.ratioSpin->setVisible(uniform && baseline_int == TR_RATIOLIMIT_SINGLE);

        setIfIdle(ui_.ratioSpin, baseline.seedRatioLimit());

        // idle
        uniform = true;
        baseline_int = baseline.seedIdleMode();

        for (Torrent const* const tor : torrents)
        {
            if (baseline_int != tor->seedIdleMode())
            {
                uniform = false;
                break;
            }
        }

        setIfIdle(ui_.idleCombo, uniform ? ui_.idleCombo->findData(baseline_int) : -1);
        ui_.idleSpin->setVisible(uniform && baseline_int == TR_RATIOLIMIT_SINGLE);

        setIfIdle(ui_.idleSpin, baseline.seedIdleLimit());
        onIdleLimitChanged();
    }

    ///
    ///  Tracker tab
    ///

    tracker_model_->refresh(model_, ids_);
    ui_.editTrackersButton->setEnabled(std::size(ids_) == 1);

    ///
    ///  Peers tab
    ///

    auto peers2 = decltype(peers_){};
    QList<QTreeWidgetItem*> new_items;

    for (Torrent const* const t : torrents)
    {
        auto const id_str = QString::number(t->id());

        for (Peer const& peer : t->peers())
        {
            auto const key = id_str + QLatin1Char(':') + peer.address;

            PeerItem* item = nullptr;
            if (auto iter = peers_.find(key); iter != std::end(peers_))
            {
                item = dynamic_cast<PeerItem*>(iter->second);
            }
            else // new peer has connected
            {
                item = new PeerItem{ peer };
                item->setTextAlignment(COL_UP, Qt::AlignRight | Qt::AlignVCenter);
                item->setTextAlignment(COL_DOWN, Qt::AlignRight | Qt::AlignVCenter);
                item->setTextAlignment(COL_PERCENT, Qt::AlignRight | Qt::AlignVCenter);
                item->setIcon(COL_LOCK, peer.is_encrypted ? icon_encrypted_ : icon_unencrypted_);
                item->setToolTip(COL_LOCK, peer.is_encrypted ? tr("Encrypted connection") : QString{});
                item->setText(COL_ADDRESS, peer.address);
                item->setText(COL_CLIENT, peer.client_name);
                new_items << item;
            }

            auto const& code = peer.flags;
            item->setStatus(code);
            item->refresh(peer);

            QString code_tip;

            for (QChar const ch : code)
            {
                QString txt;

                switch (ch.unicode())
                {
                case 'O':
                    txt = tr("Optimistic unchoke");
                    break;

                case 'D':
                    txt = tr("Downloading from this peer");
                    break;

                case 'd':
                    txt = tr("We would download from this peer if they would let us");
                    break;

                case 'U':
                    txt = tr("Uploading to peer");
                    break;

                case 'u':
                    txt = tr("We would upload to this peer if they asked");
                    break;

                case 'K':
                    txt = tr("Peer has unchoked us, but we're not interested");
                    break;

                case '?':
                    txt = tr("We unchoked this peer, but they're not interested");
                    break;

                case 'E':
                    txt = tr("Encrypted connection");
                    break;

                case 'H':
                    txt = tr("Peer was discovered through DHT");
                    break;

                case 'X':
                    txt = tr("Peer was discovered through Peer Exchange (PEX)");
                    break;

                case 'I':
                    txt = tr("Peer is an incoming connection");
                    break;

                case 'T':
                    txt = tr("Peer is connected over µTP");
                    break;

                default:
                    break;
                }

                if (!txt.isEmpty())
                {
                    code_tip += QStringLiteral("%1: %2\n").arg(ch).arg(txt);
                }
            }

            if (!code_tip.isEmpty())
            {
                code_tip.resize(code_tip.size() - 1); // eat the trailing linefeed
            }

            item->setText(COL_UP, peer.rate_to_peer.is_zero() ? QString{} : peer.rate_to_peer.to_qstring());
            item->setText(COL_DOWN, peer.rate_to_client.is_zero() ? QString{} : peer.rate_to_client.to_qstring());
            item->setText(
                COL_PERCENT,
                peer.progress > 0 ? QStringLiteral("%1%").arg(static_cast<int>(peer.progress * 100.0)) : QString{});
            item->setText(COL_STATUS, code);
            item->setToolTip(COL_STATUS, code_tip);

            peers2.try_emplace(key, item);
        }
    }

    ui_.peersView->addTopLevelItems(new_items);

    for (auto const& [key, item] : peers_)
    {
        if (peers2.count(key) == 0U) // old peer has disconnected
        {
            ui_.peersView->takeTopLevelItem(ui_.peersView->indexOfTopLevelItem(item));
            delete item;
        }
    }

    peers_ = std::move(peers2);

    if (single)
    {
        ui_.filesView->update(torrents[0]->files(), canEdit());
    }
    else
    {
        ui_.filesView->clear();
    }

    setEnabled(true);
}

void DetailsDialog::setEnabled(bool enabled)
{
    ui_.tabs->setEnabled(enabled);
}

// ---

void DetailsDialog::initInfoTab()
{
    int const cbh = QFontMetrics{ ui_.commentTextEdit->font() }.lineSpacing() * 4;
    ui_.commentTextEdit->setFixedHeight(cbh);

    int const lteh = QFontMetrics{ ui_.labelsTextEdit->font() }.lineSpacing() * 2;
    ui_.labelsTextEdit->setFixedHeight(lteh);
    ui_.labelsTextEdit->setPlainText(QStringLiteral("Initializing..."));

    auto* cr = new ColumnResizer{ this };
    cr->addLayout(ui_.activitySectionLayout);
    cr->addLayout(ui_.detailsSectionLayout);
    cr->update();
}

// ---

void DetailsDialog::onShowTrackerScrapesToggled(bool val)
{
    prefs_.set(Prefs::SHOW_TRACKER_SCRAPES, val);
}

void DetailsDialog::onShowBackupTrackersToggled(bool val)
{
    prefs_.set(Prefs::SHOW_BACKUP_TRACKERS, val);
}

void DetailsDialog::onHonorsSessionLimitsToggled(bool val)
{
    torrentSet(TR_KEY_honors_session_limits_camel, val);
}

void DetailsDialog::onDownloadLimitedToggled(bool val)
{
    torrentSet(TR_KEY_download_limited_camel, val);
}

void DetailsDialog::onSpinBoxEditingFinished()
{
    QObject const* spin = sender();
    tr_quark const key = spin->property(PrefKey).toInt();
    auto const* d = qobject_cast<QDoubleSpinBox const*>(spin);

    if (d != nullptr)
    {
        torrentSet(key, d->value());
    }
    else
    {
        torrentSet(key, qobject_cast<QSpinBox const*>(spin)->value());
    }
}

void DetailsDialog::onUploadLimitedToggled(bool val)
{
    torrentSet(TR_KEY_uploadLimited, val);
}

void DetailsDialog::onIdleModeChanged(int index)
{
    int const val = ui_.idleCombo->itemData(index).toInt();
    torrentSet(TR_KEY_seed_idle_mode_camel, val);
}

void DetailsDialog::onIdleLimitChanged()
{
    //: Spin box format, "Stop seeding if idle for: [ 5 minutes ]"
    auto const* const units_format = QT_TRANSLATE_N_NOOP("DetailsDialog", "%1 minute(s)");
    auto const placeholder = QStringLiteral("%1");
    Utils::updateSpinBoxFormat(ui_.idleSpin, "DetailsDialog", units_format, placeholder);
}

void DetailsDialog::onRatioModeChanged(int index)
{
    int const val = ui_.ratioCombo->itemData(index).toInt();
    torrentSet(TR_KEY_seed_ratio_mode_camel, val);
}

void DetailsDialog::onBandwidthPriorityChanged(int index)
{
    if (index != -1)
    {
        int const priority = ui_.bandwidthPriorityCombo->itemData(index).toInt();
        torrentSet(TR_KEY_bandwidth_priority_camel, priority);
    }
}

void DetailsDialog::onTrackerSelectionChanged()
{
    int const selection_count = ui_.trackersView->selectionModel()->selectedRows().size();
    ui_.removeTrackerButton->setEnabled(selection_count > 0);
}

void DetailsDialog::onAddTrackerClicked()
{
    bool ok = false;
    auto const text_qstr = QInputDialog::getMultiLineText(
        this,
        tr("Add URL(s)"),
        tr("Add tracker announce URLs, one per line:"),
        {},
        &ok);
    if (!ok)
    {
        return;
    }

    // for each URL entered by the user...
    auto announce_list = tr_announce_list{};
    announce_list.parse(text_qstr.toStdString());
    auto url_to_ids = std::map<QString, std::set<tr_torrent_id_t>>{};
    for (auto const& info : announce_list)
    {
        // for each selected torrent...
        auto sv = info.announce.sv();
        auto const announce_url = QString::fromUtf8(std::data(sv), std::size(sv));
        for (auto const& id : ids_)
        {
            // make a note if the torrent doesn't already have the URL
            if (tracker_model_->find(id, announce_url) == -1)
            {
                url_to_ids[announce_url].insert(id);
            }
        }
    }

    // now reverse the map so that if we're adding identical trackers
    // to more than one torrent, that can be batched into a single call
    auto ids_to_urls = std::map<std::set<tr_torrent_id_t>, std::set<QString>>{};
    for (auto& [announce_url, ids] : url_to_ids)
    {
        ids_to_urls[ids].insert(announce_url);
    }

    if (std::empty(ids_to_urls))
    {
        QMessageBox::warning(this, tr("Error"), tr("No new URLs found."));
    }
    else
    {
        for (auto const& [ids, urls] : ids_to_urls)
        {
            auto urls_list = QList<QString>{};
            urls_list.reserve(std::size(urls));
            for (auto const& url : urls)
            {
                urls_list << url;
            }

            torrentSet(torrent_ids_t{ std::begin(ids), std::end(ids) }, TR_KEY_tracker_add_camel, urls_list);
        }
    }
}

void DetailsDialog::onTrackerListEdited(QString tracker_list)
{
    torrentSet(TR_KEY_trackerList, tracker_list);
}

void DetailsDialog::onEditTrackersClicked()
{
    if (std::size(ids_) != 1)
    {
        return;
    }

    auto const* const tor = model_.getTorrentFromId(*std::begin(ids_));
    if (tor == nullptr)
    {
        return;
    }

    auto* dialog = new TrackersDialog{ tor->trackerList(), this };
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &TrackersDialog::trackerListEdited, this, &DetailsDialog::onTrackerListEdited);
    dialog->open();
}

void DetailsDialog::onRemoveTrackerClicked()
{
    // make a map of torrentIds to announce URLs to remove
    QItemSelectionModel* selection_model = ui_.trackersView->selectionModel();
    QModelIndexList const selected_rows = selection_model->selectedRows();
    auto torrent_id_to_tracker_ids = std::map<int, std::set<int>>{};

    for (auto const& model_index : selected_rows)
    {
        auto const inf = ui_.trackersView->model()->data(model_index, TrackerModel::TrackerRole).value<TrackerInfo>();
        torrent_id_to_tracker_ids[inf.torrent_id].insert(inf.st.id);
    }

    // batch all of a tracker's torrents into one command
    for (auto const& [torrent_id, tracker_ids] : torrent_id_to_tracker_ids)
    {
        auto const ids = torrent_ids_t{ torrent_id };
        auto const values = std::vector<int>{ std::begin(tracker_ids), std::end(tracker_ids) };
        torrentSet(ids, TR_KEY_trackerRemove, values);
    }

    selection_model->clearSelection();
}

void DetailsDialog::initOptionsTab()
{
    auto const speed_unit_suffix = QStringLiteral(" %1").arg(Speed::display_name(Speed::Units::KByps));
    ui_.singleDownSpin->setSuffix(speed_unit_suffix);
    ui_.singleUpSpin->setSuffix(speed_unit_suffix);

    ui_.singleDownSpin->setProperty(PrefKey, TR_KEY_download_limit_camel);
    ui_.singleUpSpin->setProperty(PrefKey, TR_KEY_uploadLimit);
    ui_.ratioSpin->setProperty(PrefKey, TR_KEY_seed_ratio_limit_camel);
    ui_.idleSpin->setProperty(PrefKey, TR_KEY_seed_idle_limit_camel);
    ui_.peerLimitSpin->setProperty(PrefKey, TR_KEY_peer_limit_kebab);

    ui_.bandwidthPriorityCombo->addItem(tr("High"), TR_PRI_HIGH);
    ui_.bandwidthPriorityCombo->addItem(tr("Normal"), TR_PRI_NORMAL);
    ui_.bandwidthPriorityCombo->addItem(tr("Low"), TR_PRI_LOW);

    ui_.ratioCombo->addItem(tr("Use Global Settings"), TR_RATIOLIMIT_GLOBAL);
    ui_.ratioCombo->addItem(tr("Seed regardless of ratio"), TR_RATIOLIMIT_UNLIMITED);
    ui_.ratioCombo->addItem(tr("Stop seeding at ratio:"), TR_RATIOLIMIT_SINGLE);

    ui_.idleCombo->addItem(tr("Use Global Settings"), TR_IDLELIMIT_GLOBAL);
    ui_.idleCombo->addItem(tr("Seed regardless of activity"), TR_IDLELIMIT_UNLIMITED);
    ui_.idleCombo->addItem(tr("Stop seeding if idle for:"), TR_IDLELIMIT_SINGLE);

    auto* cr = new ColumnResizer{ this };
    cr->addLayout(ui_.speedSectionLayout);
    cr->addLayout(ui_.seedingLimitsSectionLayout);
    cr->addLayout(ui_.peerConnectionsSectionLayout);
    cr->update();

    // clang-format off
    void (QComboBox::* const combo_index_changed)(int) = &QComboBox::currentIndexChanged;
    void (QSpinBox::* const spin_value_changed)(int) = &QSpinBox::valueChanged;
    // clang-format on
    connect(ui_.bandwidthPriorityCombo, combo_index_changed, this, &DetailsDialog::onBandwidthPriorityChanged);
    connect(ui_.idleCombo, combo_index_changed, this, &DetailsDialog::onIdleModeChanged);
    connect(ui_.idleSpin, &QSpinBox::editingFinished, this, &DetailsDialog::onSpinBoxEditingFinished);
    connect(ui_.idleSpin, spin_value_changed, this, &DetailsDialog::onIdleLimitChanged);
    connect(ui_.peerLimitSpin, &QSpinBox::editingFinished, this, &DetailsDialog::onSpinBoxEditingFinished);
    connect(ui_.ratioCombo, combo_index_changed, this, &DetailsDialog::onRatioModeChanged);
    connect(ui_.ratioSpin, &QSpinBox::editingFinished, this, &DetailsDialog::onSpinBoxEditingFinished);
    connect(ui_.sessionLimitCheck, &QCheckBox::clicked, this, &DetailsDialog::onHonorsSessionLimitsToggled);
    connect(ui_.singleDownCheck, &QCheckBox::clicked, this, &DetailsDialog::onDownloadLimitedToggled);
    connect(ui_.singleDownSpin, &QSpinBox::editingFinished, this, &DetailsDialog::onSpinBoxEditingFinished);
    connect(ui_.singleUpCheck, &QCheckBox::clicked, this, &DetailsDialog::onUploadLimitedToggled);
    connect(ui_.singleUpSpin, &QSpinBox::editingFinished, this, &DetailsDialog::onSpinBoxEditingFinished);
}

// ---

void DetailsDialog::initTrackerTab()
{
    auto deleter = [](QObject* o)
    {
        o->deleteLater();
    };

    // NOLINTNEXTLINE(modernize-make-shared) no custom deleters in make_shared
    tracker_model_.reset(new TrackerModel, deleter);
    // NOLINTNEXTLINE(modernize-make-shared) no custom deleters in make_shared
    tracker_filter_.reset(new TrackerModelFilter, deleter);
    tracker_filter_->setSourceModel(tracker_model_.get());
    // NOLINTNEXTLINE(modernize-make-shared) no custom deleters in make_shared
    tracker_delegate_.reset(new TrackerDelegate, deleter);

    ui_.trackersView->setModel(tracker_filter_.get());
    ui_.trackersView->setItemDelegate(tracker_delegate_.get());

    auto const& icons = IconCache::get();
    ui_.addTrackerButton->setIcon(icons.getThemeIcon(QStringLiteral("list-add"), QStyle::SP_DialogOpenButton));
    ui_.editTrackersButton->setIcon(icons.getThemeIcon(QStringLiteral("document-properties"), QStyle::SP_DesktopIcon));
    ui_.removeTrackerButton->setIcon(icons.getThemeIcon(QStringLiteral("list-remove"), QStyle::SP_TrashIcon));

    ui_.showTrackerScrapesCheck->setChecked(prefs_.getBool(Prefs::SHOW_TRACKER_SCRAPES));
    ui_.showBackupTrackersCheck->setChecked(prefs_.getBool(Prefs::SHOW_BACKUP_TRACKERS));

    connect(ui_.addTrackerButton, &QAbstractButton::clicked, this, &DetailsDialog::onAddTrackerClicked);
    connect(ui_.editTrackersButton, &QAbstractButton::clicked, this, &DetailsDialog::onEditTrackersClicked);
    connect(ui_.removeTrackerButton, &QAbstractButton::clicked, this, &DetailsDialog::onRemoveTrackerClicked);
    connect(ui_.showBackupTrackersCheck, &QAbstractButton::clicked, this, &DetailsDialog::onShowBackupTrackersToggled);
    connect(ui_.showTrackerScrapesCheck, &QAbstractButton::clicked, this, &DetailsDialog::onShowTrackerScrapesToggled);
    connect(
        ui_.trackersView->selectionModel(),
        &QItemSelectionModel::selectionChanged,
        this,
        &DetailsDialog::onTrackerSelectionChanged);

    onTrackerSelectionChanged();
}

// ---

void DetailsDialog::initPeersTab()
{
    auto const speed_width_str = Speed{ 1024U, Speed::Units::MByps }.to_qstring();

    ui_.peersView->setHeaderLabels({ QString{}, tr("Up"), tr("Down"), tr("%"), tr("Status"), tr("Address"), tr("Client") });
    ui_.peersView->sortByColumn(COL_ADDRESS, Qt::AscendingOrder);

    ui_.peersView->setColumnWidth(COL_LOCK, 20);
    ui_.peersView->setColumnWidth(COL_UP, measureViewItem(ui_.peersView, COL_UP, speed_width_str));
    ui_.peersView->setColumnWidth(COL_DOWN, measureViewItem(ui_.peersView, COL_DOWN, speed_width_str));
    ui_.peersView->setColumnWidth(COL_PERCENT, measureViewItem(ui_.peersView, COL_PERCENT, QStringLiteral("100%")));
    ui_.peersView->setColumnWidth(COL_STATUS, measureViewItem(ui_.peersView, COL_STATUS, QStringLiteral("ODUK?EXI")));
    ui_.peersView->setColumnWidth(COL_ADDRESS, measureViewItem(ui_.peersView, COL_ADDRESS, QStringLiteral("888.888.888.888")));
}

// ---

void DetailsDialog::initFilesTab() const
{
    connect(ui_.filesView, &FileTreeView::openRequested, this, &DetailsDialog::onOpenRequested);
    connect(ui_.filesView, &FileTreeView::pathEdited, this, &DetailsDialog::onPathEdited);
    connect(ui_.filesView, &FileTreeView::priorityChanged, this, &DetailsDialog::onFilePriorityChanged);
    connect(ui_.filesView, &FileTreeView::wantedChanged, this, &DetailsDialog::onFileWantedChanged);
}

void DetailsDialog::onFilePriorityChanged(file_indices_t const& indices, int priority)
{
    torrentSet(priorityKey(priority), std::vector<int>{ std::begin(indices), std::end(indices) });
}

void DetailsDialog::onFileWantedChanged(file_indices_t const& indices, bool wanted)
{
    tr_quark const key = wanted ? TR_KEY_files_wanted_kebab : TR_KEY_files_unwanted_kebab;
    torrentSet(key, std::vector<int>{ std::begin(indices), std::end(indices) });
}

void DetailsDialog::onPathEdited(QString const& old_path, QString const& new_name)
{
    session_.torrentRenamePath(ids_, old_path, new_name);
}

void DetailsDialog::onOpenRequested(QString const& path) const
{
    if (!session_.isLocal())
    {
        return;
    }

    for (int const id : ids_)
    {
        Torrent const* const tor = model_.getTorrentFromId(id);

        if (tor == nullptr)
        {
            continue;
        }

        auto const local_file_path = tor->getPath() + QLatin1Char('/') + path;

        if (!QFile::exists(local_file_path))
        {
            continue;
        }

        if (QDesktopServices::openUrl(QUrl::fromLocalFile(local_file_path)))
        {
            break;
        }
    }
}

#include "DetailsDialog.moc"
