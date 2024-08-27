// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cassert>
#include <string_view>
#include <utility>

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStyle>
#include <QTextStream>
#include <QtDebug>

#include <libtransmission/transmission.h>

#include <libtransmission/quark.h>
#include <libtransmission/session-id.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>

#include "Session.h"

#include "AddData.h"
#include "CustomVariantType.h"
#include "Prefs.h"
#include "RpcQueue.h"
#include "SessionDialog.h"
#include "Torrent.h"
#include "VariantHelpers.h"

using namespace std::literals;

using ::trqt::variant_helpers::dictAdd;
using ::trqt::variant_helpers::dictFind;
using ::trqt::variant_helpers::getValue;

/***
****
***/

void Session::sessionSet(tr_quark const key, QVariant const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 1);

#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
    switch (value.typeId())
#else
    switch (static_cast<QMetaType::Type>(value.type()))
#endif
    {
    case QMetaType::Bool:
        dictAdd(&args, key, value.toBool());
        break;

    case QMetaType::Int:
        dictAdd(&args, key, value.toInt());
        break;

    case QMetaType::Double:
        dictAdd(&args, key, value.toDouble());
        break;

    case QMetaType::QString:
        dictAdd(&args, key, value.toString());
        break;

    default:
        assert(false);
    }

    exec("session-set", &args);
}

void Session::portTest(Session::PortTestIpProtocol const ip_protocol)
{
    static auto constexpr IpStr = std::array{ "ipv4"sv, "ipv6"sv };

    if (portTestPending(ip_protocol))
    {
        return;
    }
    port_test_pending_[ip_protocol] = true;

    auto args = tr_variant::make_map(1U);
    tr_variantDictAddStrView(&args, TR_KEY_ipProtocol, IpStr[ip_protocol]);

    auto const response_func = [this, ip_protocol](RpcResponse const& r)
    {
        port_test_pending_[ip_protocol] = false;

        // If for whatever reason the status optional is empty here,
        // then something must have gone wrong with the port test,
        // so the UI should show the "error" state
        emit portTested(dictFind<bool>(r.args.get(), TR_KEY_port_is_open), ip_protocol);
    };

    auto* q = new RpcQueue{};

    q->add([this, &args]() { return exec("port-test", &args); }, response_func);

    q->add(response_func);

    q->run();
}

bool Session::portTestPending(Session::PortTestIpProtocol const ip_protocol) const noexcept
{
    return ip_protocol < NUM_PORT_TEST_IP_PROTOCOL && port_test_pending_[ip_protocol];
}

void Session::copyMagnetLinkToClipboard(int torrent_id)
{
    auto const fields = std::array{ tr_quark_get_string_view(TR_KEY_magnet_link_camel) };

    tr_variant args;
    tr_variantInitDict(&args, 2);
    dictAdd(&args, TR_KEY_ids, std::array<int, 1>{ torrent_id });
    dictAdd(&args, TR_KEY_fields, fields);

    auto* q = new RpcQueue{};

    q->add([this, &args]() { return exec(TR_KEY_torrent_get_kebab, &args); });

    q->add(
        [](RpcResponse const& r)
        {
            tr_variant* torrents = nullptr;
            if (!tr_variantDictFindList(r.args.get(), TR_KEY_torrents, &torrents))
            {
                return;
            }

            tr_variant* const child = tr_variantListChild(torrents, 0);
            if (child != nullptr)
            {
                auto const link = dictFind<QString>(child, TR_KEY_magnet_link_camel);
                if (link)
                {
                    QApplication::clipboard()->setText(*link);
                }
            }
        });

    q->run();
}

void Session::updatePref(int key)
{
    if (prefs_.isCore(key))
    {
        switch (key)
        {
        case Prefs::ALT_SPEED_LIMIT_DOWN:
        case Prefs::ALT_SPEED_LIMIT_ENABLED:
        case Prefs::ALT_SPEED_LIMIT_TIME_BEGIN:
        case Prefs::ALT_SPEED_LIMIT_TIME_DAY:
        case Prefs::ALT_SPEED_LIMIT_TIME_ENABLED:
        case Prefs::ALT_SPEED_LIMIT_TIME_END:
        case Prefs::ALT_SPEED_LIMIT_UP:
        case Prefs::BLOCKLIST_DATE:
        case Prefs::BLOCKLIST_ENABLED:
        case Prefs::BLOCKLIST_URL:
        case Prefs::DEFAULT_TRACKERS:
        case Prefs::DHT_ENABLED:
        case Prefs::DOWNLOAD_QUEUE_ENABLED:
        case Prefs::DOWNLOAD_QUEUE_SIZE:
        case Prefs::DSPEED:
        case Prefs::DSPEED_ENABLED:
        case Prefs::IDLE_LIMIT:
        case Prefs::IDLE_LIMIT_ENABLED:
        case Prefs::INCOMPLETE_DIR:
        case Prefs::INCOMPLETE_DIR_ENABLED:
        case Prefs::LPD_ENABLED:
        case Prefs::PEER_LIMIT_GLOBAL:
        case Prefs::PEER_LIMIT_TORRENT:
        case Prefs::PEER_PORT:
        case Prefs::PEER_PORT_RANDOM_ON_START:
        case Prefs::QUEUE_STALLED_MINUTES:
        case Prefs::PEX_ENABLED:
        case Prefs::PORT_FORWARDING:
        case Prefs::RENAME_PARTIAL_FILES:
        case Prefs::SCRIPT_TORRENT_DONE_ENABLED:
        case Prefs::SCRIPT_TORRENT_DONE_FILENAME:
        case Prefs::SCRIPT_TORRENT_DONE_SEEDING_ENABLED:
        case Prefs::SCRIPT_TORRENT_DONE_SEEDING_FILENAME:
        case Prefs::START:
        case Prefs::TRASH_ORIGINAL:
        case Prefs::USPEED:
        case Prefs::USPEED_ENABLED:
        case Prefs::UTP_ENABLED:
            sessionSet(prefs_.getKey(key), prefs_.variant(key));
            break;

        case Prefs::DOWNLOAD_DIR:
            sessionSet(prefs_.getKey(key), prefs_.variant(key));
            /* this will change the 'freespace' argument, so refresh */
            refreshSessionInfo();
            break;

        case Prefs::RATIO:
            sessionSet(TR_KEY_seed_ratio_limit_camel, prefs_.variant(key));
            break;

        case Prefs::RATIO_ENABLED:
            sessionSet(TR_KEY_seedRatioLimited, prefs_.variant(key));
            break;

        case Prefs::ENCRYPTION:
            switch (int const i = prefs_.variant(key).toInt(); i)
            {
            case 0:
                sessionSet(prefs_.getKey(key), QStringLiteral("tolerated"));
                break;

            case 1:
                sessionSet(prefs_.getKey(key), QStringLiteral("preferred"));
                break;

            case 2:
                sessionSet(prefs_.getKey(key), QStringLiteral("required"));
                break;

            default:
                break;
            }

            break;

        case Prefs::RPC_AUTH_REQUIRED:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCPasswordEnabled(session_, prefs_.getBool(key));
            }

            break;

        case Prefs::RPC_ENABLED:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCEnabled(session_, prefs_.getBool(key));
            }

            break;

        case Prefs::RPC_PASSWORD:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCPassword(session_, prefs_.getString(key).toUtf8().constData());
            }

            break;

        case Prefs::RPC_PORT:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCPort(session_, static_cast<uint16_t>(prefs_.getInt(key)));
            }

            break;

        case Prefs::RPC_USERNAME:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCUsername(session_, prefs_.getString(key).toUtf8().constData());
            }

            break;

        case Prefs::RPC_WHITELIST_ENABLED:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCWhitelistEnabled(session_, prefs_.getBool(key));
            }

            break;

        case Prefs::RPC_WHITELIST:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCWhitelist(session_, prefs_.getString(key).toUtf8().constData());
            }

            break;

        default:
            qWarning() << "unhandled pref:" << key;
        }
    }
}

/***
****
***/

Session::Session(QString config_dir, Prefs& prefs)
    : config_dir_{ std::move(config_dir) }
    , prefs_{ prefs }
{
    connect(&prefs_, &Prefs::changed, this, &Session::updatePref);
    connect(&rpc_, &RpcClient::httpAuthenticationRequired, this, &Session::httpAuthenticationRequired);
    connect(&rpc_, &RpcClient::dataReadProgress, this, &Session::dataReadProgress);
    connect(&rpc_, &RpcClient::dataSendProgress, this, &Session::dataSendProgress);
    connect(&rpc_, &RpcClient::networkResponse, this, &Session::networkResponse);

    duplicates_timer_.setSingleShot(true);
    connect(&duplicates_timer_, &QTimer::timeout, this, &Session::onDuplicatesTimer);
}

Session::~Session()
{
    stop();
}

/***
****
***/

void Session::stop()
{
    rpc_.stop();

    if (session_ != nullptr)
    {
        tr_sessionClose(session_);
        session_ = nullptr;
    }
}

void Session::restart()
{
    stop();
    start();
}

void Session::start()
{
    if (prefs_.get<bool>(Prefs::SESSION_IS_REMOTE))
    {
        QUrl url;
        if (prefs_.get<bool>(Prefs::SESSION_REMOTE_HTTPS))
        {
            url.setScheme(QStringLiteral("https"));
        }
        else
        {
            url.setScheme(QStringLiteral("http"));
        }
        url.setHost(prefs_.get<QString>(Prefs::SESSION_REMOTE_HOST));
        url.setPort(prefs_.get<int>(Prefs::SESSION_REMOTE_PORT));
        url.setPath(QStringLiteral("/transmission/rpc"));

        if (prefs_.get<bool>(Prefs::SESSION_REMOTE_AUTH))
        {
            url.setUserName(prefs_.get<QString>(Prefs::SESSION_REMOTE_USERNAME));
            url.setPassword(prefs_.get<QString>(Prefs::SESSION_REMOTE_PASSWORD));
        }

        rpc_.start(url);
    }
    else
    {
        auto const settings = tr_sessionLoadSettings(nullptr, config_dir_.toUtf8().constData(), "qt");
        session_ = tr_sessionInit(config_dir_.toUtf8().constData(), true, settings);

        rpc_.start(session_);

        auto* const ctor = tr_ctorNew(session_);
        tr_sessionLoadTorrents(session_, ctor);
        tr_ctorFree(ctor);
    }

    emit sourceChanged();
}

// ---

void Session::addOptionalIds(tr_variant* args_dict, torrent_ids_t const& torrent_ids) const
{
    auto constexpr RecentlyActiveKey = std::string_view{ "recently-active" };

    if (&torrent_ids == &RecentlyActiveIDs)
    {
        dictAdd(args_dict, TR_KEY_ids, RecentlyActiveKey);
    }
    else if (!std::empty(torrent_ids))
    {
        dictAdd(args_dict, TR_KEY_ids, torrent_ids);
    }
}

Session::Tag Session::torrentSetImpl(tr_variant* args)
{
    auto* const q = new RpcQueue{};
    auto const tag = q->tag();

    q->add([this, args]() { return rpc_.exec(TR_KEY_torrent_set_kebab, args); });
    q->add([this, tag]() { emit sessionCalled(tag); });
    q->setTolerateErrors();
    q->run();

    return tag;
}

Session::Tag Session::torrentSet(torrent_ids_t const& torrent_ids, tr_quark const key, double value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, torrent_ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& torrent_ids, tr_quark const key, int value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, torrent_ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& torrent_ids, tr_quark const key, bool value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, torrent_ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& torrent_ids, tr_quark const key, QString const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, torrent_ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& torrent_ids, tr_quark const key, QStringList const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, torrent_ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& torrent_ids, tr_quark const key, std::vector<int> const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, torrent_ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

void Session::torrentSetLocation(torrent_ids_t const& torrent_ids, QString const& path, bool do_move)
{
    tr_variant args;
    tr_variantInitDict(&args, 3);
    addOptionalIds(&args, torrent_ids);
    dictAdd(&args, TR_KEY_location, path);
    dictAdd(&args, TR_KEY_move, do_move);

    exec(TR_KEY_torrent_set_location_kebab, &args);
}

void Session::torrentRenamePath(torrent_ids_t const& torrent_ids, QString const& oldpath, QString const& newname)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, torrent_ids);
    dictAdd(&args, TR_KEY_path, oldpath);
    dictAdd(&args, TR_KEY_name, newname);

    auto* q = new RpcQueue{};

    q->add(
        [this, &args]() { return exec("torrent-rename-path", &args); },
        [](RpcResponse const& r)
        {
            auto const path = dictFind<QString>(r.args.get(), TR_KEY_path).value_or(QStringLiteral("(unknown)"));
            auto const name = dictFind<QString>(r.args.get(), TR_KEY_name).value_or(QStringLiteral("(unknown)"));

            auto* d = new QMessageBox{
                QMessageBox::Information,
                tr("Error Renaming Path"),
                tr(R"(<p><b>Unable to rename "%1" as "%2": %3.</b></p><p>Please correct the errors and try again.</p>)")
                    .arg(path)
                    .arg(name)
                    .arg(r.result),
                QMessageBox::Close,
                QApplication::activeWindow()
            };
            QObject::connect(d, &QMessageBox::rejected, d, &QMessageBox::deleteLater);
            d->show();
        });

    q->add([this, torrent_ids](RpcResponse const& /*r*/) { refreshTorrents(torrent_ids, TorrentProperties::Rename); });

    q->run();
}

std::set<std::string_view> const& Session::getKeyNames(TorrentProperties props)
{
    std::set<std::string_view>& names = names_[props];

    if (names.empty())
    {
        // unchanging fields needed by the main window
        static auto constexpr MainInfoKeys = std::array<tr_quark, 9>{
            TR_KEY_added_date_camel, //
            TR_KEY_download_dir_camel, //
            TR_KEY_file_count_kebab, //
            TR_KEY_hash_string_camel, //
            TR_KEY_labels, //
            TR_KEY_name, //
            TR_KEY_primary_mime_type_kebab, //
            TR_KEY_totalSize, //
            TR_KEY_trackers, //
        };

        // changing fields needed by the main window
        static auto constexpr MainStatKeys = std::array<tr_quark, 25>{
            TR_KEY_downloaded_ever_camel,
            TR_KEY_edit_date_camel,
            TR_KEY_error,
            TR_KEY_error_string_camel,
            TR_KEY_eta,
            TR_KEY_have_unchecked_camel,
            TR_KEY_have_valid_camel,
            TR_KEY_is_finished_camel,
            TR_KEY_left_until_done_camel,
            TR_KEY_manual_announce_time_camel,
            TR_KEY_metadata_percent_complete_camel,
            TR_KEY_peers_connected_camel,
            TR_KEY_peers_getting_from_us_camel,
            TR_KEY_peers_sending_to_us_camel,
            TR_KEY_percent_done_camel,
            TR_KEY_queue_position_camel,
            TR_KEY_rate_download_camel,
            TR_KEY_rate_upload_camel,
            TR_KEY_recheck_progress_camel,
            TR_KEY_seed_ratio_limit_camel,
            TR_KEY_seed_ratio_mode_camel,
            TR_KEY_size_when_done_camel,
            TR_KEY_status,
            TR_KEY_uploadedEver,
            TR_KEY_webseedsSendingToUs,
        };

        // unchanging fields needed by the details dialog
        static auto constexpr DetailInfoKeys = std::array<tr_quark, 10>{
            TR_KEY_comment, //
            TR_KEY_creator, //
            TR_KEY_date_created_camel, //
            TR_KEY_files, //
            TR_KEY_is_private_camel, //
            TR_KEY_labels, //
            TR_KEY_piece_count_camel, //
            TR_KEY_piece_size_camel, //
            TR_KEY_tracker_list_camel, //
            TR_KEY_trackers, //
        };

        // changing fields needed by the details dialog
        static auto constexpr DetailStatKeys = std::array<tr_quark, 18>{
            TR_KEY_activity_date_camel, //
            TR_KEY_bandwidth_priority_camel, //
            TR_KEY_corrupt_ever_camel, //
            TR_KEY_desired_available_camel, //
            TR_KEY_downloaded_ever_camel, //
            TR_KEY_download_limit_camel, //
            TR_KEY_download_limited_camel, //
            TR_KEY_file_stats_camel, //
            TR_KEY_have_unchecked_camel, //
            TR_KEY_honors_session_limits_camel, //
            TR_KEY_peer_limit_kebab, //
            TR_KEY_peers, //
            TR_KEY_seed_idle_limit_camel, //
            TR_KEY_seed_idle_mode_camel, //
            TR_KEY_start_date_camel, //
            TR_KEY_trackerStats, //
            TR_KEY_upload_limit_camel, //
            TR_KEY_upload_limited_camel, //
        };

        // keys needed after renaming a torrent
        static auto constexpr RenameKeys = std::array<tr_quark, 3>{
            TR_KEY_file_stats_camel,
            TR_KEY_files,
            TR_KEY_name,
        };

        auto const append = [&names](tr_quark key)
        {
            names.emplace(tr_quark_get_string_view(key));
        };

        switch (props)
        {
        case TorrentProperties::DetailInfo:
            std::for_each(DetailInfoKeys.begin(), DetailInfoKeys.end(), append);
            break;

        case TorrentProperties::DetailStat:
            std::for_each(DetailStatKeys.begin(), DetailStatKeys.end(), append);
            break;

        case TorrentProperties::MainAll:
            std::for_each(MainInfoKeys.begin(), MainInfoKeys.end(), append);
            std::for_each(MainStatKeys.begin(), MainStatKeys.end(), append);
            break;

        case TorrentProperties::MainInfo:
            std::for_each(MainInfoKeys.begin(), MainInfoKeys.end(), append);
            break;

        case TorrentProperties::MainStats:
            std::for_each(MainStatKeys.begin(), MainStatKeys.end(), append);
            break;

        case TorrentProperties::Rename:
            std::for_each(RenameKeys.begin(), RenameKeys.end(), append);
            break;
        }

        // must be in every torrent req
        append(TR_KEY_id);
    }

    return names;
}

void Session::refreshTorrents(torrent_ids_t const& torrent_ids, TorrentProperties props)
{
    auto constexpr Table = std::string_view{ "table" };

    tr_variant args;
    tr_variantInitDict(&args, 3);
    dictAdd(&args, TR_KEY_format, Table);
    dictAdd(&args, TR_KEY_fields, getKeyNames(props));
    addOptionalIds(&args, torrent_ids);

    auto* q = new RpcQueue{};

    q->add([this, &args]() { return exec(TR_KEY_torrent_get_kebab, &args); });

    bool const all_torrents = std::empty(torrent_ids);

    q->add(
        [this, all_torrents](RpcResponse const& r)
        {
            tr_variant* torrents = nullptr;

            if (tr_variantDictFindList(r.args.get(), TR_KEY_torrents, &torrents))
            {
                emit torrentsUpdated(torrents, all_torrents);
            }

            if (tr_variantDictFindList(r.args.get(), TR_KEY_removed, &torrents))
            {
                emit torrentsRemoved(torrents);
            }
        });

    q->run();
}

void Session::refreshDetailInfo(torrent_ids_t const& ids)
{
    refreshTorrents(ids, TorrentProperties::DetailInfo);
}

void Session::refreshExtraStats(torrent_ids_t const& ids)
{
    refreshTorrents(ids, TorrentProperties::DetailStat);
}

void Session::sendTorrentRequest(std::string_view request, torrent_ids_t const& torrent_ids)
{
    tr_variant args;
    tr_variantInitDict(&args, 1);
    addOptionalIds(&args, torrent_ids);

    auto* q = new RpcQueue{};

    q->add([this, request, &args]() { return exec(request, &args); });

    q->add([this, torrent_ids]() { refreshTorrents(torrent_ids, TorrentProperties::MainStats); });

    q->run();
}

void Session::pauseTorrents(torrent_ids_t const& ids)
{
    sendTorrentRequest("torrent-stop", ids);
}

void Session::startTorrents(torrent_ids_t const& ids)
{
    sendTorrentRequest("torrent-start", ids);
}

void Session::startTorrentsNow(torrent_ids_t const& ids)
{
    sendTorrentRequest("torrent-start-now", ids);
}

void Session::queueMoveTop(torrent_ids_t const& ids)
{
    sendTorrentRequest("queue-move-top", ids);
}

void Session::queueMoveUp(torrent_ids_t const& ids)
{
    sendTorrentRequest("queue-move-up", ids);
}

void Session::queueMoveDown(torrent_ids_t const& ids)
{
    sendTorrentRequest("queue-move-down", ids);
}

void Session::queueMoveBottom(torrent_ids_t const& ids)
{
    sendTorrentRequest("queue-move-bottom", ids);
}

void Session::refreshActiveTorrents()
{
    // If this object is passed as "ids" (compared by address), then recently active torrents are queried.
    refreshTorrents(RecentlyActiveIDs, TorrentProperties::MainStats);
}

void Session::refreshAllTorrents()
{
    // if an empty ids object is used, all torrents are queried.
    torrent_ids_t const ids = {};
    refreshTorrents(ids, TorrentProperties::MainStats);
}

void Session::initTorrents(torrent_ids_t const& ids)
{
    refreshTorrents(ids, TorrentProperties::MainAll);
}

void Session::refreshSessionStats()
{
    auto* q = new RpcQueue{};

    q->add([this]() { return exec("session-stats", nullptr); });

    q->add([this](RpcResponse const& r) { updateStats(r.args.get()); });

    q->run();
}

void Session::refreshSessionInfo()
{
    auto* q = new RpcQueue{};

    q->add([this]() { return exec("session-get", nullptr); });

    q->add([this](RpcResponse const& r) { updateInfo(r.args.get()); });

    q->run();
}

void Session::updateBlocklist()
{
    auto* q = new RpcQueue{};

    q->add([this]() { return exec("blocklist-update", nullptr); });

    q->add(
        [this](RpcResponse const& r)
        {
            if (auto const size = dictFind<int>(r.args.get(), TR_KEY_blocklist_size); size)
            {
                setBlocklistSize(*size);
            }
        });

    q->run();
}

/***
****
***/

RpcResponseFuture Session::exec(tr_quark method, tr_variant* args)
{
    return rpc_.exec(method, args);
}

RpcResponseFuture Session::exec(std::string_view method, tr_variant* args)
{
    return rpc_.exec(method, args);
}

void Session::updateStats(tr_variant* args_dict, tr_session_stats* stats)
{
    if (auto const value = dictFind<uint64_t>(args_dict, TR_KEY_uploadedBytes); value)
    {
        stats->uploadedBytes = *value;
    }

    if (auto const value = dictFind<uint64_t>(args_dict, TR_KEY_downloadedBytes); value)
    {
        stats->downloadedBytes = *value;
    }

    if (auto const value = dictFind<uint64_t>(args_dict, TR_KEY_filesAdded); value)
    {
        stats->filesAdded = *value;
    }

    if (auto const value = dictFind<uint64_t>(args_dict, TR_KEY_sessionCount); value)
    {
        stats->sessionCount = *value;
    }

    if (auto const value = dictFind<uint64_t>(args_dict, TR_KEY_secondsActive); value)
    {
        stats->secondsActive = *value;
    }

    stats->ratio = static_cast<float>(tr_getRatio(stats->uploadedBytes, stats->downloadedBytes));
}

void Session::updateStats(tr_variant* dict)
{
    if (tr_variant* var = nullptr; tr_variantDictFindDict(dict, TR_KEY_current_stats, &var))
    {
        updateStats(var, &stats_);
    }

    if (tr_variant* var = nullptr; tr_variantDictFindDict(dict, TR_KEY_cumulative_stats, &var))
    {
        updateStats(var, &cumulative_stats_);
    }

    emit statsUpdated();
}

void Session::updateInfo(tr_variant* args_dict)
{
    disconnect(&prefs_, &Prefs::changed, this, &Session::updatePref);

    for (int i = Prefs::FIRST_CORE_PREF; i <= Prefs::LAST_CORE_PREF; ++i)
    {
        tr_variant const* b(tr_variantDictFind(args_dict, prefs_.getKey(i)));

        if (b == nullptr)
        {
            continue;
        }

        if (i == Prefs::ENCRYPTION)
        {
            if (auto const str = getValue<QString>(b); str)
            {
                if (*str == QStringLiteral("required"))
                {
                    prefs_.set(i, 2);
                }
                else if (*str == QStringLiteral("preferred"))
                {
                    prefs_.set(i, 1);
                }
                else if (*str == QStringLiteral("tolerated"))
                {
                    prefs_.set(i, 0);
                }
            }

            continue;
        }

        switch (prefs_.type(i))
        {
        case QMetaType::Int:
            if (auto const value = getValue<int>(b); value)
            {
                prefs_.set(i, *value);
            }

            break;

        case QMetaType::Double:
            if (auto const value = getValue<double>(b); value)
            {
                prefs_.set(i, *value);
            }

            break;

        case QMetaType::Bool:
            if (auto const value = getValue<bool>(b); value)
            {
                prefs_.set(i, *value);
            }

            break;

        case CustomVariantType::FilterModeType:
        case CustomVariantType::SortModeType:
        case QMetaType::QString:
            if (auto const value = getValue<QString>(b); value)
            {
                prefs_.set(i, *value);
            }

            break;

        default:
            break;
        }
    }

    if (auto const b = dictFind<bool>(args_dict, TR_KEY_seedRatioLimited); b)
    {
        prefs_.set(Prefs::RATIO_ENABLED, *b);
    }

    if (auto const x = dictFind<double>(args_dict, TR_KEY_seed_ratio_limit_camel); x)
    {
        prefs_.set(Prefs::RATIO, *x);
    }

    /* Use the C API to get settings that, for security reasons, aren't supported by RPC */
    if (session_ != nullptr)
    {
        prefs_.set(Prefs::RPC_ENABLED, tr_sessionIsRPCEnabled(session_));
        prefs_.set(Prefs::RPC_AUTH_REQUIRED, tr_sessionIsRPCPasswordEnabled(session_));
        prefs_.set(Prefs::RPC_PASSWORD, QString::fromUtf8(tr_sessionGetRPCPassword(session_)));
        prefs_.set(Prefs::RPC_PORT, tr_sessionGetRPCPort(session_));
        prefs_.set(Prefs::RPC_USERNAME, QString::fromUtf8(tr_sessionGetRPCUsername(session_)));
        prefs_.set(Prefs::RPC_WHITELIST_ENABLED, tr_sessionGetRPCWhitelistEnabled(session_));
        prefs_.set(Prefs::RPC_WHITELIST, QString::fromUtf8(tr_sessionGetRPCWhitelist(session_)));
    }

    if (auto const size = dictFind<int>(args_dict, TR_KEY_blocklist_size); size && *size != blocklistSize())
    {
        setBlocklistSize(*size);
    }

    if (auto const str = dictFind<QString>(args_dict, TR_KEY_version); str)
    {
        session_version_ = *str;
    }

    if (auto const str = dictFind<QString>(args_dict, TR_KEY_session_id); str)
    {
        session_id_ = *str;
        is_definitely_local_session_ = tr_session_id::is_local(session_id_.toUtf8().constData());
    }
    else
    {
        session_id_.clear();
    }

    connect(&prefs_, &Prefs::changed, this, &Session::updatePref);

    emit sessionUpdated();
}

void Session::setBlocklistSize(int64_t i)
{
    blocklist_size_ = i;

    emit blocklistUpdated(i);
}

void Session::addTorrent(AddData add_me, tr_variant* args_dict)
{
    assert(tr_variantDictFind(args_dict, TR_KEY_filename) == nullptr);
    assert(tr_variantDictFind(args_dict, TR_KEY_metainfo) == nullptr);

    if (tr_variantDictFind(args_dict, TR_KEY_paused) == nullptr)
    {
        dictAdd(args_dict, TR_KEY_paused, !prefs_.getBool(Prefs::START));
    }

    switch (add_me.type)
    {
    case AddData::MAGNET:
        dictAdd(args_dict, TR_KEY_filename, add_me.magnet);
        break;

    case AddData::URL:
        dictAdd(args_dict, TR_KEY_filename, add_me.url.toString());
        break;

    case AddData::FILENAME:
        [[fallthrough]];
    case AddData::METAINFO:
        dictAdd(args_dict, TR_KEY_metainfo, add_me.toBase64());
        break;

    default:
        qWarning() << "Unhandled AddData type: " << add_me.type;
        break;
    }

    auto* q = new RpcQueue{};

    q->add(
        [this, args_dict]() { return exec("torrent-add", args_dict); },
        [add_me](RpcResponse const& r)
        {
            auto* d = new QMessageBox{ QMessageBox::Warning,
                                       tr("Error Adding Torrent"),
                                       QStringLiteral("<p><b>%1</b></p><p>%2</p>").arg(r.result).arg(add_me.readableName()),
                                       QMessageBox::Close,
                                       QApplication::activeWindow() };
            QObject::connect(d, &QMessageBox::rejected, d, &QMessageBox::deleteLater);
            d->show();
        });

    q->add(
        [this, add_me](RpcResponse const& r)
        {
            if (tr_variant* dup = nullptr; tr_variantDictFindDict(r.args.get(), TR_KEY_torrent_added, &dup))
            {
                add_me.disposeSourceFile();
            }
            else if (tr_variantDictFindDict(r.args.get(), TR_KEY_torrent_duplicate, &dup))
            {
                add_me.disposeSourceFile();

                if (auto const hash = dictFind<QString>(dup, TR_KEY_hash_string_camel); hash)
                {
                    duplicates_.try_emplace(add_me.readableShortName(), *hash);
                    duplicates_timer_.start(1000);
                }
            }
        });

    q->run();
}

void Session::onDuplicatesTimer()
{
    decltype(duplicates_) duplicates;
    duplicates.swap(duplicates_);

    QStringList lines;
    for (auto [dupe, original] : duplicates)
    {
        lines.push_back(tr("%1 (copy of %2)").arg(dupe).arg(original.left(7)));
    }

    if (!lines.empty())
    {
        lines.sort(Qt::CaseInsensitive);
        auto const title = tr("Duplicate Torrent(s)", "", lines.size());
        auto const detail = lines.join(QStringLiteral("\n"));
        auto const detail_text = tr("Unable to add %n duplicate torrent(s)", "", lines.size());
        auto const use_detail = lines.size() > 1;
        auto const text = use_detail ? detail_text : detail;

        auto* d = new QMessageBox{ QMessageBox::Warning, title, text, QMessageBox::Close, QApplication::activeWindow() };
        if (use_detail)
        {
            d->setDetailedText(detail);
        }

        QObject::connect(d, &QMessageBox::rejected, d, &QMessageBox::deleteLater);
        d->show();
    }
}

void Session::addTorrent(AddData add_me)
{
    tr_variant args;
    tr_variantInitDict(&args, 3);
    addTorrent(std::move(add_me), &args);
}

void Session::addNewlyCreatedTorrent(QString const& filename, QString const& local_path)
{
    QByteArray const b64 = AddData(filename).toBase64();

    tr_variant args;
    tr_variantInitDict(&args, 3);
    dictAdd(&args, TR_KEY_download_dir_kebab, local_path);
    dictAdd(&args, TR_KEY_paused, !prefs_.getBool(Prefs::START));
    dictAdd(&args, TR_KEY_metainfo, b64);

    exec("torrent-add", &args);
}

void Session::removeTorrents(torrent_ids_t const& ids, bool delete_files)
{
    if (!ids.empty())
    {
        tr_variant args;
        tr_variantInitDict(&args, 2);
        addOptionalIds(&args, ids);
        dictAdd(&args, TR_KEY_delete_local_data, delete_files);

        exec("torrent-remove", &args);
    }
}

void Session::verifyTorrents(torrent_ids_t const& ids)
{
    if (!ids.empty())
    {
        tr_variant args;
        tr_variantInitDict(&args, 1);
        addOptionalIds(&args, ids);

        exec("torrent-verify", &args);
    }
}

void Session::reannounceTorrents(torrent_ids_t const& ids)
{
    if (!ids.empty())
    {
        tr_variant args;
        tr_variantInitDict(&args, 1);
        addOptionalIds(&args, ids);

        exec("torrent-reannounce", &args);
    }
}

/***
****
***/

void Session::launchWebInterface() const
{
    QUrl url;

    if (session_ == nullptr) // remote session
    {
        url = rpc_.url();
        url.setPath(QStringLiteral("/transmission/web/"));
    }
    else // local session
    {
        url.setScheme(QStringLiteral("http"));
        url.setHost(QStringLiteral("localhost"));
        url.setPort(prefs_.getInt(Prefs::RPC_PORT));
    }

    QDesktopServices::openUrl(url);
}
