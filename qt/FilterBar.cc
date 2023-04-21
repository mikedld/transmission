// This file Copyright © 2012-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "FilterBar.h"

#include <cstdint> // uint64_t
#include <map>
#include <utility>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStandardItemModel>

#include "Application.h"
#include "FaviconCache.h"
#include "FilterBarComboBox.h"
#include "Filters.h"
#include "Prefs.h"
#include "Torrent.h"
#include "TorrentFilter.h"
#include "TorrentModel.h"
#include "FilterUIDelegate.h"

enum
{
    ACTIVITY_ROLE = FilterUI::UserRole,
    TRACKER_ROLE
};

QComboBox* FilterBar::createActivityUI()
{
    auto* c = new FilterBarComboBox(this);
    auto* delegate = new FilterUIDelegate(this, c);
    c->setItemDelegate(delegate);

    auto* model = FilterUI::createActivityModel(this);

    c->setModel(model);
    return c;
}

namespace
{

QString getCountString(size_t n)
{
    return QStringLiteral("%L1").arg(n);
}

Torrent::fields_t constexpr TrackerFields = {
    static_cast<uint64_t>(1) << Torrent::TRACKER_STATS,
};

auto constexpr ActivityFields = FilterMode::TorrentFields;

} // namespace

QComboBox* FilterBar::createTrackerUI(QStandardItemModel* model)
{
    auto* c = new FilterBarComboBox(this);
    auto* delegate = new FilterUIDelegate(this, c);
    c->setItemDelegate(delegate);

    auto* row = new QStandardItem(tr("All"));
    row->setData(QString(), TRACKER_ROLE);
    int const count = torrents_.rowCount();
    row->setData(count, CountRole);
    row->setData(getCountString(static_cast<size_t>(count)), CountStringRole);
    model->appendRow(row);

    model->appendRow(new QStandardItem); // separator
    FilterUIDelegate::setSeparator(model, model->index(1, 0));

    c->setModel(model);
    return c;
}

FilterBar::FilterBar(Prefs& prefs, TorrentModel const& torrents, TorrentFilter const& filter, QWidget* parent)
    : FilterUI(prefs, torrents, filter, parent)
{
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(1, 1, 1, 1);

    h->addWidget(btn_);
    connect(btn_, &IconToolButton::clicked, this, &FilterBar::toggleUI);

    count_label_ = new QLabel(tr("Show:"), this);
    h->addWidget(count_label_);

    h->addWidget(activity_ui_);

    tracker_ui_ = createTrackerUI(tracker_model_);
    h->addWidget(tracker_ui_);

    h->addStretch();

    line_edit_->setClearButtonEnabled(true);
    line_edit_->setPlaceholderText(tr("Search…"));
    line_edit_->setMaximumWidth(250);
    h->addWidget(line_edit_, 1);
    connect(line_edit_, &QLineEdit::textChanged, this, &FilterBar::onTextChanged);

    // listen for changes from the other players
    connect(activity_ui_, qOverload<int>(&QComboBox::currentIndexChanged), this, &FilterBar::onActivityIndexChanged);
    connect(tracker_ui_, qOverload<int>(&QComboBox::currentIndexChanged), this, &FilterBar::onTrackerIndexChanged);

    recountAllSoon();
    is_bootstrapping_ = false; // NOLINT cppcoreguidelines-prefer-member-initializer

    // initialize our state
    for (int const key : { Prefs::FILTER_MODE, Prefs::FILTER_TRACKERS })
    {
        refreshPref(key);
    }
}

void FilterBar::clear()
{
    activity_ui_->setCurrentIndex(0);
    tracker_ui_->setCurrentIndex(0);
    line_edit_->clear();
}

void FilterBar::refreshPref(int key)
{
    switch (key)
    {
    case Prefs::FILTER_MODE:
        {
            auto const modes = prefs_.get<QList<FilterMode>>(key);

            if (modes.count() == 1) {
                QAbstractItemModel const* const model = activity_ui_->model();
                QModelIndexList indices;
                indices = model->match(model->index(0, 0), ACTIVITY_ROLE, modes.first().mode());
                activity_ui_->setCurrentIndex(indices.isEmpty() ? 0 : indices.first().row());
            }
            break;
        }

    case Prefs::FILTER_TRACKERS:
        {
            auto const display_names = prefs_.get<QStringList>(key);

            if (display_names.count() == 1) {
                const auto& display_name = display_names.first();
                auto rows = tracker_model_->findItems(display_name);
                if (!rows.isEmpty())
                {
                    tracker_ui_->setCurrentIndex(rows.front()->row());
                }
                else // hm, we don't seem to have this tracker anymore...
                {
                    if (display_name.isEmpty()) // set combobox to "All" if it is selected in FilterView
                    {
                        tracker_ui_->setCurrentIndex(0);
                    }

                    bool const is_bootstrapping = tracker_model_->rowCount() <= 2;

                    if (!is_bootstrapping)
                    {
                        prefs_.set(key, QStringList());
                    }
                }
            }
            break;
        }
    }
}

void FilterBar::onTrackerIndexChanged(int i)
{
    if (!is_bootstrapping_)
    {
        auto const display_name = tracker_ui_->itemData(i, TRACKER_ROLE).toString();
        auto *const display_names = new QStringList;
        display_names->append(display_name);
        prefs_.set(Prefs::FILTER_TRACKERS, *display_names);
    }
}

void FilterBar::onActivityIndexChanged(int i)
{
    if (!is_bootstrapping_)
    {
        auto const mode = FilterMode(activity_ui_->itemData(i, ACTIVITY_ROLE).toInt());
        auto *const modes = new QList<FilterMode>;
        modes->append(mode);
        prefs_.set(Prefs::FILTER_MODE, *modes);
    }
}

void FilterBar::recount()
{
    QAbstractItemModel* model = activity_ui_->model();

    decltype(pending_) pending = {};
    std::swap(pending_, pending);

    if (pending[ACTIVITY])
    {
        auto const torrents_per_mode = filter_.countTorrentsPerMode();

        for (int row = 0, n = model->rowCount(); row < n; ++row)
        {
            auto const index = model->index(row, 0);
            auto const mode = index.data(ACTIVITY_ROLE).toInt();
            auto const count = torrents_per_mode[mode];
            model->setData(index, count, CountRole);
            model->setData(index, getCountString(static_cast<size_t>(count)), CountStringRole);
        }
    }

    if (pending[TRACKERS])
    {
        refreshTrackers();
    }
}
