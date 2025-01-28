// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef>
#include <ctime>
#include <memory>

#include <QMainWindow>
#include <QNetworkReply>
#include <QPointer>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidgetList>

#include "Filters.h"
#include "Speed.h"
#include "TorrentFilter.h"
#include "Typedefs.h"
#include "ui_MainWindow.h"

class QAction;
class QIcon;
class QMenu;

class AboutDialog;
class AddData;
class DetailsDialog;
class ListViewProxyStyle;
class Prefs;
class PrefsDialog;
class Session;
class SessionDialog;
class StatsDialog;
class TorrentDelegate;
class TorrentDelegateMin;
class TorrentModel;

extern "C"
{
    struct tr_variant;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(Session&, Prefs&, TorrentModel&, bool minimized);
    MainWindow(MainWindow&&) = delete;
    MainWindow(MainWindow const&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow const&) = delete;

    [[nodiscard]] constexpr QSystemTrayIcon& trayIcon() noexcept
    {
        return tray_icon_;
    }

public slots:
    void startAll();
    void startSelected();
    void startSelectedNow();
    void pauseAll();
    void pauseSelected();
    void removeSelected();
    void deleteSelected();
    void verifySelected();
    void queueMoveTop();
    void queueMoveUp();
    void queueMoveDown();
    void queueMoveBottom();
    void reannounceSelected();
    void onNetworkTimer();

    void setToolbarVisible(bool);
    void setFilterbarVisible(bool);
    void setStatusbarVisible(bool);
    void setCompactView(bool);
    void wrongAuthentication();

    void openSession();

protected:
    // QWidget
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    bool event(QEvent*) override;

private slots:
    void addTorrents(QStringList const& filenames);
    void copyMagnetLinkToClipboard();
    void dataReadProgress();
    void dataSendProgress();
    void newTorrent();
    void onNetworkResponse(QNetworkReply::NetworkError code, QString const& message);
    void onRefreshTimer();
    void onSessionSourceChanged();
    void onSetPrefs();
    void onSetPrefs(bool);
    void onSortModeChanged(QAction const* action);
    void onStatsModeChanged(QAction const* action);
    void openAbout();
    void openDonate() const;
    void openFolder();
    void openHelp() const;
    void openPreferences();
    void openProperties();
    void openStats();
    void openTorrent();
    void openURL();
    void refreshPref(int key);
    void refreshSoon(int fields = ~0);
    void removeTorrents(bool const delete_files);
    void setLocation();
    void setSortAscendingPref(bool);
    void toggleSpeedMode();
    void toggleWindows(bool do_show);
    void trayActivated(QSystemTrayIcon::ActivationReason);

private:
    [[nodiscard]] QIcon addEmblem(QIcon icon, QStringList const& emblem_names) const;

    [[nodiscard]] torrent_ids_t getSelectedTorrents(bool with_metadata_only = false) const;
    void updateNetworkIcon();

    QMenu* createOptionsMenu();
    QMenu* createStatsModeMenu();
    void initStatusBar();

    void clearSelection();
    void addTorrent(AddData add_me, bool show_options);

    // QWidget
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

    Session& session_;
    Prefs& prefs_;
    TorrentModel& model_;

    std::shared_ptr<ListViewProxyStyle> lvp_style_;

    QPixmap pixmap_network_error_;
    QPixmap pixmap_network_idle_;
    QPixmap pixmap_network_receive_;
    QPixmap pixmap_network_transmit_;
    QPixmap pixmap_network_transmit_receive_;

    Ui_MainWindow ui_ = {};

    time_t last_full_update_time_ = {};
    QPointer<SessionDialog> session_dialog_;
    QPointer<PrefsDialog> prefs_dialog_;
    QPointer<AboutDialog> about_dialog_;
    QPointer<StatsDialog> stats_dialog_;
    QPointer<DetailsDialog> details_dialog_;
    QSystemTrayIcon tray_icon_;
    TorrentFilter filter_model_;
    TorrentDelegate* torrent_delegate_ = {};
    TorrentDelegateMin* torrent_delegate_min_ = {};
    time_t last_send_time_ = {};
    time_t last_read_time_ = {};
    QTimer network_timer_;
    bool network_error_ = {};
    QAction* dlimit_off_action_ = {};
    QAction* dlimit_on_action_ = {};
    QAction* ulimit_off_action_ = {};
    QAction* ulimit_on_action_ = {};
    QAction* ratio_off_action_ = {};
    QAction* ratio_on_action_ = {};
    QWidgetList hidden_;
    QWidget* filter_bar_ = {};
    QAction* alt_speed_action_ = {};
    QString error_message_;
    bool auto_add_clipboard_links = {};
    QStringList clipboard_processed_keys_ = {};

    QString const total_ratio_stats_mode_name_ = QStringLiteral("total-ratio");
    QString const total_transfer_stats_mode_name_ = QStringLiteral("total-transfer");
    QString const session_ratio_stats_mode_name_ = QStringLiteral("session-ratio");
    QString const session_transfer_stats_mode_name_ = QStringLiteral("session-transfer");
    QString const show_options_checkbox_name_ = QStringLiteral("show-options-checkbox");

    struct TransferStats
    {
        Speed speed_up;
        Speed speed_down;
        size_t peers_sending = 0;
        size_t peers_receiving = 0;
    };
    [[nodiscard]] TransferStats getTransferStats() const;

    enum
    {
        REFRESH_TITLE = (1 << 0),
        REFRESH_STATUS_BAR = (1 << 1),
        REFRESH_TRAY_ICON = (1 << 2),
        REFRESH_TORRENT_VIEW_HEADER = (1 << 3),
        REFRESH_ACTION_SENSITIVITY = (1 << 4)
    };
    int refresh_fields_ = {};
    QTimer refresh_timer_;

    void refreshActionSensitivity();
    void refreshStatusBar(TransferStats const&);
    void refreshTitle();
    void refreshTorrentViewHeader();
    void refreshTrayIcon(TransferStats const&);
};
