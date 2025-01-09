// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::partial_sort(), std::min(), std::max()
#include <condition_variable>
#include <chrono>
#include <csignal>
#include <cstddef> // size_t
#include <cstdint>
#include <ctime>
#include <future>
#include <iterator> // for std::back_inserter
#include <limits> // std::numeric_limits
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h> /* umask() */
#endif

#include <event2/event.h>

#include <fmt/core.h> // fmt::ptr

#include "libtransmission/transmission.h"

#include "libtransmission/bandwidth.h"
#include "libtransmission/blocklist.h"
#include "libtransmission/cache.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/file.h"
#include "libtransmission/ip-cache.h"
#include "libtransmission/interned-string.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/peer-socket.h"
#include "libtransmission/port-forwarding.h"
#include "libtransmission/quark.h"
#include "libtransmission/rpc-server.h"
#include "libtransmission/session.h"
#include "libtransmission/session-alt-speeds.h"
#include "libtransmission/timer-ev.h"
#include "libtransmission/torrent.h"
#include "libtransmission/torrent-ctor.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-dht.h"
#include "libtransmission/tr-lpd.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/tr-utp.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"
#include "libtransmission/version.h"
#include "libtransmission/web.h"

struct tr_ctor;

using namespace std::literals;
using namespace libtransmission::Values;

namespace
{
namespace bandwidth_group_helpers
{
auto constexpr BandwidthGroupsFilename = "bandwidth-groups.json"sv;

void bandwidthGroupRead(tr_session* session, std::string_view config_dir)
{
    auto const filename = tr_pathbuf{ config_dir, '/', BandwidthGroupsFilename };
    if (!tr_sys_path_exists(filename))
    {
        return;
    }

    auto const groups_var = tr_variant_serde::json().parse_file(filename);
    if (!groups_var)
    {
        return;
    }

    auto const* const groups_map = groups_var->get_if<tr_variant::Map>();
    if (groups_map == nullptr)
    {
        return;
    }

    for (auto const& [key, group_var] : *groups_map)
    {
        auto const* const group_map = group_var.get_if<tr_variant::Map>();
        if (group_map == nullptr)
        {
            continue;
        }

        auto& group = session->getBandwidthGroup(tr_interned_string{ key });
        auto limits = tr_bandwidth_limits{};

        if (auto const val = group_map->value_if<bool>(TR_KEY_uploadLimited))
        {
            limits.up_limited = *val;
        }

        if (auto const val = group_map->value_if<bool>(TR_KEY_downloadLimited))
        {
            limits.down_limited = *val;
        }

        if (auto const val = group_map->value_if<int64_t>(TR_KEY_uploadLimit))
        {
            limits.up_limit = Speed{ *val, Speed::Units::KByps };
        }

        if (auto const val = group_map->value_if<int64_t>(TR_KEY_downloadLimit))
        {
            limits.down_limit = Speed{ *val, Speed::Units::KByps };
        }

        group.set_limits(limits);

        if (auto const val = group_map->value_if<bool>(TR_KEY_honorsSessionLimits))
        {
            group.honor_parent_limits(TR_UP, *val);
            group.honor_parent_limits(TR_DOWN, *val);
        }
    }
}

void bandwidthGroupWrite(tr_session const* session, std::string_view config_dir)
{
    auto const& groups = session->bandwidthGroups();
    auto groups_map = tr_variant::Map{ std::size(groups) };
    for (auto const& [name, group] : groups)
    {
        auto const limits = group->get_limits();
        auto group_map = tr_variant::Map{ 6U };
        group_map.try_emplace(TR_KEY_downloadLimit, limits.down_limit.count(Speed::Units::KByps));
        group_map.try_emplace(TR_KEY_downloadLimited, limits.down_limited);
        group_map.try_emplace(TR_KEY_honorsSessionLimits, group->are_parent_limits_honored(TR_UP));
        group_map.try_emplace(TR_KEY_name, name.sv());
        group_map.try_emplace(TR_KEY_uploadLimit, limits.up_limit.count(Speed::Units::KByps));
        group_map.try_emplace(TR_KEY_uploadLimited, limits.up_limited);
        groups_map.try_emplace(name.quark(), std::move(group_map));
    }

    tr_variant_serde::json().to_file(
        tr_variant{ std::move(groups_map) },
        tr_pathbuf{ config_dir, '/', BandwidthGroupsFilename });
}
} // namespace bandwidth_group_helpers
} // namespace

void tr_session::update_bandwidth(tr_direction const dir)
{
    if (auto const limit = active_speed_limit(dir); limit)
    {
        top_bandwidth_.set_limited(dir, limit->base_quantity() > 0U);
        top_bandwidth_.set_desired_speed(dir, *limit);
    }
    else
    {
        top_bandwidth_.set_limited(dir, false);
    }
}

tr_port tr_session::randomPort() const
{
    auto const lower = std::min(settings_.peer_port_random_low.host(), settings_.peer_port_random_high.host());
    auto const upper = std::max(settings_.peer_port_random_low.host(), settings_.peer_port_random_high.host());
    auto const range = upper - lower;
    return tr_port::from_host(lower + tr_rand_int(range + 1U));
}

/* Generate a peer id : "-TRxyzb-" + 12 random alphanumeric
   characters, where x is the major version number, y is the
   minor version number, z is the maintenance number, and b
   designates beta (Azureus-style) */
tr_peer_id_t tr_peerIdInit()
{
    auto peer_id = tr_peer_id_t{};
    auto* it = std::data(peer_id);

    // starts with -TRXXXX-
    auto constexpr Prefix = std::string_view{ PEERID_PREFIX };
    auto const* const end = it + std::size(peer_id);
    it = std::copy_n(std::data(Prefix), std::size(Prefix), it);

    // remainder is randomly-generated characters
    auto constexpr Pool = std::string_view{ "0123456789abcdefghijklmnopqrstuvwxyz" };
    auto total = 0;
    tr_rand_buffer(it, end - it);
    while (it + 1 < end)
    {
        int const val = *it % std::size(Pool);
        total += val;
        *it++ = Pool[val];
    }
    int const val = total % std::size(Pool) != 0 ? std::size(Pool) - (total % std::size(Pool)) : 0;
    *it = Pool[val];

    return peer_id;
}

// ---

std::vector<tr_torrent_id_t> tr_session::DhtMediator::torrents_allowing_dht() const
{
    auto ids = std::vector<tr_torrent_id_t>{};
    auto const& torrents = session_.torrents();

    ids.reserve(std::size(torrents));
    for (auto const* const tor : torrents)
    {
        if (tor->is_running() && tor->allows_dht())
        {
            ids.push_back(tor->id());
        }
    }

    return ids;
}

tr_sha1_digest_t tr_session::DhtMediator::torrent_info_hash(tr_torrent_id_t id) const
{
    if (auto const* const tor = session_.torrents().get(id); tor != nullptr)
    {
        return tor->info_hash();
    }

    return {};
}

void tr_session::DhtMediator::add_pex(tr_sha1_digest_t const& info_hash, tr_pex const* pex, size_t n_pex)
{
    if (auto* const tor = session_.torrents().get(info_hash); tor != nullptr)
    {
        tr_peerMgrAddPex(tor, TR_PEER_FROM_DHT, pex, n_pex);
    }
}

// ---

bool tr_session::LpdMediator::onPeerFound(std::string_view info_hash_str, tr_address address, tr_port port)
{
    auto const digest = tr_sha1_from_string(info_hash_str);
    if (!digest)
    {
        return false;
    }

    tr_torrent* const tor = session_.torrents_.get(*digest);
    if (!tr_isTorrent(tor) || !tor->allows_lpd())
    {
        return false;
    }

    // we found a suitable peer, add it to the torrent
    auto const socket_address = tr_socket_address{ address, port };
    auto const pex = tr_pex{ socket_address };
    tr_peerMgrAddPex(tor, TR_PEER_FROM_LPD, &pex, 1U);
    tr_logAddDebugTor(tor, fmt::format("Found a local peer from LPD ({:s})", socket_address.display_name()));
    return true;
}

std::vector<tr_lpd::Mediator::TorrentInfo> tr_session::LpdMediator::torrents() const
{
    auto ret = std::vector<tr_lpd::Mediator::TorrentInfo>{};
    ret.reserve(std::size(session_.torrents()));
    for (auto const* const tor : session_.torrents())
    {
        auto info = tr_lpd::Mediator::TorrentInfo{};
        info.info_hash_str = tor->info_hash_string();
        info.activity = tor->activity();
        info.allows_lpd = tor->allows_lpd();
        info.announce_after = tor->lpdAnnounceAt;
        ret.emplace_back(info);
    }
    return ret;
}

void tr_session::LpdMediator::setNextAnnounceTime(std::string_view info_hash_str, time_t announce_after)
{
    if (auto digest = tr_sha1_from_string(info_hash_str); digest)
    {
        if (tr_torrent* const tor = session_.torrents_.get(*digest); tr_isTorrent(tor))
        {
            tor->lpdAnnounceAt = announce_after;
        }
    }
}

// ---

std::optional<std::string> tr_session::WebMediator::cookieFile() const
{
    auto const path = tr_pathbuf{ session_->configDir(), "/cookies.txt"sv };

    if (!tr_sys_path_exists(path))
    {
        return {};
    }

    return std::string{ path };
}

std::optional<std::string_view> tr_session::WebMediator::userAgent() const
{
    return TR_NAME "/" SHORT_VERSION_STRING;
}

std::optional<std::string> tr_session::WebMediator::bind_address_V4() const
{
    if (auto const addr = session_->bind_address(TR_AF_INET); !addr.is_any())
    {
        return addr.display_name();
    }

    return std::nullopt;
}

std::optional<std::string> tr_session::WebMediator::bind_address_V6() const
{
    if (auto const addr = session_->bind_address(TR_AF_INET6); !addr.is_any())
    {
        return addr.display_name();
    }

    return std::nullopt;
}

size_t tr_session::WebMediator::clamp(int torrent_id, size_t byte_count) const
{
    auto const lock = session_->unique_lock();

    auto const* const tor = session_->torrents().get(torrent_id);
    return tor == nullptr ? 0U : tor->bandwidth().clamp(TR_DOWN, byte_count);
}

std::optional<std::string_view> tr_session::WebMediator::proxyUrl() const
{
    return session_->settings_.proxy_url;
}

void tr_session::WebMediator::run(tr_web::FetchDoneFunc&& func, tr_web::FetchResponse&& response) const
{
    session_->run_in_session_thread(std::move(func), std::move(response));
}

time_t tr_session::WebMediator::now() const
{
    return tr_time();
}

void tr_sessionFetch(tr_session* session, tr_web::FetchOptions&& options)
{
    session->fetch(std::move(options));
}

// ---

tr_encryption_mode tr_sessionGetEncryption(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->encryptionMode();
}

void tr_sessionSetEncryption(tr_session* session, tr_encryption_mode mode)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(mode == TR_ENCRYPTION_PREFERRED || mode == TR_ENCRYPTION_REQUIRED || mode == TR_CLEAR_PREFERRED);

    session->settings_.encryption_mode = mode;
}

// ---

void tr_session::onIncomingPeerConnection(tr_socket_t fd, void* vsession)
{
    auto* session = static_cast<tr_session*>(vsession);

    if (auto const incoming_info = tr_netAccept(session, fd); incoming_info)
    {
        auto const& [socket_address, sock] = *incoming_info;
        tr_logAddTrace(fmt::format("new incoming connection {} ({})", sock, socket_address.display_name()));
        session->addIncoming({ session, socket_address, sock });
    }
}

tr_session::BoundSocket::BoundSocket(
    struct event_base* evbase,
    tr_address const& addr,
    tr_port port,
    IncomingCallback cb,
    void* cb_data)
    : cb_{ cb }
    , cb_data_{ cb_data }
    , socket_{ tr_netBindTCP(addr, port, false) }
    , ev_{ event_new(evbase, socket_, EV_READ | EV_PERSIST, &BoundSocket::onCanRead, this) }
{
    if (socket_ == TR_BAD_SOCKET)
    {
        return;
    }

    tr_logAddInfo(fmt::format(
        _("Listening to incoming peer connections on {hostport}"),
        fmt::arg("hostport", tr_socket_address::display_name(addr, port))));
    event_add(ev_.get(), nullptr);
}

tr_session::BoundSocket::~BoundSocket()
{
    ev_.reset();

    if (socket_ != TR_BAD_SOCKET)
    {
        tr_net_close_socket(socket_);
        socket_ = TR_BAD_SOCKET;
    }
}

tr_address tr_session::bind_address(tr_address_type type) const noexcept
{
    if (type == TR_AF_INET)
    {
        // if user provided an address, use it.
        // otherwise, use any_ipv4 (0.0.0.0).
        return ip_cache_.bind_addr(type);
    }

    if (type == TR_AF_INET6)
    {
        // if user provided an address, use it.
        // otherwise, if we can determine which one to use via global_source_address(ipv6) magic, use it.
        // otherwise, use any_ipv6 (::).
        auto const source_addr = global_source_address(type);
        auto const default_addr = source_addr && source_addr->is_global_unicast_address() ? *source_addr :
                                                                                            tr_address::any(TR_AF_INET6);
        return tr_address::from_string(settings_.bind_address_ipv6).value_or(default_addr);
    }

    TR_ASSERT_MSG(false, "invalid type");
    return {};
}

// ---

tr_variant tr_sessionGetDefaultSettings()
{
    auto ret = tr_variant::make_map();
    ret.merge(tr_rpc_server::Settings{}.save());
    ret.merge(tr_session_alt_speeds::Settings{}.save());
    ret.merge(tr_session::Settings{}.save());
    return ret;
}

tr_variant tr_sessionGetSettings(tr_session const* session)
{
    auto settings = tr_variant::make_map();
    settings.merge(session->alt_speeds_.settings().save());
    settings.merge(session->rpc_server_->settings().save());
    settings.merge(session->settings_.save());
    tr_variantDictAddInt(&settings, TR_KEY_message_level, tr_logGetLevel());
    return settings;
}

tr_variant tr_sessionLoadSettings(tr_variant const* app_defaults, char const* config_dir, char const* app_name)
{
    auto settings = tr_sessionGetDefaultSettings();

    // if app defaults are provided, override libtransmission defaults
    if (app_defaults != nullptr && app_defaults->holds_alternative<tr_variant::Map>())
    {
        settings.merge(*app_defaults);
    }

    // if a settings file exists, use it to override the defaults
    if (auto const filename = fmt::format(
            "{:s}/settings.json",
            config_dir != nullptr ? config_dir : tr_getDefaultConfigDir(app_name));
        tr_sys_path_exists(filename))
    {
        if (auto file_settings = tr_variant_serde::json().parse_file(filename); file_settings)
        {
            settings.merge(*file_settings);
        }
    }

    return settings;
}

void tr_sessionSaveSettings(tr_session* session, char const* config_dir, tr_variant const& client_settings)
{
    using namespace bandwidth_group_helpers;

    TR_ASSERT(client_settings.holds_alternative<tr_variant::Map>());

    auto const filename = tr_pathbuf{ config_dir, "/settings.json"sv };

    // from highest to lowest precedence:
    // - actual values
    // - client settings
    // - previous session's settings stored in settings.json
    // - built-in defaults
    auto settings = tr_sessionGetDefaultSettings();
    if (auto const file_settings = tr_variant_serde::json().parse_file(filename); file_settings)
    {
        settings.merge(*file_settings);
    }
    settings.merge(client_settings);
    settings.merge(tr_sessionGetSettings(session));

    // save 'em
    tr_variant_serde::json().to_file(settings, filename);

    // write bandwidth groups limits to file
    bandwidthGroupWrite(session, config_dir);
}

// ---

struct tr_session::init_data
{
    init_data(bool message_queuing_enabled_in, std::string_view config_dir_in, tr_variant const& settings_in)
        : message_queuing_enabled{ message_queuing_enabled_in }
        , config_dir{ config_dir_in }
        , settings{ settings_in }
    {
    }

    bool message_queuing_enabled;
    std::string_view config_dir;
    tr_variant const& settings;

    std::condition_variable_any done_cv;
};

tr_session* tr_sessionInit(char const* config_dir, bool message_queueing_enabled, tr_variant const& client_settings)
{
    using namespace bandwidth_group_helpers;

    TR_ASSERT(config_dir != nullptr);
    TR_ASSERT(client_settings.holds_alternative<tr_variant::Map>());

    tr_timeUpdate(time(nullptr));

    // settings order of precedence from highest to lowest:
    // - client settings
    // - previous session's values in settings.json
    // - hardcoded defaults
    auto settings = tr_sessionLoadSettings(nullptr, config_dir, nullptr);
    settings.merge(client_settings);

    // if logging is desired, start it now before doing more work
    if (auto const* settings_map = settings.get_if<tr_variant::Map>(); settings_map != nullptr)
    {
        if (auto const val = settings_map->value_if<bool>(TR_KEY_message_level))
        {
            tr_logSetLevel(static_cast<tr_log_level>(*val));
        }
    }

    // initialize the bare skeleton of the session object
    auto* const session = new tr_session{ config_dir, tr_variant::make_map() };
    bandwidthGroupRead(session, config_dir);

    // run initImpl() in the libtransmission thread
    auto data = tr_session::init_data{ message_queueing_enabled, config_dir, settings };
    auto lock = session->unique_lock();
    session->run_in_session_thread([&session, &data]() { session->initImpl(data); });
    data.done_cv.wait(lock); // wait for the session to be ready

    return session;
}

void tr_session::on_now_timer()
{
    TR_ASSERT(now_timer_);
    auto const now = std::chrono::system_clock::now();

    // tr_session upkeep tasks to perform once per second
    tr_timeUpdate(std::chrono::system_clock::to_time_t(now));
    alt_speeds_.check_scheduler();

    // set the timer to kick again right after (10ms after) the next second
    auto const target_time = std::chrono::time_point_cast<std::chrono::seconds>(now) + 1s + 10ms;
    auto target_interval = target_time - now;
    if (target_interval < 100ms)
    {
        target_interval += 1s;
    }
    now_timer_->set_interval(std::chrono::duration_cast<std::chrono::milliseconds>(target_interval));
}

namespace
{
namespace queue_helpers
{
std::vector<tr_torrent*> get_next_queued_torrents(tr_torrents& torrents, tr_direction dir, size_t num_wanted)
{
    TR_ASSERT(tr_isDirection(dir));

    auto candidates = torrents.get_matching([dir](auto const* const tor) { return tor->is_queued(dir); });

    // find the best n candidates
    num_wanted = std::min(num_wanted, std::size(candidates));
    if (num_wanted < candidates.size())
    {
        std::partial_sort(
            std::begin(candidates),
            std::begin(candidates) + num_wanted,
            std::end(candidates),
            tr_torrent::CompareQueuePosition);
        candidates.resize(num_wanted);
    }

    return candidates;
}
} // namespace queue_helpers
} // namespace

size_t tr_session::count_queue_free_slots(tr_direction dir) const noexcept
{
    if (!queueEnabled(dir))
    {
        return std::numeric_limits<size_t>::max();
    }

    auto const max = queueSize(dir);
    auto const activity = dir == TR_UP ? TR_STATUS_SEED : TR_STATUS_DOWNLOAD;

    // count how many torrents are active
    auto active_count = size_t{};
    auto const stalled_enabled = queueStalledEnabled();
    auto const stalled_if_idle_for_n_seconds = queueStalledMinutes() * 60;
    auto const now = tr_time();
    for (auto const* const tor : torrents())
    {
        // is it the right activity?
        if (activity != tor->activity())
        {
            continue;
        }

        // is it stalled?
        if (stalled_enabled)
        {
            auto const idle_seconds = tor->idle_seconds(now);
            if (idle_seconds && *idle_seconds >= stalled_if_idle_for_n_seconds)
            {
                continue;
            }
        }

        ++active_count;

        /* if we've reached the limit, no need to keep counting */
        if (active_count >= max)
        {
            return 0;
        }
    }

    return max - active_count;
}

void tr_session::on_queue_timer()
{
    using namespace queue_helpers;

    for (auto const dir : { TR_UP, TR_DOWN })
    {
        if (!queueEnabled(dir))
        {
            continue;
        }

        auto const n_wanted = count_queue_free_slots(dir);

        for (auto* tor : get_next_queued_torrents(torrents(), dir, n_wanted))
        {
            tr_torrentStartNow(tor);

            if (queue_start_callback_ != nullptr)
            {
                queue_start_callback_(this, tor, queue_start_user_data_);
            }
        }
    }
}

// Periodically save the .resume files of any torrents whose
// status has recently changed. This prevents loss of metadata
// in the case of a crash, unclean shutdown, clumsy user, etc.
void tr_session::on_save_timer()
{
    for (auto* const tor : torrents())
    {
        tor->save_resume_file();
    }

    stats().save();
}

void tr_session::initImpl(init_data& data)
{
    auto lock = unique_lock();
    TR_ASSERT(am_in_session_thread());

    auto const& settings = data.settings;
    TR_ASSERT(settings.holds_alternative<tr_variant::Map>());

    tr_logAddTrace(fmt::format("tr_sessionInit: the session's top-level bandwidth object is {}", fmt::ptr(&top_bandwidth_)));

#ifndef _WIN32
    /* Don't exit when writing on a broken socket */
    (void)signal(SIGPIPE, SIG_IGN);
#endif

    tr_logSetQueueEnabled(data.message_queuing_enabled);

    blocklists_.load(blocklist_dir_, blocklist_enabled());

    tr_logAddInfo(fmt::format(_("Transmission version {version} starting"), fmt::arg("version", LONG_VERSION_STRING)));

    setSettings(settings, true);

    tr_utp_init(this);

    /* cleanup */
    data.done_cv.notify_one();
}

void tr_session::setSettings(tr_variant const& settings, bool force)
{
    TR_ASSERT(am_in_session_thread());
    TR_ASSERT(settings.holds_alternative<tr_variant::Map>());

    setSettings(tr_session::Settings{ settings }, force);

    // delegate loading out the other settings
    alt_speeds_.load(tr_session_alt_speeds::Settings{ settings });
    rpc_server_->load(tr_rpc_server::Settings{ settings });
}

void tr_session::setSettings(tr_session::Settings&& settings_in, bool force)
{
    auto const lock = unique_lock();

    std::swap(settings_, settings_in);
    auto const& new_settings = settings_;
    auto const& old_settings = settings_in;

    // the rest of the func is session_ responding to settings changes

    if (auto const& val = new_settings.log_level; force || val != old_settings.log_level)
    {
        tr_logSetLevel(val);
    }

#ifndef _WIN32
    if (auto const& val = new_settings.umask; force || val != old_settings.umask)
    {
        ::umask(val);
    }
#endif

    if (auto const& val = new_settings.cache_size_mbytes; force || val != old_settings.cache_size_mbytes)
    {
        tr_sessionSetCacheLimit_MB(this, val);
    }

    if (auto const& val = new_settings.bind_address_ipv4; force || val != old_settings.bind_address_ipv4)
    {
        ip_cache_.update_addr(TR_AF_INET);
    }
    if (auto const& val = new_settings.bind_address_ipv6; force || val != old_settings.bind_address_ipv6)
    {
        ip_cache_.update_addr(TR_AF_INET6);
    }

    if (auto const& val = new_settings.default_trackers_str; force || val != old_settings.default_trackers_str)
    {
        setDefaultTrackers(val);
    }

    bool const utp_changed = new_settings.utp_enabled != old_settings.utp_enabled;

    set_blocklist_enabled(new_settings.blocklist_enabled);

    auto local_peer_port = force && settings_.peer_port_random_on_start ? randomPort() : new_settings.peer_port;
    bool port_changed = false;
    if (force || local_peer_port_ != local_peer_port)
    {
        local_peer_port_ = local_peer_port;
        advertised_peer_port_ = local_peer_port;
        port_changed = true;
    }

    bool addr_changed = false;
    if (new_settings.tcp_enabled)
    {
        if (auto const& val = new_settings.bind_address_ipv4; force || port_changed || val != old_settings.bind_address_ipv4)
        {
            auto const addr = bind_address(TR_AF_INET);
            bound_ipv4_.emplace(event_base(), addr, local_peer_port_, &tr_session::onIncomingPeerConnection, this);
            addr_changed = true;
        }

        if (auto const& val = new_settings.bind_address_ipv6; force || port_changed || val != old_settings.bind_address_ipv6)
        {
            auto const addr = bind_address(TR_AF_INET6);
            bound_ipv6_.emplace(event_base(), addr, local_peer_port_, &tr_session::onIncomingPeerConnection, this);
            addr_changed = true;
        }
    }
    else
    {
        bound_ipv4_.reset();
        bound_ipv6_.reset();
        addr_changed = true;
    }

    if (auto const& val = new_settings.port_forwarding_enabled; force || val != old_settings.port_forwarding_enabled)
    {
        tr_sessionSetPortForwardingEnabled(this, val);
    }

    if (port_changed)
    {
        port_forwarding_->local_port_changed();
    }

    if (!udp_core_ || force || port_changed || utp_changed)
    {
        udp_core_ = std::make_unique<tr_session::tr_udp_core>(*this, udpPort());
    }

    // Sends out announce messages with advertisedPeerPort(), so this
    // section needs to happen here after the peer port settings changes
    if (auto const& val = new_settings.lpd_enabled; force || val != old_settings.lpd_enabled)
    {
        if (val)
        {
            lpd_ = tr_lpd::create(lpd_mediator_, event_base());
        }
        else
        {
            lpd_.reset();
        }
    }

    if (!new_settings.dht_enabled)
    {
        dht_.reset();
    }
    else if (force || !dht_ || port_changed || addr_changed || new_settings.dht_enabled != old_settings.dht_enabled)
    {
        dht_ = tr_dht::create(dht_mediator_, localPeerPort(), udp_core_->socket4(), udp_core_->socket6());
    }

    if (auto const& val = new_settings.sleep_per_seconds_during_verify;
        force || val != old_settings.sleep_per_seconds_during_verify)
    {
        verifier_->set_sleep_per_seconds_during_verify(val);
    }

    // We need to update bandwidth if speed settings changed.
    // It's a harmless call, so just call it instead of checking for settings changes
    update_bandwidth(TR_UP);
    update_bandwidth(TR_DOWN);
}

void tr_sessionSet(tr_session* session, tr_variant const& settings)
{
    // do the work in the session thread
    auto done_promise = std::promise<void>{};
    auto done_future = done_promise.get_future();
    session->run_in_session_thread(
        [&session, &settings, &done_promise]()
        {
            session->setSettings(settings, false);
            done_promise.set_value();
        });
    done_future.wait();
}

// ---

void tr_sessionSetDownloadDir(tr_session* session, char const* dir)
{
    TR_ASSERT(session != nullptr);

    session->setDownloadDir(dir != nullptr ? dir : "");
}

char const* tr_sessionGetDownloadDir(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->downloadDir().c_str();
}

char const* tr_sessionGetConfigDir(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->configDir().c_str();
}

// ---

void tr_sessionSetIncompleteFileNamingEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->settings_.is_incomplete_file_naming_enabled = enabled;
}

bool tr_sessionIsIncompleteFileNamingEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->isIncompleteFileNamingEnabled();
}

// ---

void tr_sessionSetIncompleteDir(tr_session* session, char const* dir)
{
    TR_ASSERT(session != nullptr);

    session->setIncompleteDir(dir != nullptr ? dir : "");
}

char const* tr_sessionGetIncompleteDir(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->incompleteDir().c_str();
}

void tr_sessionSetIncompleteDirEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->useIncompleteDir(enabled);
}

bool tr_sessionIsIncompleteDirEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->useIncompleteDir();
}

// --- Peer Port

void tr_sessionSetPeerPort(tr_session* session, uint16_t hport)
{
    TR_ASSERT(session != nullptr);

    if (auto const port = tr_port::from_host(hport); port != session->localPeerPort())
    {
        session->run_in_session_thread(
            [session, port]()
            {
                auto settings = session->settings_;
                settings.peer_port = port;
                session->setSettings(std::move(settings), false);
            });
    }
}

uint16_t tr_sessionGetPeerPort(tr_session const* session)
{
    return session != nullptr ? session->localPeerPort().host() : 0U;
}

uint16_t tr_sessionSetPeerPortRandom(tr_session* session)
{
    auto const p = session->randomPort();
    tr_sessionSetPeerPort(session, p.host());
    return p.host();
}

void tr_sessionSetPeerPortRandomOnStart(tr_session* session, bool random)
{
    TR_ASSERT(session != nullptr);

    session->settings_.peer_port_random_on_start = random;
}

bool tr_sessionGetPeerPortRandomOnStart(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->isPortRandom();
}

tr_port_forwarding_state tr_sessionGetPortForwarding(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->port_forwarding_->state();
}

void tr_session::onAdvertisedPeerPortChanged()
{
    for (auto* const tor : torrents())
    {
        tr_torrentChangeMyPort(tor);
    }
}

// ---

void tr_sessionSetRatioLimited(tr_session* session, bool is_limited)
{
    TR_ASSERT(session != nullptr);

    session->settings_.ratio_limit_enabled = is_limited;
}

void tr_sessionSetRatioLimit(tr_session* session, double desired_ratio)
{
    TR_ASSERT(session != nullptr);

    session->settings_.ratio_limit = desired_ratio;
}

bool tr_sessionIsRatioLimited(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->isRatioLimited();
}

double tr_sessionGetRatioLimit(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->desiredRatio();
}

// ---

void tr_sessionSetIdleLimited(tr_session* session, bool is_limited)
{
    TR_ASSERT(session != nullptr);

    session->settings_.idle_seeding_limit_enabled = is_limited;
}

void tr_sessionSetIdleLimit(tr_session* session, uint16_t idle_minutes)
{
    TR_ASSERT(session != nullptr);

    session->settings_.idle_seeding_limit_minutes = idle_minutes;
}

bool tr_sessionIsIdleLimited(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->isIdleLimited();
}

uint16_t tr_sessionGetIdleLimit(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->idleLimitMinutes();
}

// --- Speed limits

std::optional<Speed> tr_session::active_speed_limit(tr_direction dir) const noexcept
{
    if (tr_sessionUsesAltSpeed(this))
    {
        return alt_speeds_.speed_limit(dir);
    }

    if (is_speed_limited(dir))
    {
        return speed_limit(dir);
    }

    return {};
}

time_t tr_session::AltSpeedMediator::time()
{
    return tr_time();
}

void tr_session::AltSpeedMediator::is_active_changed(bool is_active, tr_session_alt_speeds::ChangeReason reason)
{
    auto const in_session_thread = [session = &session_, is_active, reason]()
    {
        session->update_bandwidth(TR_UP);
        session->update_bandwidth(TR_DOWN);

        if (session->alt_speed_active_changed_func_ != nullptr)
        {
            session->alt_speed_active_changed_func_(
                session,
                is_active,
                reason == tr_session_alt_speeds::ChangeReason::User,
                session->alt_speed_active_changed_func_user_data_);
        }
    };

    session_.run_in_session_thread(in_session_thread);
}

// --- Session primary speed limits

void tr_sessionSetSpeedLimit_KBps(tr_session* const session, tr_direction const dir, size_t const limit_kbyps)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    session->set_speed_limit(dir, Speed{ limit_kbyps, Speed::Units::KByps });
}

size_t tr_sessionGetSpeedLimit_KBps(tr_session const* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    return session->speed_limit(dir).count(Speed::Units::KByps);
}

void tr_sessionLimitSpeed(tr_session* session, tr_direction const dir, bool limited)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    if (dir == TR_DOWN)
    {
        session->settings_.speed_limit_down_enabled = limited;
    }
    else
    {
        session->settings_.speed_limit_up_enabled = limited;
    }

    session->update_bandwidth(dir);
}

bool tr_sessionIsSpeedLimited(tr_session const* session, tr_direction const dir)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    return session->is_speed_limited(dir);
}

// --- Session alt speed limits

void tr_sessionSetAltSpeed_KBps(tr_session* const session, tr_direction const dir, size_t const limit_kbyps)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    session->alt_speeds_.set_speed_limit(dir, Speed{ limit_kbyps, Speed::Units::KByps });
    session->update_bandwidth(dir);
}

size_t tr_sessionGetAltSpeed_KBps(tr_session const* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    return session->alt_speeds_.speed_limit(dir).count(Speed::Units::KByps);
}

void tr_sessionUseAltSpeedTime(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->alt_speeds_.set_scheduler_enabled(enabled);
}

bool tr_sessionUsesAltSpeedTime(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.is_scheduler_enabled();
}

void tr_sessionSetAltSpeedBegin(tr_session* session, size_t minutes_since_midnight)
{
    TR_ASSERT(session != nullptr);

    session->alt_speeds_.set_start_minute(minutes_since_midnight);
}

size_t tr_sessionGetAltSpeedBegin(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.start_minute();
}
void tr_sessionSetAltSpeedEnd(tr_session* session, size_t minutes_since_midnight)
{
    TR_ASSERT(session != nullptr);

    session->alt_speeds_.set_end_minute(minutes_since_midnight);
}

size_t tr_sessionGetAltSpeedEnd(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.end_minute();
}

void tr_sessionSetAltSpeedDay(tr_session* session, tr_sched_day days)
{
    TR_ASSERT(session != nullptr);

    session->alt_speeds_.set_weekdays(days);
}

tr_sched_day tr_sessionGetAltSpeedDay(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.weekdays();
}

void tr_sessionUseAltSpeed(tr_session* session, bool enabled)
{
    session->alt_speeds_.set_active(enabled, tr_session_alt_speeds::ChangeReason::User);
}

bool tr_sessionUsesAltSpeed(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.is_active();
}

void tr_sessionSetAltSpeedFunc(tr_session* session, tr_altSpeedFunc func, void* user_data)
{
    TR_ASSERT(session != nullptr);

    session->alt_speed_active_changed_func_ = func;
    session->alt_speed_active_changed_func_user_data_ = user_data;
}

// ---

void tr_sessionSetPeerLimit(tr_session* session, uint16_t max_global_peers)
{
    TR_ASSERT(session != nullptr);

    session->settings_.peer_limit_global = max_global_peers;
}

uint16_t tr_sessionGetPeerLimit(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->peerLimit();
}

void tr_sessionSetPeerLimitPerTorrent(tr_session* session, uint16_t max_peers)
{
    TR_ASSERT(session != nullptr);

    session->settings_.peer_limit_per_torrent = max_peers;
}

uint16_t tr_sessionGetPeerLimitPerTorrent(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->peerLimitPerTorrent();
}

// ---

void tr_sessionSetPaused(tr_session* session, bool is_paused)
{
    TR_ASSERT(session != nullptr);

    session->settings_.should_start_added_torrents = !is_paused;
}

bool tr_sessionGetPaused(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->shouldPauseAddedTorrents();
}

void tr_sessionSetDeleteSource(tr_session* session, bool delete_source)
{
    TR_ASSERT(session != nullptr);

    session->settings_.should_delete_source_torrents = delete_source;
}

// ---

double tr_sessionGetRawSpeed_KBps(tr_session const* session, tr_direction dir)
{
    if (session != nullptr)
    {
        return session->top_bandwidth_.get_raw_speed(0, dir).count(Speed::Units::KByps);
    }

    return {};
}

void tr_session::closeImplPart1(std::promise<void>* closed_promise, std::chrono::time_point<std::chrono::steady_clock> deadline)
{
    is_closing_ = true;

    // close the low-hanging fruit that can be closed immediately w/o consequences
    utp_timer.reset();
    verifier_.reset();
    save_timer_.reset();
    queue_timer_.reset();
    now_timer_.reset();
    rpc_server_.reset();
    dht_.reset();
    lpd_.reset();

    port_forwarding_.reset();
    bound_ipv6_.reset();
    bound_ipv4_.reset();

    // Close the torrents in order of most active to least active
    // so that the most important announce=stopped events are
    // fired out first...
    auto torrents = torrents_.get_all();
    std::sort(
        std::begin(torrents),
        std::end(torrents),
        [](auto const* a, auto const* b)
        {
            auto const a_cur = a->bytes_downloaded_.ever();
            auto const b_cur = b->bytes_downloaded_.ever();
            return a_cur > b_cur; // larger xfers go first
        });
    for (auto* tor : torrents)
    {
        tr_torrentFreeInSessionThread(tor);
    }
    torrents.clear();
    // ...now that all the torrents have been closed, any remaining
    // `&event=stopped` announce messages are queued in the announcer.
    // Tell the announcer to start shutdown, which sends out the stop
    // events and stops scraping.
    this->announcer_->startShutdown();
    // ...since global_ip_cache_ relies on web_ to update global addresses,
    // we tell it to stop updating before web_ starts to refuse new requests.
    // But we keep it intact for now, so that udp_core_ can continue.
    this->ip_cache_.try_shutdown();
    // ...and now that those are done, tell web_ that we're shutting
    // down soon. This leaves the `event=stopped` going but refuses any
    // new tasks.
    this->web_->startShutdown(10s);
    this->cache.reset();

    // recycle the now-unused save_timer_ here to wait for UDP shutdown
    TR_ASSERT(!save_timer_);
    save_timer_ = timerMaker().create([this, closed_promise, deadline]() { closeImplPart2(closed_promise, deadline); });
    save_timer_->start_repeating(50ms);
}

void tr_session::closeImplPart2(std::promise<void>* closed_promise, std::chrono::time_point<std::chrono::steady_clock> deadline)
{
    // try to keep web_ and the UDP announcer alive long enough to send out
    // all the &event=stopped tracker announces.
    // also wait for all ip cache updates to finish so that web_ can
    // safely destruct.
    if ((!web_->is_idle() || !announcer_udp_->is_idle() || !ip_cache_.try_shutdown()) &&
        std::chrono::steady_clock::now() < deadline)
    {
        announcer_->upkeep();
        return;
    }

    save_timer_.reset();

    this->announcer_.reset();
    this->announcer_udp_.reset();

    stats().save();
    peer_mgr_.reset();
    openFiles().close_all();
    tr_utp_close(this);
    this->udp_core_.reset();

    // tada we are done!
    closed_promise->set_value();
}

void tr_sessionClose(tr_session* session, size_t timeout_secs)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(!session->am_in_session_thread());

    tr_logAddInfo(fmt::format(_("Transmission version {version} shutting down"), fmt::arg("version", LONG_VERSION_STRING)));

    auto closed_promise = std::promise<void>{};
    auto closed_future = closed_promise.get_future();
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ timeout_secs };
    session->run_in_session_thread([&closed_promise, deadline, session]()
                                   { session->closeImplPart1(&closed_promise, deadline); });
    closed_future.wait();

    delete session;
}

namespace
{
namespace load_torrents_helpers
{
void session_load_torrents(tr_session* session, tr_ctor* ctor, std::promise<size_t>* loaded_promise)
{
    auto n_torrents = size_t{};
    auto const& folder = session->torrentDir();

    for (auto const& name : tr_sys_dir_get_files(folder, [](auto name) { return tr_strv_ends_with(name, ".torrent"sv); }))
    {
        auto const path = tr_pathbuf{ folder, '/', name };

        if (ctor->set_metainfo_from_file(path.sv()) && tr_torrentNew(ctor, nullptr) != nullptr)
        {
            ++n_torrents;
        }
    }

    auto buf = std::vector<char>{};
    for (auto const& name : tr_sys_dir_get_files(folder, [](auto name) { return tr_strv_ends_with(name, ".magnet"sv); }))
    {
        auto const path = tr_pathbuf{ folder, '/', name };

        if (tr_file_read(path, buf) &&
            ctor->set_metainfo_from_magnet_link(std::string_view{ std::data(buf), std::size(buf) }, nullptr) &&
            tr_torrentNew(ctor, nullptr) != nullptr)
        {
            ++n_torrents;
        }
    }

    if (n_torrents != 0U)
    {
        tr_logAddInfo(fmt::format(
            tr_ngettext("Loaded {count} torrent", "Loaded {count} torrents", n_torrents),
            fmt::arg("count", n_torrents)));
    }

    loaded_promise->set_value(n_torrents);
}
} // namespace load_torrents_helpers
} // namespace

size_t tr_sessionLoadTorrents(tr_session* session, tr_ctor* ctor)
{
    using namespace load_torrents_helpers;

    auto loaded_promise = std::promise<size_t>{};
    auto loaded_future = loaded_promise.get_future();

    session->run_in_session_thread(session_load_torrents, session, ctor, &loaded_promise);
    loaded_future.wait();
    auto const n_torrents = loaded_future.get();

    return n_torrents;
}

size_t tr_sessionGetAllTorrents(tr_session* session, tr_torrent** buf, size_t buflen)
{
    auto& torrents = session->torrents();
    auto const n = std::size(torrents);

    if (buflen >= n)
    {
        std::copy_n(std::begin(torrents), n, buf);
    }

    return n;
}

// ---

void tr_sessionSetPexEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->settings_.pex_enabled = enabled;
}

bool tr_sessionIsPexEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->allows_pex();
}

bool tr_sessionIsDHTEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->allowsDHT();
}

void tr_sessionSetDHTEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    if (enabled != session->allowsDHT())
    {
        session->run_in_session_thread(
            [session, enabled]()
            {
                auto settings = session->settings_;
                settings.dht_enabled = enabled;
                session->setSettings(std::move(settings), false);
            });
    }
}

// ---

bool tr_session::allowsUTP() const noexcept
{
#ifdef WITH_UTP
    return settings_.utp_enabled;
#else
    return false;
#endif
}

bool tr_sessionIsUTPEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->allowsUTP();
}

void tr_sessionSetUTPEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    if (enabled == session->allowsUTP())
    {
        return;
    }

    session->run_in_session_thread(
        [session, enabled]()
        {
            auto settings = session->settings_;
            settings.utp_enabled = enabled;
            session->setSettings(std::move(settings), false);
        });
}

void tr_sessionSetLPDEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    if (enabled != session->allowsLPD())
    {
        session->run_in_session_thread(
            [session, enabled]()
            {
                auto settings = session->settings_;
                settings.lpd_enabled = enabled;
                session->setSettings(std::move(settings), false);
            });
    }
}

bool tr_sessionIsLPDEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->allowsLPD();
}

// ---

void tr_sessionSetCacheLimit_MB(tr_session* session, size_t mbytes)
{
    TR_ASSERT(session != nullptr);

    session->settings_.cache_size_mbytes = mbytes;
    session->cache->set_limit(Memory{ mbytes, Memory::Units::MBytes });
}

size_t tr_sessionGetCacheLimit_MB(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->settings_.cache_size_mbytes;
}

// ---

void tr_session::setDefaultTrackers(std::string_view trackers)
{
    auto const oldval = default_trackers_;

    settings_.default_trackers_str = trackers;
    default_trackers_.parse(trackers);

    // if the list changed, update all the public torrents
    if (default_trackers_ != oldval)
    {
        for (auto* const tor : torrents())
        {
            if (tor->is_public())
            {
                announcer_->resetTorrent(tor);
            }
        }
    }
}

void tr_sessionSetDefaultTrackers(tr_session* session, char const* trackers)
{
    TR_ASSERT(session != nullptr);

    session->setDefaultTrackers(trackers != nullptr ? trackers : "");
}

// ---

tr_bandwidth& tr_session::getBandwidthGroup(std::string_view name)
{
    auto& groups = this->bandwidth_groups_;

    for (auto const& [group_name, group] : groups)
    {
        if (group_name == name)
        {
            return *group;
        }
    }

    auto& [group_name, group] = groups.emplace_back(name, std::make_unique<tr_bandwidth>(&top_bandwidth_, true));
    return *group;
}

// ---

void tr_sessionSetPortForwardingEnabled(tr_session* session, bool enabled)
{
    session->run_in_session_thread(
        [session, enabled]()
        {
            session->settings_.port_forwarding_enabled = enabled;
            session->port_forwarding_->set_enabled(enabled);
        });
}

bool tr_sessionIsPortForwardingEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->port_forwarding_->is_enabled();
}

// ---

void tr_sessionReloadBlocklists(tr_session* session)
{
    session->blocklists_.load(session->blocklist_dir_, session->blocklist_enabled());
}

size_t tr_blocklistGetRuleCount(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->blocklists_.num_rules();
}

bool tr_blocklistIsEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->blocklist_enabled();
}

void tr_blocklistSetEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->set_blocklist_enabled(enabled);
}

bool tr_blocklistExists(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->blocklists_.num_lists() > 0U;
}

size_t tr_blocklistSetContent(tr_session* session, char const* content_filename)
{
    auto const lock = session->unique_lock();
    return session->blocklists_.update_primary_blocklist(content_filename, session->blocklist_enabled());
}

void tr_blocklistSetURL(tr_session* session, char const* url)
{
    session->setBlocklistUrl(url != nullptr ? url : "");
}

char const* tr_blocklistGetURL(tr_session const* session)
{
    return session->blocklistUrl().c_str();
}

// ---

void tr_session::setRpcWhitelist(std::string_view whitelist) const
{
    this->rpc_server_->set_whitelist(whitelist);
}

void tr_session::useRpcWhitelist(bool enabled) const
{
    this->rpc_server_->set_whitelist_enabled(enabled);
}

bool tr_session::useRpcWhitelist() const
{
    return this->rpc_server_->is_whitelist_enabled();
}

void tr_sessionSetRPCEnabled(tr_session* session, bool is_enabled)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_enabled(is_enabled);
}

bool tr_sessionIsRPCEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->is_enabled();
}

void tr_sessionSetRPCPort(tr_session* session, uint16_t hport)
{
    TR_ASSERT(session != nullptr);

    if (session->rpc_server_)
    {
        session->rpc_server_->set_port(tr_port::from_host(hport));
    }
}

uint16_t tr_sessionGetRPCPort(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_ ? session->rpc_server_->port().host() : uint16_t{};
}

void tr_sessionSetRPCCallback(tr_session* session, tr_rpc_func func, void* user_data)
{
    TR_ASSERT(session != nullptr);

    session->rpc_func_ = func;
    session->rpc_func_user_data_ = user_data;
}

void tr_sessionSetRPCWhitelist(tr_session* session, char const* whitelist)
{
    TR_ASSERT(session != nullptr);

    session->setRpcWhitelist(whitelist != nullptr ? whitelist : "");
}

char const* tr_sessionGetRPCWhitelist(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->whitelist().c_str();
}

void tr_sessionSetRPCWhitelistEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->useRpcWhitelist(enabled);
}

bool tr_sessionGetRPCWhitelistEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->useRpcWhitelist();
}

void tr_sessionSetRPCPassword(tr_session* session, char const* password)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_password(password != nullptr ? password : "");
}

char const* tr_sessionGetRPCPassword(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->get_salted_password().c_str();
}

void tr_sessionSetRPCUsername(tr_session* session, char const* username)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_username(username != nullptr ? username : "");
}

char const* tr_sessionGetRPCUsername(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->username().c_str();
}

void tr_sessionSetRPCPasswordEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_password_enabled(enabled);
}

bool tr_sessionIsRPCPasswordEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->is_password_enabled();
}

// ---

void tr_sessionSetScriptEnabled(tr_session* session, TrScript type, bool enabled)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(type < TR_SCRIPT_N_TYPES);

    session->useScript(type, enabled);
}

bool tr_sessionIsScriptEnabled(tr_session const* session, TrScript type)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(type < TR_SCRIPT_N_TYPES);

    return session->useScript(type);
}

void tr_sessionSetScript(tr_session* session, TrScript type, char const* script)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(type < TR_SCRIPT_N_TYPES);

    session->setScript(type, script != nullptr ? script : "");
}

char const* tr_sessionGetScript(tr_session const* session, TrScript type)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(type < TR_SCRIPT_N_TYPES);

    return session->script(type).c_str();
}

// ---

void tr_sessionSetQueueSize(tr_session* session, tr_direction dir, size_t max_simultaneous_torrents)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    if (dir == TR_DOWN)
    {
        session->settings_.download_queue_size = max_simultaneous_torrents;
    }
    else
    {
        session->settings_.seed_queue_size = max_simultaneous_torrents;
    }
}

size_t tr_sessionGetQueueSize(tr_session const* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    return session->queueSize(dir);
}

void tr_sessionSetQueueEnabled(tr_session* session, tr_direction dir, bool do_limit_simultaneous_torrents)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    if (dir == TR_DOWN)
    {
        session->settings_.download_queue_enabled = do_limit_simultaneous_torrents;
    }
    else
    {
        session->settings_.seed_queue_enabled = do_limit_simultaneous_torrents;
    }
}

bool tr_sessionGetQueueEnabled(tr_session const* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    return session->queueEnabled(dir);
}

void tr_sessionSetQueueStalledMinutes(tr_session* session, int minutes)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(minutes > 0);

    session->settings_.queue_stalled_minutes = minutes;
}

void tr_sessionSetQueueStalledEnabled(tr_session* session, bool is_enabled)
{
    TR_ASSERT(session != nullptr);

    session->settings_.queue_stalled_enabled = is_enabled;
}

bool tr_sessionGetQueueStalledEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->queueStalledEnabled();
}

size_t tr_sessionGetQueueStalledMinutes(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->queueStalledMinutes();
}

// ---

void tr_sessionSetAntiBruteForceThreshold(tr_session* session, int max_bad_requests)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(max_bad_requests > 0);

    session->rpc_server_->set_anti_brute_force_limit(max_bad_requests);
}

int tr_sessionGetAntiBruteForceThreshold(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->get_anti_brute_force_limit();
}

void tr_sessionSetAntiBruteForceEnabled(tr_session* session, bool is_enabled)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_anti_brute_force_enabled(is_enabled);
}

bool tr_sessionGetAntiBruteForceEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->is_anti_brute_force_enabled();
}

// ---

void tr_session::verify_remove(tr_torrent const* const tor)
{
    if (verifier_)
    {
        verifier_->remove(tor->info_hash());
    }
}

void tr_session::verify_add(tr_torrent* const tor)
{
    if (verifier_)
    {
        verifier_->add(std::make_unique<tr_torrent::VerifyMediator>(tor), tor->get_priority());
    }
}

// ---

void tr_session::close_torrent_files(tr_torrent_id_t const tor_id) noexcept
{
    this->cache->flush_torrent(tor_id);
    openFiles().close_torrent(tor_id);
}

void tr_session::close_torrent_file(tr_torrent const& tor, tr_file_index_t file_num) noexcept
{
    this->cache->flush_file(tor, file_num);
    openFiles().close_file(tor.id(), file_num);
}

// ---

void tr_sessionSetQueueStartCallback(tr_session* session, void (*callback)(tr_session*, tr_torrent*, void*), void* user_data)
{
    session->setQueueStartCallback(callback, user_data);
}

void tr_sessionSetRatioLimitHitCallback(tr_session* session, tr_session_ratio_limit_hit_func callback, void* user_data)
{
    session->setRatioLimitHitCallback(callback, user_data);
}

void tr_sessionSetIdleLimitHitCallback(tr_session* session, tr_session_idle_limit_hit_func callback, void* user_data)
{
    session->setIdleLimitHitCallback(callback, user_data);
}

void tr_sessionSetMetadataCallback(tr_session* session, tr_session_metadata_func callback, void* user_data)
{
    session->setMetadataCallback(callback, user_data);
}

void tr_sessionSetCompletenessCallback(tr_session* session, tr_torrent_completeness_func callback, void* user_data)
{
    session->setTorrentCompletenessCallback(callback, user_data);
}

tr_session_stats tr_sessionGetStats(tr_session const* session)
{
    return session->stats().current();
}

tr_session_stats tr_sessionGetCumulativeStats(tr_session const* session)
{
    return session->stats().cumulative();
}

void tr_sessionClearStats(tr_session* session)
{
    session->stats().clear();
}

// ---

namespace
{
auto constexpr QueueInterval = 1s;
auto constexpr SaveInterval = 360s;

auto makeResumeDir(std::string_view config_dir)
{
#if defined(__APPLE__) || defined(_WIN32)
    auto dir = fmt::format("{:s}/Resume"sv, config_dir);
#else
    auto dir = fmt::format("{:s}/resume"sv, config_dir);
#endif
    tr_sys_dir_create(dir.c_str(), TR_SYS_DIR_CREATE_PARENTS, 0777);
    return dir;
}

auto makeTorrentDir(std::string_view config_dir)
{
#if defined(__APPLE__) || defined(_WIN32)
    auto dir = fmt::format("{:s}/Torrents"sv, config_dir);
#else
    auto dir = fmt::format("{:s}/torrents"sv, config_dir);
#endif
    tr_sys_dir_create(dir.c_str(), TR_SYS_DIR_CREATE_PARENTS, 0777);
    return dir;
}

auto makeBlocklistDir(std::string_view config_dir)
{
    auto dir = fmt::format("{:s}/blocklists"sv, config_dir);
    tr_sys_dir_create(dir.c_str(), TR_SYS_DIR_CREATE_PARENTS, 0777);
    return dir;
}
} // namespace

tr_session::tr_session(std::string_view config_dir, tr_variant const& settings_dict)
    : config_dir_{ config_dir }
    , resume_dir_{ makeResumeDir(config_dir) }
    , torrent_dir_{ makeTorrentDir(config_dir) }
    , blocklist_dir_{ makeBlocklistDir(config_dir) }
    , session_thread_{ tr_session_thread::create() }
    , timer_maker_{ std::make_unique<libtransmission::EvTimerMaker>(event_base()) }
    , settings_{ settings_dict }
    , session_id_{ tr_time }
    , peer_mgr_{ tr_peerMgrNew(this), &tr_peerMgrFree }
    , rpc_server_{ std::make_unique<tr_rpc_server>(this, tr_rpc_server::Settings{ settings_dict }) }
    , now_timer_{ timer_maker_->create([this]() { on_now_timer(); }) }
    , queue_timer_{ timer_maker_->create([this]() { on_queue_timer(); }) }
    , save_timer_{ timer_maker_->create([this]() { on_save_timer(); }) }
{
    now_timer_->start_repeating(1s);
    queue_timer_->start_repeating(QueueInterval);
    save_timer_->start_repeating(SaveInterval);
}

void tr_session::addIncoming(tr_peer_socket&& socket)
{
    tr_peerMgrAddIncoming(peer_mgr_.get(), std::move(socket));
}

void tr_session::addTorrent(tr_torrent* tor)
{
    tor->init_id(torrents().add(tor));

    tr_peerMgrAddTorrent(peer_mgr_.get(), tor);
}
