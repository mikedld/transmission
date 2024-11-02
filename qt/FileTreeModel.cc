// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>

#include <small/map.hpp>

#include <libtransmission/transmission.h> // priorities

#include <QAbstractItemModel>
#include <QMutableListIterator>

#include "FileTreeItem.h"
#include "FileTreeModel.h"

namespace
{

class PathIteratorBase
{
protected:
    PathIteratorBase(QString const& path, int const slash_index)
        : path_{ path }
        , slash_index_{ slash_index }
    {
        token_.reserve(path.size() / 2);
    }

    QString const& path_;
    QString token_;
    int slash_index_;

    static QChar const SlashChar;
};

QChar const PathIteratorBase::SlashChar = QLatin1Char('/');

class ForwardPathIterator : public PathIteratorBase
{
public:
    explicit ForwardPathIterator(QString const& path)
        : PathIteratorBase(path, path.size() - 1)
    {
    }

    [[nodiscard]] bool hasNext() const
    {
        return slash_index_ > -1;
    }

    QString const& next()
    {
        int const new_slash_index = path_.lastIndexOf(SlashChar, slash_index_);
        token_.truncate(0);
        token_.append(&path_.data()[new_slash_index + 1], slash_index_ - new_slash_index);
        slash_index_ = new_slash_index - 1;
        return token_;
    }
};

class BackwardPathIterator : public PathIteratorBase
{
public:
    explicit BackwardPathIterator(QString const& path)
        : PathIteratorBase{ path, 0 }
    {
    }

    [[nodiscard]] bool hasNext() const
    {
        return slash_index_ < path_.size();
    }

    QString const& next()
    {
        int new_slash_index = path_.indexOf(SlashChar, slash_index_);

        if (new_slash_index == -1)
        {
            new_slash_index = path_.size();
        }

        token_.truncate(0);
        token_.append(&path_.data()[slash_index_], new_slash_index - slash_index_);
        slash_index_ = new_slash_index + 1;
        return token_;
    }
};

} // namespace

FileTreeModel::FileTreeModel(QObject* parent, bool is_editable)
    : QAbstractItemModel{ parent }
    , root_item_{ std::make_unique<FileTreeItem>() }
    , is_editable_{ is_editable }
{
}

FileTreeModel::~FileTreeModel()
{
    clear();
}

void FileTreeModel::setEditable(bool is_editable)
{
    is_editable_ = is_editable;
}

FileTreeItem* FileTreeModel::itemFromIndex(QModelIndex const& index) const
{
    if (!index.isValid())
    {
        return nullptr;
    }

    assert(index.model() == this);
    return static_cast<FileTreeItem*>(index.internalPointer());
}

QModelIndexList FileTreeModel::getOrphanIndices(QModelIndexList const& indices) const
{
    QModelIndexList orphan_indices = indices;

    std::sort(orphan_indices.begin(), orphan_indices.end());

    for (QMutableListIterator<QModelIndex> it(orphan_indices); it.hasNext();)
    {
        QModelIndex walk = it.next();

        for (;;)
        {
            walk = parent(walk, walk.column());

            if (!walk.isValid())
            {
                break;
            }

            if (std::binary_search(orphan_indices.begin(), orphan_indices.end(), walk))
            {
                it.remove();
                break;
            }
        }
    }

    return orphan_indices;
}

QVariant FileTreeModel::data(QModelIndex const& index, int role) const
{
    if (index.isValid())
    {
        return itemFromIndex(index)->data(index.column(), role);
    }

    return {};
}

Qt::ItemFlags FileTreeModel::flags(QModelIndex const& index) const
{
    int i(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    if (is_editable_ && index.column() == COL_NAME)
    {
        i |= Qt::ItemIsEditable;
    }

    if (index.column() == COL_WANTED)
    {
        i |= Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate;
    }

    return { i };
}

bool FileTreeModel::setData(QModelIndex const& index, QVariant const& value, int role)
{
    if (role == Qt::EditRole)
    {
        FileTreeItem const* item = itemFromIndex(index);

        emit pathEdited(item->path(), value.toString());
    }

    return false; // don't update the view until the session confirms the change
}

QVariant FileTreeModel::headerData(int column, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
    {
        switch (column)
        {
        case COL_NAME:
            return tr("File");

        case COL_SIZE:
            return tr("Size");

        case COL_PROGRESS:
            return tr("Progress");

        case COL_WANTED:
            return tr("Download");

        case COL_PRIORITY:
            return tr("Priority");

        default:
            break;
        }
    }

    return {};
}

QModelIndex FileTreeModel::index(int row, int column, QModelIndex const& parent) const
{
    if (hasIndex(row, column, parent))
    {
        auto* const parent_item = parent.isValid() ? itemFromIndex(parent) : root_item_.get();

        if (auto* const child_item = parent_item->child(row); child_item != nullptr)
        {
            return createIndex(row, column, child_item);
        }
    }

    return {};
}

QModelIndex FileTreeModel::parent(QModelIndex const& child) const
{
    return parent(child, 0); // QAbstractItemModel::parent() wants col 0
}

QModelIndex FileTreeModel::parent(QModelIndex const& child, int column) const
{
    QModelIndex parent;

    if (child.isValid())
    {
        parent = indexOf(itemFromIndex(child)->parent(), column);
    }

    return parent;
}

int FileTreeModel::rowCount(QModelIndex const& parent) const
{
    FileTreeItem const* parent_item = parent.isValid() ? itemFromIndex(parent) : root_item_.get();

    return parent_item->childCount();
}

int FileTreeModel::columnCount(QModelIndex const& parent) const
{
    Q_UNUSED(parent)

    return NUM_COLUMNS;
}

QModelIndex FileTreeModel::indexOf(FileTreeItem* item, int column) const
{
    if (item == nullptr || item == root_item_.get())
    {
        return {};
    }

    return createIndex(item->row(), column, item);
}

void FileTreeModel::clearSubtree(QModelIndex const& top)
{
    size_t i = rowCount(top);

    while (i > 0)
    {
        clearSubtree(index(--i, 0, top));
    }

    FileTreeItem* const item = itemFromIndex(top);

    if (item == nullptr)
    {
        return;
    }

    if (auto const idx = item->fileIndex(); idx != -1)
    {
        if (auto const iter = index_cache_.find(idx); iter != std::end(index_cache_))
        {
            index_cache_.erase(iter);
        }
    }

    delete item;
}

void FileTreeModel::clear()
{
    beginResetModel();
    clearSubtree(QModelIndex{});
    root_item_ = std::make_unique<FileTreeItem>();
    endResetModel();

    assert(std::empty(index_cache_));
}

FileTreeItem* FileTreeModel::findItemForFileIndex(int file_index) const
{
    auto iter = index_cache_.find(file_index);
    return iter == std::end(index_cache_) ? nullptr : iter->second;
}

void FileTreeModel::addFile(
    int file_index,
    QString const& filename,
    bool wanted,
    int priority,
    uint64_t total_size,
    uint64_t have,
    bool update_fields)
{
    auto* item = findItemForFileIndex(file_index);

    if (item != nullptr) // this file is already in the tree, we've added this
    {
        QModelIndex index_with_changed_parents;
        ForwardPathIterator filename_it(filename);

        while (filename_it.hasNext())
        {
            auto const& token = filename_it.next();
            auto const& [first_col, last_col] = item->update(token, wanted, priority, have, update_fields);

            if (first_col >= 0)
            {
                emit dataChanged(indexOf(item, first_col), indexOf(item, last_col));

                if (!index_with_changed_parents.isValid() && first_col <= COL_PRIORITY && last_col >= COL_SIZE)
                {
                    index_with_changed_parents = indexOf(item, 0);
                }
            }

            item = item->parent();
        }

        assert(item == root_item_.get());

        if (index_with_changed_parents.isValid())
        {
            emitParentsChanged(index_with_changed_parents, COL_SIZE, COL_PRIORITY);
        }
    }
    else // we haven't build the FileTreeItems for these tokens yet
    {
        bool added = false;

        item = root_item_.get();
        BackwardPathIterator filename_it(filename);

        while (filename_it.hasNext())
        {
            QString const& token = filename_it.next();
            FileTreeItem* child(item->child(token));

            if (child == nullptr)
            {
                added = true;
                auto const parent_index = indexOf(item, 0);
                auto const n = item->childCount();

                beginInsertRows(parent_index, n, n);

                if (!filename_it.hasNext())
                {
                    child = new FileTreeItem{ token, file_index, total_size };
                }
                else
                {
                    child = new FileTreeItem{ token };
                }

                item->appendChild(child);
                endInsertRows();
            }

            item = child;
        }

        if (item != root_item_.get())
        {
            assert(item->fileIndex() == file_index);
            assert(item->totalSize() == total_size);

            index_cache_[file_index] = item;

            auto const [first_col, last_col] = item->update(item->name(), wanted, priority, have, added || update_fields);

            if (first_col >= 0)
            {
                emit dataChanged(indexOf(item, first_col), indexOf(item, last_col));
            }
        }
    }
}

void FileTreeModel::emitParentsChanged(
    QModelIndex const& index,
    int first_column,
    int last_column,
    std::set<QModelIndex>* visited_parent_indices)
{
    assert(first_column <= last_column);

    QModelIndex walk = index;

    for (;;)
    {
        walk = parent(walk, first_column);

        if (!walk.isValid())
        {
            break;
        }

        if (visited_parent_indices != nullptr)
        {
            if (visited_parent_indices->count(walk) != 0U)
            {
                break;
            }

            visited_parent_indices->insert(walk);
        }

        emit dataChanged(walk, walk.sibling(walk.row(), last_column));
    }
}

void FileTreeModel::emitSubtreeChanged(QModelIndex const& idx, int first_column, int last_column)
{
    assert(first_column <= last_column);

    int const child_count = rowCount(idx);

    if (child_count == 0)
    {
        return;
    }

    // tell everyone that this item changed
    emit dataChanged(index(0, first_column, idx), index(child_count - 1, last_column, idx));

    // walk the subitems
    for (int i = 0; i < child_count; ++i)
    {
        emitSubtreeChanged(index(i, 0, idx), first_column, last_column);
    }
}

void FileTreeModel::twiddleWanted(QModelIndexList const& indices)
{
    auto wanted_indices = small::max_size_map<bool, QModelIndexList, 2U>{};

    for (QModelIndex const& i : getOrphanIndices(indices))
    {
        FileTreeItem const* const item = itemFromIndex(i);
        wanted_indices[item->isSubtreeWanted() != Qt::Checked] << i;
    }

    for (int i = 0; i <= 1; ++i)
    {
        if (wanted_indices.count(i) != 0)
        {
            setWanted(wanted_indices[i], i != 0);
        }
    }
}

void FileTreeModel::twiddlePriority(QModelIndexList const& indices)
{
    auto priority_indices = small::max_size_map<int, QModelIndexList, 8U>{};

    for (QModelIndex const& i : getOrphanIndices(indices))
    {
        FileTreeItem const* const item = itemFromIndex(i);
        int priority = item->priority();

        // ... -> normal -> high -> low -> normal -> ...; mixed -> normal
        if (priority == FileTreeItem::Normal)
        {
            priority = TR_PRI_HIGH;
        }
        else if (priority == FileTreeItem::High)
        {
            priority = TR_PRI_LOW;
        }
        else
        {
            priority = TR_PRI_NORMAL;
        }

        priority_indices[priority] << i;
    }

    for (int i = TR_PRI_LOW; i <= TR_PRI_HIGH; ++i)
    {
        if (priority_indices.contains(i))
        {
            setPriority(priority_indices[i], i);
        }
    }
}

void FileTreeModel::setWanted(QModelIndexList const& indices, bool wanted)
{
    if (indices.isEmpty())
    {
        return;
    }

    QModelIndexList const orphan_indices = getOrphanIndices(indices);

    auto file_ids = file_indices_t{};

    for (QModelIndex const& i : orphan_indices)
    {
        FileTreeItem* const item = itemFromIndex(i);
        item->setSubtreeWanted(wanted, file_ids);

        emit dataChanged(i, i);
        emitSubtreeChanged(i, COL_WANTED, COL_WANTED);
    }

    // emit parent changes separately to avoid multiple updates for same items
    auto parent_indices = std::set<QModelIndex>{};

    for (QModelIndex const& i : orphan_indices)
    {
        emitParentsChanged(i, COL_SIZE, COL_WANTED, &parent_indices);
    }

    if (!std::empty(file_ids))
    {
        emit wantedChanged(file_ids, wanted);
    }
}

void FileTreeModel::setPriority(QModelIndexList const& indices, int priority)
{
    if (indices.isEmpty())
    {
        return;
    }

    QModelIndexList const orphan_indices = getOrphanIndices(indices);

    auto file_ids = file_indices_t{};

    for (QModelIndex const& i : orphan_indices)
    {
        FileTreeItem* const item = itemFromIndex(i);
        item->setSubtreePriority(priority, file_ids);

        emit dataChanged(i, i);
        emitSubtreeChanged(i, COL_PRIORITY, COL_PRIORITY);
    }

    // emit parent changes separately to avoid multiple updates for same items
    auto parent_indices = std::set<QModelIndex>{};
    for (QModelIndex const& i : orphan_indices)
    {
        emitParentsChanged(i, COL_PRIORITY, COL_PRIORITY, &parent_indices);
    }

    if (!std::empty(file_ids))
    {
        emit priorityChanged(file_ids, priority);
    }
}

bool FileTreeModel::openFile(QModelIndex const& index)
{
    if (!index.isValid())
    {
        return false;
    }

    FileTreeItem const* const item = itemFromIndex(index);

    if (item->fileIndex() < 0 || !item->isComplete())
    {
        return false;
    }

    emit openRequested(item->path());
    return true;
}
