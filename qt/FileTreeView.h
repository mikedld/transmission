// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QTreeView>

#include "Torrent.h" // FileList
#include "Typedefs.h" // file_indices_t

class QAction;
class QMenu;
class QSortFilterProxyModel;

class FileTreeDelegate;
class FileTreeModel;

class FileTreeView : public QTreeView
{
    Q_OBJECT

public:
    FileTreeView(QWidget* parent = nullptr, bool editable = true);
    FileTreeView(FileTreeView&&) = delete;
    FileTreeView(FileTreeView const&) = delete;
    FileTreeView& operator=(FileTreeView&&) = delete;
    FileTreeView& operator=(FileTreeView const&) = delete;

    void clear();
    void update(FileList const& files, bool update_fields = true);

    void setEditable(bool editable);

signals:
    void priorityChanged(file_indices_t const& file_indices, int priority);
    void wantedChanged(file_indices_t const& file_indices, bool wanted);
    void pathEdited(QString const& old_path, QString const& new_name);
    void openRequested(QString const& path);

protected:
    // QWidget
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

    // QAbstractItemView
    bool edit(QModelIndex const& index, EditTrigger trigger, QEvent* event) override;

private slots:
    void onClicked(QModelIndex const& index);

    void checkSelectedItems();
    void uncheckSelectedItems();
    void onlyCheckSelectedItems();
    void setSelectedItemsPriority();
    bool openSelectedItem();
    void renameSelectedItem();

    void refreshContextMenuActionsSensitivity();

private:
    void initContextMenu();
    [[nodiscard]] QModelIndexList selectedSourceRows(int column = 0) const;

    static Qt::CheckState getCumulativeCheckState(QModelIndexList const& indices);

    FileTreeModel* model_ = {};
    QSortFilterProxyModel* proxy_ = {};
    FileTreeDelegate* delegate_ = {};

    QMenu* context_menu_ = {};
    QMenu* priority_menu_ = {};
    QAction* check_selected_action_ = {};
    QAction* uncheck_selected_action_ = {};
    QAction* only_check_selected_action_ = {};
    QAction* high_priority_action_ = {};
    QAction* normal_priority_action_ = {};
    QAction* low_priority_action_ = {};
    QAction* open_action_ = {};
    QAction* rename_action_ = {};
};
