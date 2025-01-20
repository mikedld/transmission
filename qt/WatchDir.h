// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <memory>
#include <set>

#include <QObject>
#include <QFileSystemWatcher>
#include <QString>

class TorrentModel;

class WatchDir : public QObject
{
    Q_OBJECT

public:
    explicit WatchDir(TorrentModel const&);
    WatchDir(WatchDir&&) = delete;
    WatchDir(WatchDir const&) = delete;
    WatchDir& operator=(WatchDir&&) = delete;
    WatchDir& operator=(WatchDir const&) = delete;

    void setPath(QString const& path, bool is_enabled);

signals:
    void torrentFileAdded(QString const& filename);

private slots:
    void watcherActivated(QString const& path);
    void onTimeout();

    void rescanAllWatchedDirectories();

private:
    enum class AddResult
    {
        Success,
        Duplicate,
        Error
    };

    [[nodiscard]] AddResult metainfoTest(QString const& filename) const;

    TorrentModel const& model_;

    std::set<QString> watch_dir_files_;
    std::unique_ptr<QFileSystemWatcher> watcher_;
};
