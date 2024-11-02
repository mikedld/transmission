// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cassert>
#include <queue>
#include <set>

#include <QHeaderView>
#include <QMenu>
#include <QResizeEvent>
#include <QSortFilterProxyModel>

#include <libtransmission/transmission.h> // priorities

#include "FileTreeDelegate.h"
#include "FileTreeItem.h"
#include "FileTreeModel.h"
#include "FileTreeView.h"
#include "Formatter.h"
#include "Utils.h"

using namespace libtransmission::Values;

namespace
{

char const* const PriorityKey = "priority";

}

FileTreeView::FileTreeView(QWidget* parent, bool is_editable)
    : QTreeView{ parent }
    , model_{ new FileTreeModel{ this, is_editable } }
    , proxy_{ new QSortFilterProxyModel{ this } }
    , delegate_{ new FileTreeDelegate{ this } }
{
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(FileTreeModel::SortRole);
    proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);

    setModel(proxy_);
    setItemDelegate(delegate_);
    sortByColumn(FileTreeModel::COL_NAME, Qt::AscendingOrder);

    connect(this, &QAbstractItemView::clicked, this, &FileTreeView::onClicked);

    connect(model_, &FileTreeModel::openRequested, this, &FileTreeView::openRequested);
    connect(model_, &FileTreeModel::pathEdited, this, &FileTreeView::pathEdited);
    connect(model_, &FileTreeModel::priorityChanged, this, &FileTreeView::priorityChanged);
    connect(model_, &FileTreeModel::wantedChanged, this, &FileTreeView::wantedChanged);
}

void FileTreeView::onClicked(QModelIndex const& proxy_index)
{
    QModelIndex const model_index = proxy_->mapToSource(proxy_index);

    if (model_index.column() == FileTreeModel::COL_WANTED)
    {
        model_->twiddleWanted(QModelIndexList{} << model_index);
    }
    else if (model_index.column() == FileTreeModel::COL_PRIORITY)
    {
        model_->twiddlePriority(QModelIndexList{} << model_index);
    }
}

void FileTreeView::resizeEvent(QResizeEvent* event)
{
    QTreeView::resizeEvent(event);

    // this is kind of a hack to get the last four columns be the
    // right size, and to have the filename column use whatever
    // space is left over...

    int left = event->size().width() - 1;

    for (int column = 0; column < FileTreeModel::NUM_COLUMNS; ++column)
    {
        if (column == FileTreeModel::COL_NAME)
        {
            continue;
        }

        int min_width = 0;

        QStringList item_texts;

        switch (column)
        {
        case FileTreeModel::COL_SIZE:
            item_texts << QString::fromStdString(Memory{ 999.9, Memory::Units::Bytes }.to_string())
                       << QString::fromStdString(Memory{ 999.9, Memory::Units::KBytes }.to_string())
                       << QString::fromStdString(Memory{ 999.9, Memory::Units::MBytes }.to_string())
                       << QString::fromStdString(Memory{ 999.9, Memory::Units::GBytes }.to_string())
                       << QString::fromStdString(Memory{ 999.9, Memory::Units::TBytes }.to_string());
            break;

        case FileTreeModel::COL_PROGRESS:
            item_texts << QStringLiteral("  100%  ");
            break;

        case FileTreeModel::COL_WANTED:
            min_width = 20;
            break;

        case FileTreeModel::COL_PRIORITY:
            item_texts << FileTreeItem::tr("Low") << FileTreeItem::tr("Normal") << FileTreeItem::tr("High")
                       << FileTreeItem::tr("Mixed");
            break;

        default:
            break;
        }

        int item_width = 0;

        for (QString const& item_text : item_texts)
        {
            item_width = std::max(item_width, Utils::measureViewItem(this, item_text));
        }

        QString const header_text = model_->headerData(column, Qt::Horizontal).toString();
        int const header_width = Utils::measureHeaderItem(this->header(), header_text);

        int const width = std::max({ min_width, item_width, header_width });
        setColumnWidth(column, width);

        left -= width;
    }

    setColumnWidth(FileTreeModel::COL_NAME, std::max(left, 0));
}

void FileTreeView::keyPressEvent(QKeyEvent* event)
{
    if ((state() != EditingState) && (event->key() == Qt::Key_Space))
    {
        // handle using the keyboard to toggle the
        // wanted/unwanted state or the file priority

        Qt::KeyboardModifiers const modifiers = event->modifiers();

        if (modifiers == Qt::NoModifier)
        {
            model_->twiddleWanted(selectedSourceRows());
            return;
        }

        if (modifiers == Qt::ShiftModifier)
        {
            model_->twiddlePriority(selectedSourceRows());
            return;
        }
    }

    QTreeView::keyPressEvent(event);
}

void FileTreeView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (auto const index = currentIndex();
        !index.isValid() || index.column() == FileTreeModel::COL_WANTED || index.column() == FileTreeModel::COL_PRIORITY)
    {
        return;
    }

    if (openSelectedItem())
    {
        return;
    }

    QTreeView::mouseDoubleClickEvent(event);
}

void FileTreeView::contextMenuEvent(QContextMenuEvent* event)
{
    if (auto const root_index = model_->index(0, 0); !root_index.isValid())
    {
        return;
    }

    if (context_menu_ == nullptr)
    {
        initContextMenu();
    }

    context_menu_->popup(event->globalPos());
}

void FileTreeView::update(FileList const& files, bool update_fields)
{
    bool const model_was_empty = proxy_->rowCount() == 0;

    for (TorrentFile const& file : files)
    {
        model_->addFile(file.index, file.filename, file.wanted, file.priority, file.size, file.have, update_fields);
    }

    if (model_was_empty)
    {
        expand(proxy_->index(0, 0));
    }

    proxy_->sort(header()->sortIndicatorSection(), header()->sortIndicatorOrder());
}

void FileTreeView::clear()
{
    model_->clear();
}

void FileTreeView::setEditable(bool editable)
{
    model_->setEditable(editable);
}

bool FileTreeView::edit(QModelIndex const& index, EditTrigger trigger, QEvent* event)
{
    if (selectionModel()->selectedRows().size() != 1)
    {
        return false;
    }

    QModelIndex const name_index = index.sibling(index.row(), FileTreeModel::COL_NAME);

    if (editTriggers().testFlag(trigger))
    {
        selectionModel()->setCurrentIndex(name_index, QItemSelectionModel::NoUpdate);
    }

    return QTreeView::edit(name_index, trigger, event);
}

void FileTreeView::checkSelectedItems()
{
    model_->setWanted(selectedSourceRows(), true);
}

void FileTreeView::uncheckSelectedItems()
{
    model_->setWanted(selectedSourceRows(), false);
}

void FileTreeView::onlyCheckSelectedItems()
{
    QModelIndex const root_index = model_->index(0, 0);

    if (!root_index.isValid())
    {
        return;
    }

    QModelIndexList wanted_indices = selectedSourceRows();
    model_->setWanted(wanted_indices, true);

    std::sort(wanted_indices.begin(), wanted_indices.end());

    auto wanted_indices_parents = std::set<QModelIndex>{};

    for (QModelIndex const& i : wanted_indices)
    {
        for (QModelIndex p = i.parent(); p.isValid(); p = p.parent())
        {
            wanted_indices_parents.insert(p);
        }
    }

    auto parents_queue = std::queue<QModelIndex>{};
    parents_queue.emplace(root_index);
    QModelIndexList unwanted_indices;

    while (!std::empty(parents_queue))
    {
        auto const parent_index = parents_queue.front();
        parents_queue.pop();

        if (std::binary_search(wanted_indices.begin(), wanted_indices.end(), parent_index))
        {
            continue;
        }

        auto const* parent_model = parent_index.model();

        for (int i = 0, count = model_->rowCount(parent_index); i < count; ++i)
        {
            QModelIndex const child_index = parent_model->index(i, 0, parent_index);
            int const child_check_state = child_index.data(FileTreeModel::WantedRole).toInt();

            if (child_check_state == Qt::Unchecked ||
                std::binary_search(wanted_indices.begin(), wanted_indices.end(), child_index))
            {
                continue;
            }

            if (child_check_state == Qt::Checked && child_index.data(FileTreeModel::FileIndexRole).toInt() >= 0)
            {
                unwanted_indices << child_index;
            }
            else if (wanted_indices_parents.count(child_index) == 0U)
            {
                unwanted_indices << child_index;
            }
            else
            {
                parents_queue.emplace(child_index);
            }
        }
    }

    model_->setWanted(unwanted_indices, false);
}

void FileTreeView::setSelectedItemsPriority()
{
    auto const* action = qobject_cast<QAction const*>(sender());
    assert(action != nullptr);
    model_->setPriority(selectedSourceRows(), action->property(PriorityKey).toInt());
}

bool FileTreeView::openSelectedItem()
{
    return model_->openFile(proxy_->mapToSource(currentIndex()));
}

void FileTreeView::renameSelectedItem()
{
    QTreeView::edit(currentIndex());
}

void FileTreeView::refreshContextMenuActionsSensitivity()
{
    assert(context_menu_ != nullptr);

    QModelIndexList const selected_rows = selectionModel()->selectedRows();
    Qt::CheckState const check_state = getCumulativeCheckState(selected_rows);

    bool const have_selection = !selected_rows.isEmpty();
    bool const have_single_selection = selected_rows.size() == 1;
    bool const have_unchecked = check_state == Qt::Unchecked || check_state == Qt::PartiallyChecked;
    bool const have_checked = check_state == Qt::Checked || check_state == Qt::PartiallyChecked;

    check_selected_action_->setEnabled(have_unchecked);
    uncheck_selected_action_->setEnabled(have_checked);
    only_check_selected_action_->setEnabled(have_selection);
    priority_menu_->setEnabled(have_selection);
    open_action_->setEnabled(
        have_single_selection && selected_rows.first().data(FileTreeModel::FileIndexRole).toInt() >= 0 &&
        selected_rows.first().data(FileTreeModel::CompleteRole).toBool());
    rename_action_->setEnabled(have_single_selection);
}

void FileTreeView::initContextMenu()
{
    context_menu_ = new QMenu{ this };

    check_selected_action_ = context_menu_->addAction(tr("Check Selected"), this, SLOT(checkSelectedItems()));
    uncheck_selected_action_ = context_menu_->addAction(tr("Uncheck Selected"), this, SLOT(uncheckSelectedItems()));
    only_check_selected_action_ = context_menu_->addAction(tr("Only Check Selected"), this, SLOT(onlyCheckSelectedItems()));

    context_menu_->addSeparator();

    priority_menu_ = context_menu_->addMenu(tr("Priority"));
    high_priority_action_ = priority_menu_->addAction(FileTreeItem::tr("High"), this, SLOT(setSelectedItemsPriority()));
    normal_priority_action_ = priority_menu_->addAction(FileTreeItem::tr("Normal"), this, SLOT(setSelectedItemsPriority()));
    low_priority_action_ = priority_menu_->addAction(FileTreeItem::tr("Low"), this, SLOT(setSelectedItemsPriority()));

    high_priority_action_->setProperty(PriorityKey, TR_PRI_HIGH);
    normal_priority_action_->setProperty(PriorityKey, TR_PRI_NORMAL);
    low_priority_action_->setProperty(PriorityKey, TR_PRI_LOW);

    context_menu_->addSeparator();

    open_action_ = context_menu_->addAction(tr("Open"), this, SLOT(openSelectedItem()));
    rename_action_ = context_menu_->addAction(tr("Rename…"), this, SLOT(renameSelectedItem()));

    connect(context_menu_, &QMenu::aboutToShow, this, &FileTreeView::refreshContextMenuActionsSensitivity);
}

QModelIndexList FileTreeView::selectedSourceRows(int column) const
{
    QModelIndexList indices;

    for (QModelIndex const& i : selectionModel()->selectedRows(column))
    {
        indices << proxy_->mapToSource(i);
    }

    return indices;
}

Qt::CheckState FileTreeView::getCumulativeCheckState(QModelIndexList const& indices)
{
    bool have_checked = false;
    bool have_unchecked = false;

    for (QModelIndex const& i : indices)
    {
        switch (i.data(FileTreeModel::WantedRole).toInt())
        {
        case Qt::Checked:
            have_checked = true;
            break;

        case Qt::Unchecked:
            have_unchecked = true;
            break;

        case Qt::PartiallyChecked:
            return Qt::PartiallyChecked;

        default:
            break;
        }

        if (have_checked && have_unchecked)
        {
            return Qt::PartiallyChecked;
        }
    }

    return have_checked ? Qt::Checked : Qt::Unchecked;
}
