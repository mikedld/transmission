// This file Copyright © 2009-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cassert>
#include <string_view>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFile>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringDecoder>
#else
#include <QTextCodec>
#endif

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>

#include "CustomVariantType.h"
#include "Filters.h"
#include "Prefs.h"
#include "VariantHelpers.h"

using ::trqt::variant_helpers::dictAdd;
using ::trqt::variant_helpers::getValue;

/***
****
***/

namespace
{

void ensureSoundCommandIsAList(tr_variant* dict)
{
    tr_quark const key = TR_KEY_torrent_complete_sound_command;

    if (tr_variant* list = nullptr; tr_variantDictFindList(dict, key, &list))
    {
        return;
    }

    tr_variantDictRemove(dict, key);
    dictAdd(
        dict,
        key,
        std::array<std::string_view, 5>{
            "canberra-gtk-play",
            TR_ARG_TUPLE("-i", "complete-download"),
            TR_ARG_TUPLE("-d", "transmission torrent downloaded"),
        });
}

} // namespace

std::array<Prefs::PrefItem, Prefs::PREFS_COUNT> const Prefs::Items{
    /* gui settings */
    PrefItem{ OPTIONS_PROMPT, TR_KEY_show_options_window, QMetaType::Bool },
    { OPEN_DIALOG_FOLDER, TR_KEY_open_dialog_dir, QMetaType::QString },
    { INHIBIT_HIBERNATION, TR_KEY_inhibit_desktop_hibernation, QMetaType::Bool },
    { DIR_WATCH, TR_KEY_watch_dir, QMetaType::QString },
    { DIR_WATCH_ENABLED, TR_KEY_watch_dir_enabled, QMetaType::Bool },
    { SHOW_TRAY_ICON, TR_KEY_show_notification_area_icon, QMetaType::Bool },
    { START_MINIMIZED, TR_KEY_start_minimized, QMetaType::Bool },
    { SHOW_NOTIFICATION_ON_ADD, TR_KEY_torrent_added_notification_enabled, QMetaType::Bool },
    { SHOW_NOTIFICATION_ON_COMPLETE, TR_KEY_torrent_complete_notification_enabled, QMetaType::Bool },
    { ASKQUIT, TR_KEY_prompt_before_exit, QMetaType::Bool },
    { SORT_MODE, TR_KEY_sort_mode, CustomVariantType::SortModeType },
    { SORT_REVERSED, TR_KEY_sort_reversed, QMetaType::Bool },
    { COMPACT_VIEW, TR_KEY_compact_view, QMetaType::Bool },
    { FILTERBAR, TR_KEY_show_filterbar, QMetaType::Bool },
    { STATUSBAR, TR_KEY_show_statusbar, QMetaType::Bool },
    { STATUSBAR_STATS, TR_KEY_statusbar_stats, QMetaType::QString },
    { SHOW_TRACKER_SCRAPES, TR_KEY_show_extra_peer_details, QMetaType::Bool },
    { SHOW_BACKUP_TRACKERS, TR_KEY_show_backup_trackers, QMetaType::Bool },
    { TOOLBAR, TR_KEY_show_toolbar, QMetaType::Bool },
    { BLOCKLIST_DATE, TR_KEY_blocklist_date, QMetaType::QDateTime },
    { BLOCKLIST_UPDATES_ENABLED, TR_KEY_blocklist_updates_enabled, QMetaType::Bool },
    { MAIN_WINDOW_LAYOUT_ORDER, TR_KEY_main_window_layout_order, QMetaType::QString },
    { MAIN_WINDOW_HEIGHT, TR_KEY_main_window_height, QMetaType::Int },
    { MAIN_WINDOW_WIDTH, TR_KEY_main_window_width, QMetaType::Int },
    { MAIN_WINDOW_X, TR_KEY_main_window_x, QMetaType::Int },
    { MAIN_WINDOW_Y, TR_KEY_main_window_y, QMetaType::Int },
    { FILTER_MODE, TR_KEY_filter_mode, CustomVariantType::FilterModeType },
    { FILTER_TRACKERS, TR_KEY_filter_trackers, QMetaType::QString },
    { FILTER_TEXT, TR_KEY_filter_text, QMetaType::QString },
    { SESSION_IS_REMOTE, TR_KEY_remote_session_enabled, QMetaType::Bool },
    { SESSION_REMOTE_HOST, TR_KEY_remote_session_host, QMetaType::QString },
    { SESSION_REMOTE_HTTPS, TR_KEY_remote_session_https, QMetaType::Bool },
    { SESSION_REMOTE_PASSWORD, TR_KEY_remote_session_password, QMetaType::QString },
    { SESSION_REMOTE_PORT, TR_KEY_remote_session_port, QMetaType::Int },
    { SESSION_REMOTE_AUTH, TR_KEY_remote_session_requres_authentication, QMetaType::Bool },
    { SESSION_REMOTE_USERNAME, TR_KEY_remote_session_username, QMetaType::QString },
    { COMPLETE_SOUND_COMMAND, TR_KEY_torrent_complete_sound_command, QMetaType::QStringList },
    { COMPLETE_SOUND_ENABLED, TR_KEY_torrent_complete_sound_enabled, QMetaType::Bool },
    { USER_HAS_GIVEN_INFORMED_CONSENT, TR_KEY_user_has_given_informed_consent, QMetaType::Bool },
    { READ_CLIPBOARD, TR_KEY_read_clipboard, QMetaType::Bool },

    /* libtransmission settings */
    { ALT_SPEED_LIMIT_UP, TR_KEY_alt_speed_up, QMetaType::Int },
    { ALT_SPEED_LIMIT_DOWN, TR_KEY_alt_speed_down, QMetaType::Int },
    { ALT_SPEED_LIMIT_ENABLED, TR_KEY_alt_speed_enabled, QMetaType::Bool },
    { ALT_SPEED_LIMIT_TIME_BEGIN, TR_KEY_alt_speed_time_begin, QMetaType::Int },
    { ALT_SPEED_LIMIT_TIME_END, TR_KEY_alt_speed_time_end, QMetaType::Int },
    { ALT_SPEED_LIMIT_TIME_ENABLED, TR_KEY_alt_speed_time_enabled, QMetaType::Bool },
    { ALT_SPEED_LIMIT_TIME_DAY, TR_KEY_alt_speed_time_day, QMetaType::Int },
    { BLOCKLIST_ENABLED, TR_KEY_blocklist_enabled, QMetaType::Bool },
    { BLOCKLIST_URL, TR_KEY_blocklist_url, QMetaType::QString },
    { DEFAULT_TRACKERS, TR_KEY_default_trackers, QMetaType::QString },
    { DSPEED, TR_KEY_speed_limit_down, QMetaType::Int },
    { DSPEED_ENABLED, TR_KEY_speed_limit_down_enabled, QMetaType::Bool },
    { DOWNLOAD_DIR, TR_KEY_download_dir, QMetaType::QString },
    { DOWNLOAD_QUEUE_ENABLED, TR_KEY_download_queue_enabled, QMetaType::Bool },
    { DOWNLOAD_QUEUE_SIZE, TR_KEY_download_queue_size, QMetaType::Int },
    { ENCRYPTION, TR_KEY_encryption, QMetaType::Int },
    { IDLE_LIMIT, TR_KEY_idle_seeding_limit, QMetaType::Int },
    { IDLE_LIMIT_ENABLED, TR_KEY_idle_seeding_limit_enabled, QMetaType::Bool },
    { INCOMPLETE_DIR, TR_KEY_incomplete_dir, QMetaType::QString },
    { INCOMPLETE_DIR_ENABLED, TR_KEY_incomplete_dir_enabled, QMetaType::Bool },
    { MSGLEVEL, TR_KEY_message_level, QMetaType::Int },
    { PEER_LIMIT_GLOBAL, TR_KEY_peer_limit_global, QMetaType::Int },
    { PEER_LIMIT_TORRENT, TR_KEY_peer_limit_per_torrent, QMetaType::Int },
    { PEER_PORT, TR_KEY_peer_port, QMetaType::Int },
    { PEER_PORT_RANDOM_ON_START, TR_KEY_peer_port_random_on_start, QMetaType::Bool },
    { PEER_PORT_RANDOM_LOW, TR_KEY_peer_port_random_low, QMetaType::Int },
    { PEER_PORT_RANDOM_HIGH, TR_KEY_peer_port_random_high, QMetaType::Int },
    { QUEUE_STALLED_MINUTES, TR_KEY_queue_stalled_minutes, QMetaType::Int },
    { SCRIPT_TORRENT_DONE_ENABLED, TR_KEY_script_torrent_done_enabled, QMetaType::Bool },
    { SCRIPT_TORRENT_DONE_FILENAME, TR_KEY_script_torrent_done_filename, QMetaType::QString },
    { SCRIPT_TORRENT_DONE_SEEDING_ENABLED, TR_KEY_script_torrent_done_seeding_enabled, QMetaType::Bool },
    { SCRIPT_TORRENT_DONE_SEEDING_FILENAME, TR_KEY_script_torrent_done_seeding_filename, QMetaType::QString },
    { SOCKET_TOS, TR_KEY_peer_socket_tos, QMetaType::QString },
    { START, TR_KEY_start_added_torrents, QMetaType::Bool },
    { TRASH_ORIGINAL, TR_KEY_trash_original_torrent_files, QMetaType::Bool },
    { PEX_ENABLED, TR_KEY_pex_enabled, QMetaType::Bool },
    { DHT_ENABLED, TR_KEY_dht_enabled, QMetaType::Bool },
    { UTP_ENABLED, TR_KEY_utp_enabled, QMetaType::Bool },
    { LPD_ENABLED, TR_KEY_lpd_enabled, QMetaType::Bool },
    { PORT_FORWARDING, TR_KEY_port_forwarding_enabled, QMetaType::Bool },
    { PREALLOCATION, TR_KEY_preallocation, QMetaType::Int },
    { RATIO, TR_KEY_ratio_limit, QMetaType::Double },
    { RATIO_ENABLED, TR_KEY_ratio_limit_enabled, QMetaType::Bool },
    { RENAME_PARTIAL_FILES, TR_KEY_rename_partial_files, QMetaType::Bool },
    { RPC_AUTH_REQUIRED, TR_KEY_rpc_authentication_required, QMetaType::Bool },
    { RPC_ENABLED, TR_KEY_rpc_enabled, QMetaType::Bool },
    { RPC_PASSWORD, TR_KEY_rpc_password, QMetaType::QString },
    { RPC_PORT, TR_KEY_rpc_port, QMetaType::Int },
    { RPC_USERNAME, TR_KEY_rpc_username, QMetaType::QString },
    { RPC_WHITELIST_ENABLED, TR_KEY_rpc_whitelist_enabled, QMetaType::Bool },
    { RPC_WHITELIST, TR_KEY_rpc_whitelist, QMetaType::QString },
    { USPEED_ENABLED, TR_KEY_speed_limit_up_enabled, QMetaType::Bool },
    { USPEED, TR_KEY_speed_limit_up, QMetaType::Int },
    { UPLOAD_SLOTS_PER_TORRENT, TR_KEY_upload_slots_per_torrent, QMetaType::Int },
};

namespace
{

auto constexpr FilterModes = std::array<std::pair<int, std::string_view>, FilterMode::NUM_MODES>{ {
    { FilterMode::SHOW_ALL, "show-all" },
    { FilterMode::SHOW_ACTIVE, "show-active" },
    { FilterMode::SHOW_DOWNLOADING, "show-downloading" },
    { FilterMode::SHOW_SEEDING, "show-seeding" },
    { FilterMode::SHOW_PAUSED, "show-paused" },
    { FilterMode::SHOW_FINISHED, "show-finished" },
    { FilterMode::SHOW_VERIFYING, "show-verifying" },
    { FilterMode::SHOW_ERROR, "show-error" },
} };

auto constexpr SortModes = std::array<std::pair<int, std::string_view>, SortMode::NUM_MODES>{ {
    { SortMode::SORT_BY_NAME, "sort-by-name" },
    { SortMode::SORT_BY_ACTIVITY, "sort-by-activity" },
    { SortMode::SORT_BY_AGE, "sort-by-age" },
    { SortMode::SORT_BY_ETA, "sort-by-eta" },
    { SortMode::SORT_BY_PROGRESS, "sort-by-progress" },
    { SortMode::SORT_BY_QUEUE, "sort-by-queue" },
    { SortMode::SORT_BY_RATIO, "sort-by-ratio" },
    { SortMode::SORT_BY_SIZE, "sort-by-size" },
    { SortMode::SORT_BY_STATE, "sort-by-state" },
    { SortMode::SORT_BY_ID, "sort-by-id" },
} };

bool isValidUtf8(QByteArray const& byteArray)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

    auto decoder = QStringDecoder{ QStringConverter::Utf8, QStringConverter::Flag::Stateless };
    auto const text = QString{ decoder.decode(byteArray) };
    return !decoder.hasError() && !text.contains(QChar::ReplacementCharacter);

#else

    auto const* const codec = QTextCodec::codecForName("UTF-8");
    auto state = QTextCodec::ConverterState{};
    codec->toUnicode(byteArray.constData(), byteArray.size(), &state);
    return state.invalidChars == 0;

#endif
}

} // namespace

/***
****
***/

Prefs::Prefs(QString config_dir)
    : config_dir_(std::move(config_dir))
{
    static_assert(sizeof(Items) / sizeof(Items[0]) == PREFS_COUNT);

#ifndef NDEBUG
    for (int i = 0; i < PREFS_COUNT; ++i)
    {
        assert(Items[i].id == i);
    }

#endif

    // these are the prefs that don't get saved to settings.json
    // when the application exits.
    temporary_prefs_ << FILTER_TEXT;

    auto top = tr_variant::make_map();
    initDefaults(&top);
    tr_sessionLoadSettings(&top, config_dir_.toUtf8().constData(), nullptr);
    ensureSoundCommandIsAList(&top);

    for (int i = 0; i < PREFS_COUNT; ++i)
    {
        tr_variant const* b = tr_variantDictFind(&top, Items[i].key);

        switch (Items[i].type)
        {
        case QMetaType::Int:
            if (auto const value = getValue<int64_t>(b); value)
            {
                values_[i].setValue(*value);
            }
            break;

        case CustomVariantType::SortModeType:
            if (auto const value = getValue<std::string_view>(b); value)
            {
                auto const test = [&value](auto const& item)
                {
                    return item.second == *value;
                };
                // NOLINTNEXTLINE(readability-qualified-auto)
                auto const it = std::find_if(std::cbegin(SortModes), std::cend(SortModes), test);
                auto const& [mode, mode_str] = it == std::end(SortModes) ? SortModes.front() : *it;
                values_[i] = QVariant::fromValue(SortMode{ mode });
            }
            break;

        case CustomVariantType::FilterModeType:
            if (auto const value = getValue<std::string_view>(b); value)
            {
                auto const test = [&value](auto const& item)
                {
                    return item.second == *value;
                };
                // NOLINTNEXTLINE(readability-qualified-auto)
                auto const it = std::find_if(std::cbegin(FilterModes), std::cend(FilterModes), test);
                auto const& [mode, mode_str] = it == std::end(FilterModes) ? FilterModes.front() : *it;
                values_[i] = QVariant::fromValue(FilterMode{ mode });
            }
            break;

        case QMetaType::QString:
            if (auto const value = getValue<QString>(b); value)
            {
                values_[i].setValue(*value);
            }
            break;

        case QMetaType::QStringList:
            if (auto const value = getValue<QStringList>(b); value)
            {
                values_[i].setValue(*value);
            }
            break;

        case QMetaType::Bool:
            if (auto const value = getValue<bool>(b); value)
            {
                values_[i].setValue(*value);
            }
            break;

        case QMetaType::Double:
            if (auto const value = getValue<double>(b); value)
            {
                values_[i].setValue(*value);
            }
            break;

        case QMetaType::QDateTime:
            if (auto const value = getValue<time_t>(b); value)
            {
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
                values_[i].setValue(QDateTime::fromSecsSinceEpoch(*value));
#else
                values_[i].setValue(QDateTime::fromTime_t(*value));
#endif
            }
            break;

        default:
            assert(false && "unhandled type");
            break;
        }
    }
}

Prefs::~Prefs()
{
    // make a dict from settings.json
    auto current_settings = tr_variant::make_map(PREFS_COUNT);

    for (int i = 0; i < PREFS_COUNT; ++i)
    {
        if (temporary_prefs_.contains(i))
        {
            continue;
        }

        tr_quark const key = Items[i].key;
        QVariant const& val = values_[i];

        switch (Items[i].type)
        {
        case QMetaType::Int:
            dictAdd(&current_settings, key, val.toInt());
            break;

        case CustomVariantType::SortModeType:
            {
                auto const mode = val.value<SortMode>().mode();
                auto const test = [&mode](auto const& item)
                {
                    return item.first == mode;
                };
                // NOLINTNEXTLINE(readability-qualified-auto)
                auto const it = std::find_if(std::cbegin(SortModes), std::cend(SortModes), test);
                auto const& [mode_val, mode_str] = it == std::end(SortModes) ? SortModes.front() : *it;
                dictAdd(&current_settings, key, mode_str);
                break;
            }

        case CustomVariantType::FilterModeType:
            {
                auto const mode = val.value<FilterMode>().mode();
                auto const test = [&mode](auto const& item)
                {
                    return item.first == mode;
                };
                // NOLINTNEXTLINE(readability-qualified-auto)
                auto const it = std::find_if(std::cbegin(FilterModes), std::cend(FilterModes), test);
                auto const& [mode_val, mode_str] = it == std::end(FilterModes) ? FilterModes.front() : *it;
                dictAdd(&current_settings, key, mode_str);
                break;
            }

        case QMetaType::QString:
            dictAdd(&current_settings, key, val.toString());
            break;

        case QMetaType::QStringList:
            dictAdd(&current_settings, key, val.toStringList());
            break;

        case QMetaType::Bool:
            dictAdd(&current_settings, key, val.toBool());
            break;

        case QMetaType::Double:
            dictAdd(&current_settings, key, val.toDouble());
            break;

        case QMetaType::QDateTime:
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
            dictAdd(&current_settings, key, int64_t{ val.toDateTime().toSecsSinceEpoch() });
#else
            dictAdd(&current_settings, key, val.toDateTime().toTime_t());
#endif
            break;

        default:
            assert(false && "unhandled type");
            break;
        }
    }

    // update settings.json with our settings
    auto serde = tr_variant_serde::json();
    auto const file = QFile{ QDir{ config_dir_ }.absoluteFilePath(QStringLiteral("settings.json")) };
    auto const filename = file.fileName().toStdString();
    auto settings = serde.parse_file(filename);
    if (!settings)
    {
        settings = tr_variant::make_map(PREFS_COUNT);
    }

    tr_variantMergeDicts(&*settings, &current_settings);
    serde.to_file(*settings, filename);
}

/**
 * This is where we initialize the preferences file with the default values.
 * If you add a new preferences key, you /must/ add a default value here.
 */
void Prefs::initDefaults(tr_variant* d) const
{
    auto constexpr FilterMode = std::string_view{ "all" };
    auto constexpr SessionHost = std::string_view{ "localhost" };
    auto constexpr SessionPassword = std::string_view{};
    auto constexpr SessionUsername = std::string_view{};
    auto constexpr SortMode = std::string_view{ "sort-by-name" };
    auto constexpr StatsMode = std::string_view{ "total-ratio" };
    auto constexpr WindowLayout = std::string_view{ "menu,toolbar,filter,list,statusbar" };

    auto const download_dir = tr_getDefaultDownloadDir();

    tr_variantDictReserve(d, 38);
    dictAdd(d, TR_KEY_blocklist_updates_enabled, true);
    dictAdd(d, TR_KEY_compact_view, false);
    dictAdd(d, TR_KEY_inhibit_desktop_hibernation, false);
    dictAdd(d, TR_KEY_prompt_before_exit, true);
    dictAdd(d, TR_KEY_remote_session_enabled, false);
    dictAdd(d, TR_KEY_remote_session_requres_authentication, false);
    dictAdd(d, TR_KEY_show_backup_trackers, false);
    dictAdd(d, TR_KEY_show_extra_peer_details, false);
    dictAdd(d, TR_KEY_show_filterbar, true);
    dictAdd(d, TR_KEY_show_notification_area_icon, false);
    dictAdd(d, TR_KEY_start_minimized, false);
    dictAdd(d, TR_KEY_show_options_window, true);
    dictAdd(d, TR_KEY_show_statusbar, true);
    dictAdd(d, TR_KEY_show_toolbar, true);
    dictAdd(d, TR_KEY_show_tracker_scrapes, false);
    dictAdd(d, TR_KEY_sort_reversed, false);
    dictAdd(d, TR_KEY_torrent_added_notification_enabled, true);
    dictAdd(d, TR_KEY_torrent_complete_notification_enabled, true);
    dictAdd(d, TR_KEY_torrent_complete_sound_enabled, true);
    dictAdd(d, TR_KEY_user_has_given_informed_consent, false);
    dictAdd(d, TR_KEY_watch_dir_enabled, false);
    dictAdd(d, TR_KEY_blocklist_date, 0);
    dictAdd(d, TR_KEY_main_window_height, 500);
    dictAdd(d, TR_KEY_main_window_width, 300);
    dictAdd(d, TR_KEY_main_window_x, 50);
    dictAdd(d, TR_KEY_main_window_y, 50);
    dictAdd(d, TR_KEY_remote_session_port, TR_DEFAULT_RPC_PORT);
    dictAdd(d, TR_KEY_download_dir, download_dir);
    dictAdd(d, TR_KEY_filter_mode, FilterMode);
    dictAdd(d, TR_KEY_main_window_layout_order, WindowLayout);
    dictAdd(d, TR_KEY_open_dialog_dir, QDir::home().absolutePath());
    dictAdd(d, TR_KEY_remote_session_https, false);
    dictAdd(d, TR_KEY_remote_session_host, SessionHost);
    dictAdd(d, TR_KEY_remote_session_password, SessionPassword);
    dictAdd(d, TR_KEY_remote_session_username, SessionUsername);
    dictAdd(d, TR_KEY_sort_mode, SortMode);
    dictAdd(d, TR_KEY_statusbar_stats, StatsMode);
    dictAdd(d, TR_KEY_watch_dir, download_dir);
    dictAdd(d, TR_KEY_read_clipboard, false);
}

/***
****
***/

bool Prefs::getBool(int key) const
{
    assert(Items[key].type == QMetaType::Bool);
    return values_[key].toBool();
}

QString Prefs::getString(int key) const
{
    assert(Items[key].type == QMetaType::QString);

    if (auto const b = values_[key].toByteArray(); isValidUtf8(b.constData()))
    {
        values_[key].setValue(QString::fromUtf8(b.constData()));
    }

    return values_[key].toString();
}

int Prefs::getInt(int key) const
{
    assert(Items[key].type == QMetaType::Int);
    return values_[key].toInt();
}

double Prefs::getDouble(int key) const
{
    assert(Items[key].type == QMetaType::Double);
    return values_[key].toDouble();
}

QDateTime Prefs::getDateTime(int key) const
{
    assert(Items[key].type == QMetaType::QDateTime);
    return values_[key].toDateTime();
}

/***
****
***/

void Prefs::toggleBool(int key)
{
    set(key, !getBool(key));
}
