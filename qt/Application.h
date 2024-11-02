// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <ctime>
#include <memory>
#include <unordered_set>

#include <QApplication>
#include <QRegularExpression>
#include <QTimer>
#include <QTranslator>
#include <QWeakPointer>

#include <libtransmission/tr-macros.h>
#include <libtransmission/favicon-cache.h>

#include "AddData.h"
#include "Typedefs.h"
#include "Utils.h" // std::hash<QString>

class AddData;
class MainWindow;
class Prefs;
class Session;
class Torrent;
class TorrentModel;
class WatchDir;

class Application : public QApplication
{
    Q_OBJECT
    TR_DISABLE_COPY_MOVE(Application)

public:
    Application(
        std::unique_ptr<Prefs> prefs,
        bool minimized,
        QString const& config_dir,
        QStringList const& filenames,
        int& argc,
        char** argv);
    ~Application() override;

    void raise() const;
    bool notifyApp(QString const& title, QString const& body, QStringList const& actions = {}) const;

    QString const& intern(QString const& in)
    {
        return *interned_strings_.insert(in).first;
    }

    [[nodiscard]] QPixmap find_favicon(QString const& sitename) const
    {
        auto const key = sitename.toStdString();
        auto const* const icon = favicon_cache_.find(key);
        return icon != nullptr ? *icon : QPixmap{};
    }

    void load_favicon(QString const& url)
    {
        auto weak_self = QPointer<Application>{ this };

        favicon_cache_.load(
            url.toStdString(),
            [weak_self = std::move(weak_self)](QPixmap const* /*favicon_or_nullptr*/)
            {
                if (!weak_self.isNull())
                {
                    weak_self.data()->faviconsChanged();
                }
            });
    }

signals:
    void faviconsChanged();

public slots:
    void addTorrent(AddData) const;
    void addWatchdirTorrent(QString const& filename) const;

private slots:
    void consentGiven(int result) const;
    void onSessionSourceChanged() const;
    void onTorrentsAdded(torrent_ids_t const& torrent_ids) const;
    void onTorrentsCompleted(torrent_ids_t const& torrent_ids) const;
    void onTorrentsEdited(torrent_ids_t const& torrent_ids) const;
    void onTorrentsNeedInfo(torrent_ids_t const& torrent_ids) const;
    void refreshPref(int key) const;
    void refreshTorrents();
    void saveGeometry() const;
#ifdef QT_DBUS_LIB
    void onNotificationActionInvoked(quint32 notification_id, QString action_key);
#endif

private:
    void maybeUpdateBlocklist() const;
    void loadTranslations();
    void initUnits();
    QStringList getNames(torrent_ids_t const& ids) const;
    void quitLater() const;
    void notifyTorrentAdded(Torrent const*) const;

    std::unordered_set<QString> interned_strings_;

    std::unique_ptr<Prefs> prefs_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<TorrentModel> model_;
    std::unique_ptr<MainWindow> window_;
    std::unique_ptr<WatchDir> watch_dir_;
    QTimer model_timer_;
    QTimer stats_timer_;
    QTimer session_timer_;
    time_t last_full_update_time_ = {};
    QTranslator qt_translator_;
    QTranslator app_translator_;

    FaviconCache<QPixmap> favicon_cache_;

#ifdef QT_DBUS_LIB
    QString const fdo_notifications_service_name_ = QStringLiteral("org.freedesktop.Notifications");
    QString const fdo_notifications_path_ = QStringLiteral("/org/freedesktop/Notifications");
    QString const fdo_notifications_interface_name_ = QStringLiteral("org.freedesktop.Notifications");
#endif

    QRegularExpression const start_now_regex_{ QStringLiteral(R"rgx(start-now\((\d+)\))rgx") };
};

#define trApp dynamic_cast<Application*>(Application::instance())
