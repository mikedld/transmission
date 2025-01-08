// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno> // EINVAL
#include <climits> /* INT_MAX */
#include <cstddef> // size_t
#include <ctime>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/core.h>

#include <small/map.hpp>

#include "libtransmission/transmission.h"
#include "libtransmission/tr-macros.h"

#include "libtransmission/announcer.h"
#include "libtransmission/bandwidth.h"
#include "libtransmission/completion.h"
#include "libtransmission/crypto-utils.h" // for tr_sha1()
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/inout.h" // tr_ioTestPiece()
#include "libtransmission/log.h"
#include "libtransmission/magnet-metainfo.h"
#include "libtransmission/peer-common.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/resume.h"
#include "libtransmission/session.h"
#include "libtransmission/subprocess.h"
#include "libtransmission/torrent-ctor.h"
#include "libtransmission/torrent-magnet.h"
#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"
#include "libtransmission/version.h"
#include "libtransmission/web-utils.h"

struct tr_ctor;

using namespace std::literals;
using namespace libtransmission::Values;

// ---

void tr_torrent::Error::set_tracker_warning(tr_interned_string announce_url, std::string_view errmsg)
{
    announce_url_ = announce_url;
    errmsg_.assign(errmsg);
    error_type_ = TR_STAT_TRACKER_WARNING;
}

void tr_torrent::Error::set_tracker_error(tr_interned_string announce_url, std::string_view errmsg)
{
    announce_url_ = announce_url;
    errmsg_.assign(errmsg);
    error_type_ = TR_STAT_TRACKER_ERROR;
}

void tr_torrent::Error::set_local_error(std::string_view errmsg)
{
    announce_url_.clear();
    errmsg_.assign(errmsg);
    error_type_ = TR_STAT_LOCAL_ERROR;
}

void tr_torrent::Error::clear() noexcept
{
    announce_url_.clear();
    errmsg_.clear();
    error_type_ = TR_STAT_OK;
}

void tr_torrent::Error::clear_if_tracker() noexcept
{
    if (error_type_ == TR_STAT_TRACKER_WARNING || error_type_ == TR_STAT_TRACKER_ERROR)
    {
        clear();
    }
}

// ---

char const* tr_torrentName(tr_torrent const* tor)
{
    return tor != nullptr ? tor->name().c_str() : "";
}

tr_torrent_id_t tr_torrentId(tr_torrent const* tor)
{
    return tor != nullptr ? tor->id() : -1;
}

tr_torrent* tr_torrentFindFromId(tr_session* session, tr_torrent_id_t id)
{
    return session->torrents().get(id);
}

tr_torrent* tr_torrentFindFromMetainfo(tr_session* session, tr_torrent_metainfo const* metainfo)
{
    if (session == nullptr || metainfo == nullptr)
    {
        return nullptr;
    }

    return session->torrents().get(metainfo->info_hash());
}

tr_torrent* tr_torrentFindFromMagnetLink(tr_session* session, char const* magnet_link)
{
    return magnet_link == nullptr ? nullptr : session->torrents().get(magnet_link);
}

bool tr_torrentSetMetainfoFromFile(tr_torrent* tor, tr_torrent_metainfo const* metainfo, char const* filename)
{
    if (tr_torrentHasMetadata(tor))
    {
        return false;
    }

    auto error = tr_error{};
    tor->use_metainfo_from_file(metainfo, filename, &error);
    if (error)
    {
        tor->error().set_local_error(fmt::format(
            fmt::runtime(_("Couldn't use metainfo from '{path}' for '{magnet}': {error} ({error_code})")),
            fmt::arg("path", filename),
            fmt::arg("magnet", tor->magnet()),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code())));
        return false;
    }

    return true;
}

// ---

namespace
{
bool did_files_disappear(tr_torrent* tor, std::optional<bool> has_any_local_data = {})
{
    auto const has = has_any_local_data ? *has_any_local_data : tor->has_any_local_data();
    return tor->has_total() > 0 && !has;
}

bool set_local_error_if_files_disappeared(tr_torrent* tor, std::optional<bool> has_any_local_data = {})
{
    auto const files_disappeared = did_files_disappear(tor, has_any_local_data);

    if (files_disappeared)
    {
        tr_logAddTraceTor(tor, "[LAZY] uh oh, the files disappeared");
        tor->error().set_local_error(
            _("No data found! Ensure your drives are connected or use \"Set Location\". "
              "To re-download, use \"Verify Local Data\" and start the torrent afterwards."));
    }

    return files_disappeared;
}

/* returns true if the seed ratio applies --
 * it applies if the torrent's a seed AND it has a seed ratio set */
bool tr_torrentGetSeedRatioBytes(tr_torrent const* tor, uint64_t* setme_left, uint64_t* setme_goal)
{
    bool seed_ratio_applies = false;

    TR_ASSERT(tr_isTorrent(tor));

    if (auto const seed_ratio = tor->effective_seed_ratio(); seed_ratio)
    {
        auto const uploaded = tor->bytes_uploaded_.ever();
        auto const baseline = tor->size_when_done();
        auto const goal = baseline * *seed_ratio;

        if (setme_left != nullptr)
        {
            *setme_left = goal > uploaded ? goal - uploaded : 0;
        }

        if (setme_goal != nullptr)
        {
            *setme_goal = goal;
        }

        seed_ratio_applies = tor->is_done();
    }

    return seed_ratio_applies;
}

bool tr_torrentIsSeedRatioDone(tr_torrent const* tor)
{
    auto bytes_left = uint64_t{};
    return tr_torrentGetSeedRatioBytes(tor, &bytes_left, nullptr) && bytes_left == 0;
}
} // namespace

// --- PER-TORRENT UL / DL SPEEDS

void tr_torrentSetSpeedLimit_KBps(tr_torrent* const tor, tr_direction const dir, size_t const limit_kbyps)
{
    tor->set_speed_limit(dir, Speed{ limit_kbyps, Speed::Units::KByps });
}

size_t tr_torrentGetSpeedLimit_KBps(tr_torrent const* const tor, tr_direction const dir)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tr_isDirection(dir));

    return tor->speed_limit(dir).count(Speed::Units::KByps);
}

void tr_torrentUseSpeedLimit(tr_torrent* const tor, tr_direction const dir, bool const enabled)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tr_isDirection(dir));

    tor->use_speed_limit(dir, enabled);
}

bool tr_torrentUsesSpeedLimit(tr_torrent const* const tor, tr_direction const dir)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->uses_speed_limit(dir);
}

void tr_torrentUseSessionLimits(tr_torrent* const tor, bool const enabled)
{
    TR_ASSERT(tr_isTorrent(tor));

    if (tor->bandwidth().honor_parent_limits(TR_UP, enabled) || tor->bandwidth().honor_parent_limits(TR_DOWN, enabled))
    {
        tor->set_dirty();
    }
}

bool tr_torrentUsesSessionLimits(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->uses_session_limits();
}

// --- Download Ratio

void tr_torrentSetRatioMode(tr_torrent* const tor, tr_ratiolimit mode)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->set_seed_ratio_mode(mode);
}

tr_ratiolimit tr_torrentGetRatioMode(tr_torrent const* const tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->seed_ratio_mode();
}

void tr_torrentSetRatioLimit(tr_torrent* const tor, double desired_ratio)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->set_seed_ratio(desired_ratio);
}

double tr_torrentGetRatioLimit(tr_torrent const* const tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->seed_ratio();
}

bool tr_torrentGetSeedRatio(tr_torrent const* const tor, double* ratio)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto const val = tor->effective_seed_ratio();

    if (ratio != nullptr && val)
    {
        *ratio = *val;
    }

    return val.has_value();
}

// ---

void tr_torrentSetIdleMode(tr_torrent* const tor, tr_idlelimit mode)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->set_idle_limit_mode(mode);
}

tr_idlelimit tr_torrentGetIdleMode(tr_torrent const* const tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->idle_limit_mode();
}

void tr_torrentSetIdleLimit(tr_torrent* const tor, uint16_t idle_minutes)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->set_idle_limit_minutes(idle_minutes);
}

uint16_t tr_torrentGetIdleLimit(tr_torrent const* const tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->idle_limit_minutes();
}

namespace
{
namespace script_helpers
{
[[nodiscard]] std::string build_labels_string(tr_torrent::labels_t const& labels)
{
    auto buf = std::stringstream{};

    for (auto it = std::begin(labels), end = std::end(labels); it != end;)
    {
        buf << it->sv();

        if (++it != end)
        {
            buf << ',';
        }
    }

    return buf.str();
}

[[nodiscard]] std::string buildTrackersString(tr_torrent const* tor)
{
    auto buf = std::stringstream{};

    for (size_t i = 0, n = tr_torrentTrackerCount(tor); i < n; ++i)
    {
        buf << tr_torrentTracker(tor, i).host_and_port;

        if (i < n)
        {
            buf << ',';
        }
    }

    return buf.str();
}

void torrentCallScript(tr_torrent const* tor, std::string const& script)
{
    if (std::empty(script))
    {
        return;
    }

    auto torrent_dir = tr_pathbuf{ tor->current_dir() };
    tr_sys_path_native_separators(std::data(torrent_dir));

    auto const cmd = std::array<char const*, 2>{ script.c_str(), nullptr };

    auto const id_str = std::to_string(tr_torrentId(tor));
    auto const labels_str = build_labels_string(tor->labels());
    auto const trackers_str = buildTrackersString(tor);
    auto const bytes_downloaded_str = std::to_string(tor->bytes_downloaded_.ever());
    auto const localtime_str = fmt::format("{:%a %b %d %T %Y%n}", fmt::localtime(tr_time()));
    auto const priority_str = std::to_string(tor->get_priority());

    auto const env = std::map<std::string_view, std::string_view>{
        { "TR_APP_VERSION"sv, SHORT_VERSION_STRING },
        { "TR_TIME_LOCALTIME"sv, localtime_str },
        { "TR_TORRENT_BYTES_DOWNLOADED"sv, bytes_downloaded_str },
        { "TR_TORRENT_DIR"sv, torrent_dir.c_str() },
        { "TR_TORRENT_HASH"sv, tor->info_hash_string() },
        { "TR_TORRENT_ID"sv, id_str },
        { "TR_TORRENT_LABELS"sv, labels_str },
        { "TR_TORRENT_NAME"sv, tor->name() },
        { "TR_TORRENT_PRIORITY"sv, priority_str },
        { "TR_TORRENT_TRACKERS"sv, trackers_str },
    };

    tr_logAddInfoTor(tor, fmt::format(fmt::runtime(_("Calling script '{path}'")), fmt::arg("path", script)));

    auto error = tr_error{};
    if (!tr_spawn_async(std::data(cmd), env, TR_IF_WIN32("\\", "/"), &error))
    {
        tr_logAddWarnTor(
            tor,
            fmt::format(
                fmt::runtime(_("Couldn't call script '{path}': {error} ({error_code})")),
                fmt::arg("path", script),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
    }
}
} // namespace script_helpers

void callScriptIfEnabled(tr_torrent const* tor, TrScript type)
{
    using namespace script_helpers;

    auto const* session = tor->session;

    if (tr_sessionIsScriptEnabled(session, type))
    {
        torrentCallScript(tor, session->script(type));
    }
}

} // namespace

// ---

void tr_torrent::stop_if_seed_limit_reached()
{
    if (!is_running() || is_stopping_ || !is_done())
    {
        return;
    }

    /* if we're seeding and reach our seed ratio limit, stop the torrent */
    if (tr_torrentIsSeedRatioDone(this))
    {
        tr_logAddInfoTor(this, _("Seed ratio reached; pausing torrent"));
        stop_soon();
        session->onRatioLimitHit(this);
    }
    /* if we're seeding and reach our inactivity limit, stop the torrent */
    else if (auto const secs_left = idle_seconds_left(tr_time()); secs_left && *secs_left <= 0U)
    {
        tr_logAddInfoTor(this, _("Seeding idle limit reached; pausing torrent"));

        stop_soon();
        finished_seeding_by_idle_ = true;
        session->onIdleLimitHit(this);
    }

    if (is_stopping_)
    {
        callScriptIfEnabled(this, TR_SCRIPT_ON_TORRENT_DONE_SEEDING);
    }
}

// --- Queue

namespace
{
namespace queue_helpers
{
constexpr auto MinQueuePosition = std::numeric_limits<size_t>::min();
constexpr auto MaxQueuePosition = std::numeric_limits<size_t>::max();

#ifdef TR_ENABLE_ASSERTS
[[nodiscard]] bool torrents_are_sorted_by_queue_position(std::vector<tr_torrent*> torrents)
{
    std::sort(std::begin(torrents), std::end(torrents), tr_torrent::CompareQueuePosition);

    for (size_t idx = 0, end_idx = std::size(torrents); idx < end_idx; ++idx)
    {
        if (torrents[idx]->queue_position() != idx)
        {
            return false;
        }
    }

    return true;
}
#endif
} // namespace queue_helpers
} // namespace

size_t tr_torrentGetQueuePosition(tr_torrent const* tor)
{
    return tor->queue_position();
}

void tr_torrent::set_unique_queue_position(size_t const new_pos)
{
    using namespace queue_helpers;

    auto max_pos = size_t{};
    auto const old_pos = queue_position_;

    auto& torrents = session->torrents();
    for (auto* const walk : torrents)
    {
        if (walk == this)
        {
            continue;
        }

        if ((old_pos < new_pos) && (old_pos < walk->queue_position_) && (walk->queue_position_ <= new_pos))
        {
            --walk->queue_position_;
            walk->mark_changed();
            walk->set_dirty();
        }

        if ((old_pos > new_pos) && (new_pos <= walk->queue_position_) && (walk->queue_position_ < old_pos))
        {
            ++walk->queue_position_;
            walk->mark_changed();
            walk->set_dirty();
        }

        max_pos = std::max(max_pos, walk->queue_position_);
    }

    queue_position_ = std::min(new_pos, max_pos + 1);
    mark_changed();
    set_dirty();

    TR_ASSERT(torrents_are_sorted_by_queue_position(torrents.get_all()));
}

void tr_torrentSetQueuePosition(tr_torrent* tor, size_t queue_position)
{
    tor->set_unique_queue_position(queue_position);
}

void tr_torrentsQueueMoveTop(tr_torrent* const* torrents_in, size_t torrent_count)
{
    using namespace queue_helpers;

    auto torrents = std::vector<tr_torrent*>(torrents_in, torrents_in + torrent_count);
    std::sort(std::rbegin(torrents), std::rend(torrents), tr_torrent::CompareQueuePosition);
    for (auto* const tor : torrents)
    {
        tor->set_unique_queue_position(MinQueuePosition);
    }
}

void tr_torrentsQueueMoveUp(tr_torrent* const* torrents_in, size_t torrent_count)
{
    using namespace queue_helpers;

    auto torrents = std::vector<tr_torrent*>(torrents_in, torrents_in + torrent_count);
    std::sort(std::begin(torrents), std::end(torrents), tr_torrent::CompareQueuePosition);
    for (auto* const tor : torrents)
    {
        if (auto const pos = tor->queue_position(); pos > MinQueuePosition)
        {
            tor->set_unique_queue_position(pos - 1U);
        }
    }
}

void tr_torrentsQueueMoveDown(tr_torrent* const* torrents_in, size_t torrent_count)
{
    using namespace queue_helpers;

    auto torrents = std::vector<tr_torrent*>(torrents_in, torrents_in + torrent_count);
    std::sort(std::rbegin(torrents), std::rend(torrents), tr_torrent::CompareQueuePosition);
    for (auto* const tor : torrents)
    {
        if (auto const pos = tor->queue_position(); pos < MaxQueuePosition)
        {
            tor->set_unique_queue_position(pos + 1U);
        }
    }
}

void tr_torrentsQueueMoveBottom(tr_torrent* const* torrents_in, size_t torrent_count)
{
    using namespace queue_helpers;

    auto torrents = std::vector<tr_torrent*>(torrents_in, torrents_in + torrent_count);
    std::sort(std::begin(torrents), std::end(torrents), tr_torrent::CompareQueuePosition);
    for (auto* const tor : torrents)
    {
        tor->set_unique_queue_position(MaxQueuePosition);
    }
}

// --- Start, Stop

namespace
{
namespace start_stop_helpers
{
bool torrentShouldQueue(tr_torrent const* const tor)
{
    tr_direction const dir = tor->queue_direction();

    return tor->session->count_queue_free_slots(dir) == 0;
}

bool removeTorrentFile(char const* filename, void* /*user_data*/, tr_error* error)
{
    return tr_sys_path_remove(filename, error);
}

void freeTorrent(tr_torrent* tor)
{
    using namespace queue_helpers;

    auto const lock = tor->unique_lock();

    TR_ASSERT(!tor->is_running());

    tr_session* session = tor->session;

    tor->doomed_.emit(tor);

    session->announcer_->removeTorrent(tor);

    session->torrents().remove(tor, tr_time());

    if (!session->isClosing())
    {
        // move the torrent being freed to the end of the queue so that
        // all the torrents queued after it will move up one position
        tor->set_unique_queue_position(queue_helpers::MaxQueuePosition);
    }

    delete tor;
}
} // namespace start_stop_helpers
} // namespace

// has_any_local_data is true or false if we know whether or not local data exists,
// or unset if we don't know and need to check for ourselves
void tr_torrent::start(bool bypass_queue, std::optional<bool> has_any_local_data)
{
    using namespace start_stop_helpers;

    auto const lock = unique_lock();

    switch (activity())
    {
    case TR_STATUS_SEED:
    case TR_STATUS_DOWNLOAD:
        return; /* already started */

    case TR_STATUS_SEED_WAIT:
    case TR_STATUS_DOWNLOAD_WAIT:
        if (!bypass_queue)
        {
            return; /* already queued */
        }

        break;

    case TR_STATUS_CHECK:
    case TR_STATUS_CHECK_WAIT:
        /* verifying right now... wait until that's done so
         * we'll know what completeness to use/announce */
        return;

    case TR_STATUS_STOPPED:
        if (!bypass_queue && torrentShouldQueue(this))
        {
            set_is_queued();
            return;
        }

        break;
    }

    /* don't allow the torrent to be started if the files disappeared */
    if (set_local_error_if_files_disappeared(this, has_any_local_data))
    {
        return;
    }

    /* allow finished torrents to be resumed */
    if (tr_torrentIsSeedRatioDone(this))
    {
        tr_logAddInfoTor(this, _("Restarted manually -- disabling its seed ratio"));
        set_seed_ratio_mode(TR_RATIOLIMIT_UNLIMITED);
    }

    is_running_ = true;
    set_dirty();
    session->run_in_session_thread([this]() { start_in_session_thread(); });
}

void tr_torrent::start_in_session_thread()
{
    using namespace start_stop_helpers;

    TR_ASSERT(session->am_in_session_thread());
    auto const lock = unique_lock();

    // We are after `torrentStart` and before announcing to trackers/peers,
    // so now is the best time to create wanted empty files.
    create_empty_files();

    recheck_completeness();
    set_is_queued(false);

    time_t const now = tr_time();

    is_running_ = true;
    date_started_ = now;
    mark_changed();
    error().clear();
    finished_seeding_by_idle_ = false;

    bytes_uploaded_.start_new_session();
    bytes_downloaded_.start_new_session();
    bytes_corrupt_.start_new_session();
    set_dirty();

    session->announcer_->startTorrent(this);
    lpdAnnounceAt = now;
    started_.emit(this);
}

void tr_torrent::stop_now()
{
    TR_ASSERT(session->am_in_session_thread());
    auto const lock = unique_lock();

    auto const now = tr_time();
    seconds_downloading_before_current_start_ = seconds_downloading(now);
    seconds_seeding_before_current_start_ = seconds_seeding(now);

    is_running_ = false;
    is_stopping_ = false;
    mark_changed();

    if (!session->isClosing())
    {
        tr_logAddInfoTor(this, _("Pausing torrent"));
    }

    session->verify_remove(this);

    stopped_.emit(this);
    session->announcer_->stopTorrent(this);

    session->close_torrent_files(id());

    if (!is_deleting_)
    {
        save_resume_file();
    }

    set_is_queued(false);
}

void tr_torrentRemoveInSessionThread(
    tr_torrent* tor,
    bool delete_flag,
    tr_fileFunc delete_func,
    void* delete_user_data,
    tr_torrent_remove_done_func callback,
    void* callback_user_data)
{
    auto const lock = tor->unique_lock();

    bool ok = true;
    if (delete_flag && tor->has_metainfo())
    {
        // ensure the files are all closed and idle before moving
        tor->session->close_torrent_files(tor->id());
        tor->session->verify_remove(tor);

        if (delete_func == nullptr)
        {
            delete_func = start_stop_helpers::removeTorrentFile;
        }

        auto const delete_func_wrapper = [&delete_func, delete_user_data](char const* filename)
        {
            delete_func(filename, delete_user_data, nullptr);
        };

        tr_error error;
        tor->files().remove(tor->current_dir(), tor->name(), delete_func_wrapper, &error);
        if (error)
        {
            ok = false;
            tor->is_deleting_ = false;

            tor->error().set_local_error(fmt::format(
                fmt::runtime(_("Couldn't remove all torrent files: {error} ({error_code})")),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
            tr_torrentStop(tor);
        }
    }

    if (callback != nullptr)
    {
        callback(tor->id(), ok, callback_user_data);
    }

    if (ok)
    {
        tr_torrentFreeInSessionThread(tor);
    }
}

void tr_torrentStop(tr_torrent* tor)
{
    if (!tr_isTorrent(tor))
    {
        return;
    }

    auto const lock = tor->unique_lock();

    tor->start_when_stable_ = false;
    tor->set_dirty();
    tor->session->run_in_session_thread([tor]() { tor->stop_now(); });
}

void tr_torrentRemove(
    tr_torrent* tor,
    bool delete_flag,
    tr_fileFunc delete_func,
    void* delete_user_data,
    tr_torrent_remove_done_func callback,
    void* callback_user_data)
{
    using namespace start_stop_helpers;

    TR_ASSERT(tr_isTorrent(tor));

    tor->is_deleting_ = true;

    tor->session->run_in_session_thread(
        tr_torrentRemoveInSessionThread,
        tor,
        delete_flag,
        delete_func,
        delete_user_data,
        callback,
        callback_user_data);
}

void tr_torrentFreeInSessionThread(tr_torrent* tor)
{
    using namespace start_stop_helpers;

    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->session != nullptr);
    TR_ASSERT(tor->session->am_in_session_thread());

    if (!tor->session->isClosing())
    {
        tr_logAddInfoTor(tor, _("Removing torrent"));
    }

    tor->set_dirty(!tor->is_deleting_);
    tor->stop_now();

    if (tor->is_deleting_)
    {
        tr_torrent_metainfo::remove_file(tor->session->torrentDir(), tor->name(), tor->info_hash_string(), ".torrent"sv);
        tr_torrent_metainfo::remove_file(tor->session->torrentDir(), tor->name(), tor->info_hash_string(), ".magnet"sv);
        tr_torrent_metainfo::remove_file(tor->session->resumeDir(), tor->name(), tor->info_hash_string(), ".resume"sv);
    }

    freeTorrent(tor);
}

// ---

// Sniff out newly-added seeds so that they can skip the verify step
bool tr_torrent::is_new_torrent_a_seed()
{
    if (!has_metainfo())
    {
        return false;
    }

    for (tr_file_index_t i = 0, n = file_count(); i < n; ++i)
    {
        // it's not a new seed if a file is missing
        auto const found = find_file(i);
        if (!found)
        {
            return false;
        }

        // it's not a new seed if a file is partial
        if (tr_strv_ends_with(found->filename(), tr_torrent_files::PartialFileSuffix))
        {
            return false;
        }

        // it's not a new seed if a file size is wrong
        if (found->size != file_size(i))
        {
            return false;
        }

        // it's not a new seed if it was modified after it was added
        if (found->last_modified_at >= date_added_)
        {
            return false;
        }
    }

    // check the first piece
    return ensure_piece_is_checked(0);
}

void tr_torrent::on_metainfo_updated()
{
    completion_ = tr_completion{ this, &block_info() };
    obfuscated_hash_ = tr_sha1::digest("req2"sv, info_hash());
    fpm_ = tr_file_piece_map{ metainfo_ };
    file_mtimes_.resize(file_count());
    file_priorities_ = tr_file_priorities{ &fpm_ };
    files_wanted_ = tr_files_wanted{ &fpm_ };
    checked_pieces_ = tr_bitfield{ size_t(piece_count()) };
}

void tr_torrent::on_metainfo_completed()
{
    // we can look for files now that we know what files are in the torrent
    refresh_current_dir();

    callScriptIfEnabled(this, TR_SCRIPT_ON_TORRENT_ADDED);

    if (session->shouldFullyVerifyAddedTorrents() || !is_new_torrent_a_seed())
    {
        // Potentially, we are in `tr_torrent::init`,
        // and we don't want any file created before `tr_torrent::start`
        // so we Verify but we don't Create files.
        tr_torrentVerify(this);
    }
    else
    {
        completion_.set_has_all();
        recheck_completeness();
        date_done_ = date_added_; // Must be after recheck_completeness()

        if (start_when_stable_)
        {
            start(false, {});
        }
        else if (is_running())
        {
            stop_soon();
        }
    }
}

void tr_torrent::init(tr_ctor const& ctor)
{
    session = ctor.session();
    TR_ASSERT(session != nullptr);
    auto const lock = unique_lock();

    auto const now_sec = tr_time();

    queue_position_ = std::size(session->torrents());

    on_metainfo_updated();

    if (auto dir = ctor.download_dir(TR_FORCE); !std::empty(dir))
    {
        download_dir_ = dir;
    }
    else if (dir = ctor.download_dir(TR_FALLBACK); !std::empty(dir))
    {
        download_dir_ = dir;
    }

    if (tr_sessionIsIncompleteDirEnabled(session))
    {
        auto const& dir = ctor.incomplete_dir();
        incomplete_dir_ = !std::empty(dir) ? dir : session->incompleteDir();
    }

    bandwidth().set_parent(&session->top_bandwidth_);
    bandwidth().set_priority(ctor.bandwidth_priority());
    error().clear();
    finished_seeding_by_idle_ = false;

    set_labels(ctor.labels());

    session->addTorrent(this);

    TR_ASSERT(bytes_downloaded_.during_this_session() == 0U);
    TR_ASSERT(bytes_uploaded_.during_this_session() == 0);

    mark_changed();

    date_added_ = now_sec; // this is a default that will be overwritten by the resume file

    tr_resume::fields_t loaded = {};

    {
        // tr_resume::load() calls a lot of tr_torrentSetFoo() methods
        // that set things as dirty, but... these settings being loaded are
        // the same ones that would be saved back again, so don't let them
        // affect the 'is dirty' flag.
        auto const was_dirty = is_dirty();
        auto resume_helper = ResumeHelper{ *this };
        loaded = tr_resume::load(this, resume_helper, tr_resume::All, ctor);
        set_dirty(was_dirty);
        tr_torrent_metainfo::migrate_file(session->torrentDir(), name(), info_hash_string(), ".torrent"sv);
    }

    completeness_ = completion_.status();

    ctor.init_torrent_priorities(*this);
    ctor.init_torrent_wanted(*this);

    refresh_current_dir();

    if ((loaded & tr_resume::Speedlimit) == 0)
    {
        use_speed_limit(TR_UP, false);
        set_speed_limit(TR_UP, session->speed_limit(TR_UP));
        use_speed_limit(TR_DOWN, false);
        set_speed_limit(TR_DOWN, session->speed_limit(TR_DOWN));
        tr_torrentUseSessionLimits(this, true);
    }

    if ((loaded & tr_resume::Ratiolimit) == 0)
    {
        set_seed_ratio_mode(TR_RATIOLIMIT_GLOBAL);
        set_seed_ratio(session->desiredRatio());
    }

    if ((loaded & tr_resume::Idlelimit) == 0)
    {
        set_idle_limit_mode(TR_IDLELIMIT_GLOBAL);
        set_idle_limit_minutes(session->idleLimitMinutes());
    }

    auto has_any_local_data = std::optional<bool>{};
    if ((loaded & tr_resume::Progress) != 0)
    {
        // if tr_resume::load() loaded progress info, then initCheckedPieces()
        // has already looked for local data on the filesystem
        has_any_local_data = std::any_of(
            std::begin(file_mtimes_),
            std::end(file_mtimes_),
            [](auto mtime) { return mtime > 0; });
    }

    auto const filename = has_metainfo() ? torrent_file() : magnet_file();

    // if we don't have a local .torrent or .magnet file already,
    // assume the torrent is new
    bool const is_new_torrent = !tr_sys_path_exists(filename);

    if (is_new_torrent)
    {
        auto error = tr_error{};

        if (has_metainfo()) // torrent file
        {
            ctor.save(filename, &error);
        }
        else // magnet link
        {
            auto const magnet_link = magnet();
            tr_file_save(filename, magnet_link, &error);
        }

        if (error)
        {
            this->error().set_local_error(fmt::format(
                fmt::runtime(_("Couldn't save '{path}': {error} ({error_code})")),
                fmt::arg("path", filename),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
        }
    }

    torrent_announcer = session->announcer_->addTorrent(this, &tr_torrent::on_tracker_response);

    if (auto const has_metainfo = this->has_metainfo(); is_new_torrent && has_metainfo)
    {
        on_metainfo_completed();
    }
    else if (start_when_stable_)
    {
        auto const bypass_queue = !has_metainfo; // to fetch metainfo from peers
        start(bypass_queue, has_any_local_data);
    }
    else
    {
        set_local_error_if_files_disappeared(this, has_any_local_data);
    }

    // Recover from the bug reported at https://github.com/transmission/transmission/issues/6899
    if (is_done() && date_done_ == time_t{})
    {
        date_done_ = now_sec;
    }
}

void tr_torrent::set_metainfo(tr_torrent_metainfo tm)
{
    TR_ASSERT(!has_metainfo());
    metainfo_ = std::move(tm);
    on_metainfo_updated();

    got_metainfo_.emit(this);
    session->onMetadataCompleted(this);
    set_dirty();
    mark_edited();

    on_metainfo_completed();
    this->on_announce_list_changed();
}

tr_torrent* tr_torrentNew(tr_ctor* ctor, tr_torrent** setme_duplicate_of)
{
    TR_ASSERT(ctor != nullptr);
    auto* const session = ctor->session();
    TR_ASSERT(session != nullptr);

    // is the metainfo valid?
    auto metainfo = ctor->steal_metainfo();
    if (std::empty(metainfo.info_hash_string()))
    {
        return nullptr;
    }

    // is it a duplicate?
    if (auto* const duplicate_of = session->torrents().get(metainfo.info_hash()); duplicate_of != nullptr)
    {
        if (setme_duplicate_of != nullptr)
        {
            *setme_duplicate_of = duplicate_of;
        }

        return nullptr;
    }

    auto* const tor = new tr_torrent{ std::move(metainfo) };
    tor->verify_done_callback_ = ctor->steal_verify_done_callback();
    tor->init(*ctor);
    return tor;
}

// --- Location

void tr_torrent::set_location_in_session_thread(std::string_view const path, bool move_from_old_path, int volatile* setme_state)
{
    TR_ASSERT(session->am_in_session_thread());

    auto ok = true;
    if (move_from_old_path)
    {
        if (setme_state != nullptr)
        {
            *setme_state = TR_LOC_MOVING;
        }

        // ensure the files are all closed and idle before moving
        session->close_torrent_files(id());
        session->verify_remove(this);

        auto error = tr_error{};
        ok = files().move(current_dir(), path, name(), &error);
        if (error)
        {
            this->error().set_local_error(fmt::format(
                fmt::runtime(_("Couldn't move '{old_path}' to '{path}': {error} ({error_code})")),
                fmt::arg("old_path", current_dir()),
                fmt::arg("path", path),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
            tr_torrentStop(this);
        }
    }

    // tell the torrent where the files are
    if (ok)
    {
        set_download_dir(path);

        if (move_from_old_path)
        {
            incomplete_dir_.clear();
            current_dir_ = download_dir();
        }
    }

    if (setme_state != nullptr)
    {
        *setme_state = ok ? TR_LOC_DONE : TR_LOC_ERROR;
    }
}

namespace
{
namespace location_helpers
{
size_t buildSearchPathArray(tr_torrent const* tor, std::string_view* paths)
{
    auto* walk = paths;

    if (auto const& path = tor->download_dir(); !std::empty(path))
    {
        *walk++ = path.sv();
    }

    if (auto const& path = tor->incomplete_dir(); !std::empty(path))
    {
        *walk++ = path.sv();
    }

    return walk - paths;
}
} // namespace location_helpers
} // namespace

void tr_torrent::set_location(std::string_view location, bool move_from_old_path, int volatile* setme_state)
{
    if (setme_state != nullptr)
    {
        *setme_state = TR_LOC_MOVING;
    }

    session->run_in_session_thread([this, loc = std::string(location), move_from_old_path, setme_state]()
                                   { set_location_in_session_thread(loc, move_from_old_path, setme_state); });
}

void tr_torrentSetLocation(tr_torrent* tor, char const* location, bool move_from_old_path, int volatile* setme_state)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(location != nullptr);
    TR_ASSERT(*location != '\0');

    tor->set_location(location, move_from_old_path, setme_state);
}

std::optional<tr_torrent_files::FoundFile> tr_torrent::find_file(tr_file_index_t file_index) const
{
    using namespace location_helpers;

    auto paths = std::array<std::string_view, 4>{};
    auto const n_paths = buildSearchPathArray(this, std::data(paths));
    return files().find(file_index, std::data(paths), n_paths);
}

bool tr_torrent::has_any_local_data() const
{
    using namespace location_helpers;

    auto paths = std::array<std::string_view, 4>{};
    auto const n_paths = buildSearchPathArray(this, std::data(paths));
    return files().has_any_local_data(std::data(paths), n_paths);
}

void tr_torrentSetDownloadDir(tr_torrent* tor, char const* path)
{
    TR_ASSERT(tr_isTorrent(tor));

    if (tor->download_dir_ != path)
    {
        tor->set_download_dir(path, true);
    }
}

char const* tr_torrentGetDownloadDir(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->download_dir().c_str();
}

char const* tr_torrentGetCurrentDir(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->current_dir().c_str();
}

void tr_torrentChangeMyPort(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    if (tor->is_running())
    {
        tr_announcerChangeMyPort(tor);
    }
}

// ---

namespace
{
namespace manual_update_helpers
{
void torrentManualUpdateImpl(tr_torrent* const tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    if (tor->is_running())
    {
        tr_announcerManualAnnounce(tor);
    }
}
} // namespace manual_update_helpers
} // namespace

void tr_torrentManualUpdate(tr_torrent* tor)
{
    using namespace manual_update_helpers;

    TR_ASSERT(tr_isTorrent(tor));

    tor->session->run_in_session_thread(torrentManualUpdateImpl, tor);
}

bool tr_torrentCanManualUpdate(tr_torrent const* tor)
{
    return tr_isTorrent(tor) && tor->is_running() && tr_announcerCanManualAnnounce(tor);
}

// ---

tr_stat tr_torrent::stats() const
{
    static auto constexpr IsStalled = [](tr_torrent const* const tor, std::optional<size_t> idle_secs)
    {
        return tor->session->queueStalledEnabled() && idle_secs > tor->session->queueStalledMinutes() * 60U;
    };

    auto const now_msec = tr_time_msec();
    auto const now_sec = tr_time();

    auto const swarm_stats = this->swarm != nullptr ? tr_swarmGetStats(this->swarm) : tr_swarm_stats{};
    auto const activity = this->activity();
    auto const idle_seconds = this->idle_seconds(now_sec);

    auto stats = tr_stat{};

    stats.id = this->id();
    stats.activity = activity;
    stats.error = this->error().error_type();
    stats.queuePosition = queue_position();
    stats.idleSecs = idle_seconds ? static_cast<time_t>(*idle_seconds) : -1;
    stats.isStalled = IsStalled(this, idle_seconds);
    stats.errorString = this->error().errmsg().c_str();

    stats.peersConnected = swarm_stats.peer_count;
    stats.peersSendingToUs = swarm_stats.active_peer_count[TR_DOWN];
    stats.peersGettingFromUs = swarm_stats.active_peer_count[TR_UP];
    stats.webseedsSendingToUs = swarm_stats.active_webseed_count;

    for (int i = 0; i < TR_PEER_FROM__MAX; i++)
    {
        stats.peersFrom[i] = swarm_stats.peer_from_count[i];
        stats.knownPeersFrom[i] = swarm_stats.known_peer_from_count[i];
    }

    auto const piece_upload_speed = bandwidth().get_piece_speed(now_msec, TR_UP);
    stats.pieceUploadSpeed_KBps = piece_upload_speed.count(Speed::Units::KByps);
    auto const piece_download_speed = bandwidth().get_piece_speed(now_msec, TR_DOWN);
    stats.pieceDownloadSpeed_KBps = piece_download_speed.count(Speed::Units::KByps);

    stats.percentComplete = this->completion_.percent_complete();
    stats.metadataPercentComplete = get_metadata_percent();

    stats.percentDone = this->completion_.percent_done();
    stats.leftUntilDone = this->completion_.left_until_done();
    stats.sizeWhenDone = this->completion_.size_when_done();

    auto const verify_progress = this->verify_progress();
    stats.recheckProgress = verify_progress.value_or(0.0);
    stats.activityDate = this->date_active_;
    stats.addedDate = this->date_added_;
    stats.doneDate = this->date_done_;
    stats.editDate = this->date_edited_;
    stats.startDate = this->date_started_;
    stats.secondsSeeding = this->seconds_seeding(now_sec);
    stats.secondsDownloading = this->seconds_downloading(now_sec);

    stats.corruptEver = this->bytes_corrupt_.ever();
    stats.downloadedEver = this->bytes_downloaded_.ever();
    stats.uploadedEver = this->bytes_uploaded_.ever();
    stats.haveValid = this->completion_.has_valid();
    stats.haveUnchecked = this->has_total() - stats.haveValid;
    stats.desiredAvailable = tr_peerMgrGetDesiredAvailable(this);

    stats.ratio = tr_getRatio(stats.uploadedEver, this->size_when_done());

    auto seed_ratio_bytes_left = uint64_t{};
    auto seed_ratio_bytes_goal = uint64_t{};
    bool const seed_ratio_applies = tr_torrentGetSeedRatioBytes(this, &seed_ratio_bytes_left, &seed_ratio_bytes_goal);

    // eta, etaIdle
    stats.eta = TR_ETA_NOT_AVAIL;
    stats.etaIdle = TR_ETA_NOT_AVAIL;
    if (activity == TR_STATUS_DOWNLOAD)
    {
        if (auto const eta_speed_byps = eta_speed_.update(now_msec, piece_download_speed).base_quantity(); eta_speed_byps == 0U)
        {
            stats.eta = TR_ETA_UNKNOWN;
        }
        else if (stats.leftUntilDone <= stats.desiredAvailable || webseed_count() >= 1U)
        {
            stats.eta = stats.leftUntilDone / eta_speed_byps;
        }
    }
    else if (activity == TR_STATUS_SEED)
    {
        auto const eta_speed_byps = eta_speed_.update(now_msec, piece_upload_speed).base_quantity();

        if (seed_ratio_applies)
        {
            stats.eta = eta_speed_byps == 0U ? static_cast<time_t>(TR_ETA_UNKNOWN) : seed_ratio_bytes_left / eta_speed_byps;
        }

        if (eta_speed_byps < 1U)
        {
            if (auto const secs_left = idle_seconds_left(now_sec); secs_left)
            {
                stats.etaIdle = *secs_left;
            }
        }
    }

    /* stats.haveValid is here to make sure a torrent isn't marked 'finished'
     * when the user hits "uncheck all" prior to starting the torrent... */
    stats.finished = this->finished_seeding_by_idle_ ||
        (seed_ratio_applies && seed_ratio_bytes_left == 0 && stats.haveValid != 0);

    if (!seed_ratio_applies || stats.finished)
    {
        stats.seedRatioPercentDone = 1.0F;
    }
    else if (seed_ratio_bytes_goal == 0) /* impossible? safeguard for div by zero */
    {
        stats.seedRatioPercentDone = 0.0F;
    }
    else
    {
        stats.seedRatioPercentDone = float(seed_ratio_bytes_goal - seed_ratio_bytes_left) / seed_ratio_bytes_goal;
    }

    /* test some of the constraints */
    TR_ASSERT(stats.sizeWhenDone <= this->total_size());
    TR_ASSERT(stats.leftUntilDone <= stats.sizeWhenDone);
    TR_ASSERT(stats.desiredAvailable <= stats.leftUntilDone);
    return stats;
}

tr_stat const* tr_torrentStat(tr_torrent* const tor)
{
    tor->stats_ = tor->stats();
    return &tor->stats_;
}

// ---

tr_file_view tr_torrentFile(tr_torrent const* tor, tr_file_index_t file)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto const& subpath = tor->file_subpath(file);
    auto const priority = tor->file_priorities_.file_priority(file);
    auto const wanted = tor->files_wanted_.file_wanted(file);
    auto const length = tor->file_size(file);
    auto const [begin, end] = tor->piece_span_for_file(file);

    if (tor->is_seed() || length == 0)
    {
        return { subpath.c_str(), length, length, 1.0, begin, end, priority, wanted };
    }

    auto const have = tor->completion_.count_has_bytes_in_span(tor->byte_span_for_file(file));
    return { subpath.c_str(), have, length, have >= length ? 1.0 : have / double(length), begin, end, priority, wanted };
}

size_t tr_torrentFileCount(tr_torrent const* torrent)
{
    TR_ASSERT(tr_isTorrent(torrent));

    return torrent->file_count();
}

tr_webseed_view tr_torrentWebseed(tr_torrent const* tor, size_t nth)
{
    return tr_peerMgrWebseed(tor, nth);
}

size_t tr_torrentWebseedCount(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->webseed_count();
}

tr_tracker_view tr_torrentTracker(tr_torrent const* tor, size_t i)
{
    return tr_announcerTracker(tor, i);
}

size_t tr_torrentTrackerCount(tr_torrent const* tor)
{
    return tr_announcerTrackerCount(tor);
}

tr_torrent_view tr_torrentView(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto ret = tr_torrent_view{};
    ret.name = tor->name().c_str();
    ret.hash_string = tor->info_hash_string().c_str();
    ret.comment = tor->comment().c_str();
    ret.creator = tor->creator().c_str();
    ret.source = tor->source().c_str();
    ret.total_size = tor->total_size();
    ret.date_created = tor->date_created();
    ret.piece_size = tor->piece_size();
    ret.n_pieces = tor->piece_count();
    ret.is_private = tor->is_private();
    ret.is_folder = tor->file_count() > 1 || (tor->file_count() == 1 && tr_strv_contains(tor->file_subpath(0), '/'));

    return ret;
}

std::string tr_torrentFilename(tr_torrent const* tor)
{
    return std::string{ tor->torrent_file() };
}

size_t tr_torrentFilenameToBuf(tr_torrent const* tor, char* buf, size_t buflen)
{
    return tr_strv_to_buf(tr_torrentFilename(tor), buf, buflen);
}

// ---

tr_peer_stat* tr_torrentPeers(tr_torrent const* tor, size_t* peer_count)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tr_peerMgrPeerStats(tor, peer_count);
}

void tr_torrentPeersFree(tr_peer_stat* peer_stats, size_t /*peer_count*/)
{
    delete[] peer_stats;
}

void tr_torrentAvailability(tr_torrent const* tor, int8_t* tab, int size)
{
    TR_ASSERT(tr_isTorrent(tor));

    if (tab != nullptr && size > 0)
    {
        tr_peerMgrTorrentAvailability(tor, tab, size);
    }
}

void tr_torrentAmountFinished(tr_torrent const* tor, float* tabs, int n_tabs)
{
    tor->amount_done_bins(tabs, n_tabs);
}

// --- Start/Stop Callback

void tr_torrentStart(tr_torrent* tor)
{
    if (tr_isTorrent(tor))
    {
        tor->start_when_stable_ = true;
        tor->start(false /*bypass_queue*/, {});
    }
}

void tr_torrentStartNow(tr_torrent* tor)
{
    if (tr_isTorrent(tor))
    {
        tor->start_when_stable_ = true;
        tor->start(true /*bypass_queue*/, {});
    }
}

// ---

void tr_torrentVerify(tr_torrent* tor)
{
    tor->session->run_in_session_thread(
        [tor, session = tor->session, tor_id = tor->id()]()
        {
            TR_ASSERT(session->am_in_session_thread());
            auto const lock = session->unique_lock();

            if (tor != session->torrents().get(tor_id) || tor->is_deleting_)
            {
                return;
            }

            session->verify_remove(tor);

            if (!tor->has_metainfo())
            {
                return;
            }

            if (tor->is_running())
            {
                tor->stop_now();
            }

            if (did_files_disappear(tor))
            {
                tor->error().set_local_error(
                    _("Paused torrent as no data was found! Ensure your drives are connected or use \"Set Location\", "
                      "then use \"Verify Local Data\" again. To re-download, start the torrent."));
                tor->start_when_stable_ = false;
            }

            session->verify_add(tor);
        });
}

void tr_torrent::set_verify_state(VerifyState const state)
{
    TR_ASSERT(state == VerifyState::None || state == VerifyState::Queued || state == VerifyState::Active);

    verify_state_ = state;
    verify_progress_ = {};
    mark_changed();
}

tr_torrent_metainfo const& tr_torrent::VerifyMediator::metainfo() const
{
    return tor_->metainfo_;
}

std::optional<std::string> tr_torrent::VerifyMediator::find_file(tr_file_index_t const file_index) const
{
    if (auto const found = tor_->find_file(file_index); found)
    {
        return std::string{ found->filename().sv() };
    }

    return {};
}

void tr_torrent::update_file_path(tr_file_index_t file, std::optional<bool> has_file) const
{
    auto const found = find_file(file);
    if (!found)
    {
        return;
    }

    auto const has = has_file ? *has_file : this->has_file(file);
    auto const needs_suffix = session->isIncompleteFileNamingEnabled() && !has;
    auto const oldpath = found->filename();
    auto const newpath = needs_suffix ?
        tr_pathbuf{ found->base(), '/', file_subpath(file), tr_torrent_files::PartialFileSuffix } :
        tr_pathbuf{ found->base(), '/', file_subpath(file) };

    if (tr_sys_path_is_same(oldpath, newpath))
    {
        return;
    }

    if (auto error = tr_error{}; !tr_sys_path_rename(oldpath, newpath, &error))
    {
        tr_logAddErrorTor(
            this,
            fmt::format(
                fmt::runtime(_("Couldn't move '{old_path}' to '{path}': {error} ({error_code})")),
                fmt::arg("old_path", oldpath),
                fmt::arg("path", newpath),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
    }
}

void tr_torrent::VerifyMediator::on_verify_queued()
{
    tr_logAddTraceTor(tor_, "Queued for verification");
    tor_->set_verify_state(VerifyState::Queued);
}

void tr_torrent::VerifyMediator::on_verify_started()
{
    tr_logAddDebugTor(tor_, "Verifying torrent");
    time_started_ = tr_time();
    tor_->set_verify_state(VerifyState::Active);
}

void tr_torrent::VerifyMediator::on_piece_checked(tr_piece_index_t const piece, bool const has_piece)
{
    if (auto const had_piece = tor_->has_piece(piece); !has_piece || !had_piece)
    {
        tor_->set_has_piece(piece, has_piece);
        tor_->set_dirty();
    }

    tor_->checked_pieces_.set(piece, true);
    tor_->mark_changed();
    tor_->verify_progress_ = std::clamp(static_cast<float>(piece + 1U) / tor_->metainfo_.piece_count(), 0.0F, 1.0F);
}

// (usually called from tr_verify_worker's thread)
void tr_torrent::VerifyMediator::on_verify_done(bool const aborted)
{
    if (time_started_.has_value())
    {
        auto const total_size = tor_->total_size();
        auto const duration_secs = tr_time() - *time_started_;
        tr_logAddDebugTor(
            tor_,
            fmt::format(
                "Verification is done. It took {} seconds to verify {} bytes ({} bytes per second)",
                duration_secs,
                total_size,
                total_size / (1 + duration_secs)));
    }

    tor_->set_verify_state(VerifyState::None);

    if (!aborted && !tor_->is_deleting_)
    {
        tor_->session->run_in_session_thread(
            // Do not capture the torrent pointer directly, or else we will crash if program
            // execution reaches this point while the session thread is about to free this torrent.
            [tor_id = tor_->id(), session = tor_->session]()
            {
                auto* const tor = session->torrents().get(tor_id);
                if (tor == nullptr || tor->is_deleting_)
                {
                    return;
                }

                for (tr_file_index_t file = 0, n_files = tor->file_count(); file < n_files; ++file)
                {
                    tor->update_file_path(file, {});
                }

                tor->recheck_completeness();

                if (tor->verify_done_callback_)
                {
                    tor->verify_done_callback_(tor);
                }

                if (tor->start_when_stable_)
                {
                    tor->start(false, !tor->checked_pieces_.has_none());
                }
            });
    }
}

// ---

void tr_torrent::save_resume_file()
{
    if (!is_dirty())
    {
        return;
    }

    set_dirty(false);
    auto helper = ResumeHelper{ *this };
    tr_resume::save(this, helper);
}

// --- Completeness

namespace
{
namespace completeness_helpers
{
[[nodiscard]] constexpr char const* get_completion_string(int type)
{
    switch (type)
    {
    case TR_PARTIAL_SEED:
        /* Translators: this is a minor point that's safe to skip over, but FYI:
           "Complete" and "Done" are specific, different terms in Transmission:
           "Complete" means we've downloaded every file in the torrent.
           "Done" means we're done downloading the files we wanted, but NOT all
           that exist */
        return "Done";

    case TR_SEED:
        return "Complete";

    default:
        return "Incomplete";
    }
}
} // namespace completeness_helpers
} // namespace

void tr_torrent::create_empty_files() const
{
    auto const base = current_dir();
    TR_ASSERT(!std::empty(base));
    if (!has_metainfo() || std::empty(base))
    {
        return;
    }

    auto const file_count = this->file_count();
    for (tr_file_index_t file_index = 0U; file_index < file_count; ++file_index)
    {
        if (file_size(file_index) != 0U || !file_is_wanted(file_index) || find_file(file_index))
        {
            continue;
        }

        // torrent contains a wanted zero-bytes file and that file isn't on disk yet.
        // We attempt to create that file.
        auto filename = tr_pathbuf{};
        auto const& subpath = file_subpath(file_index);
        filename.assign(base, '/', subpath);

        // create subfolders, if any
        auto dir = tr_pathbuf{ filename.sv() };
        dir.popdir();
        tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0777);

        // create the file
        if (auto const fd = tr_sys_file_open(filename, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_SEQUENTIAL, 0666);
            fd != TR_BAD_SYS_FILE)
        {
            tr_sys_file_close(fd);
        }
    }
}

void tr_torrent::recheck_completeness()
{
    using namespace completeness_helpers;

    auto const lock = unique_lock();

    needs_completeness_check_ = false;

    if (auto const new_completeness = completion_.status(); completeness_ != new_completeness)
    {
        bool const recent_change = bytes_downloaded_.during_this_session() != 0U;
        bool const was_running = is_running();

        tr_logAddTraceTor(
            this,
            fmt::format(
                "State changed from {} to {}",
                get_completion_string(completeness_),
                get_completion_string(new_completeness)));

        completeness_ = new_completeness;
        session->close_torrent_files(id());

        if (is_done())
        {
            if (recent_change)
            {
                // https://www.bittorrent.org/beps/bep_0003.html
                // ...and one using completed is sent when the download is complete.
                // No completed is sent if the file was complete when started.
                tr_announcerTorrentCompleted(this);
            }
            date_done_ = tr_time();

            if (current_dir() == incomplete_dir())
            {
                set_location(download_dir(), true, nullptr);
            }

            done_.emit(this, recent_change);
        }

        session->onTorrentCompletenessChanged(this, completeness_, was_running);

        set_dirty();
        mark_changed();

        if (is_done())
        {
            save_resume_file();
            callScriptIfEnabled(this, TR_SCRIPT_ON_TORRENT_DONE);
        }
    }
}

// --- File DND

void tr_torrentSetFileDLs(tr_torrent* tor, tr_file_index_t const* files, tr_file_index_t n_files, bool wanted)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->set_files_wanted(files, n_files, wanted);
}

// ---

void tr_torrent::set_labels(labels_t const& new_labels)
{
    auto const lock = unique_lock();
    labels_.clear();

    for (auto label : new_labels)
    {
        if (std::find(std::begin(labels_), std::end(labels_), label) == std::end(labels_))
        {
            labels_.push_back(label);
        }
    }
    labels_.shrink_to_fit();
    this->set_dirty();
}

// ---

void tr_torrent::set_bandwidth_group(std::string_view group_name) noexcept
{
    group_name = tr_strv_strip(group_name);

    auto const lock = this->unique_lock();

    if (std::empty(group_name))
    {
        this->bandwidth_group_ = tr_interned_string{};
        this->bandwidth().set_parent(&this->session->top_bandwidth_);
    }
    else
    {
        this->bandwidth_group_ = group_name;
        this->bandwidth().set_parent(&this->session->getBandwidthGroup(group_name));
    }

    this->set_dirty();
}

// ---

tr_priority_t tr_torrentGetPriority(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->get_priority();
}

void tr_torrentSetPriority(tr_torrent* const tor, tr_priority_t const priority)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tr_isPriority(priority));

    if (tor->bandwidth().get_priority() != priority)
    {
        tor->bandwidth().set_priority(priority);

        tor->set_dirty();
    }
}

// ---

void tr_torrentSetPeerLimit(tr_torrent* tor, uint16_t max_connected_peers)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->set_peer_limit(max_connected_peers);
}

uint16_t tr_torrentGetPeerLimit(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    return tor->peer_limit();
}

// ---

tr_block_span_t tr_torrent::block_span_for_file(tr_file_index_t const file) const noexcept
{
    auto const [begin_byte, end_byte] = byte_span_for_file(file);

    // N.B. If the last file in the torrent is 0 bytes, and the torrent size is a multiple of block size,
    // then the computed block index will be past-the-end. We handle this with std::min.
    auto const begin_block = std::min(byte_loc(begin_byte).block, block_count() - 1U);

    if (begin_byte >= end_byte) // 0-byte file
    {
        return { begin_block, begin_block + 1 };
    }

    auto const final_block = byte_loc(end_byte - 1).block;
    auto const end_block = final_block + 1;
    return { begin_block, end_block };
}

// ---

void tr_torrent::set_file_priorities(tr_file_index_t const* files, tr_file_index_t file_count, tr_priority_t priority)
{
    if (std::any_of(
            files,
            files + file_count,
            [this, priority](tr_file_index_t file) { return priority != file_priorities_.file_priority(file); }))
    {
        file_priorities_.set(files, file_count, priority);
        priority_changed_.emit(this, files, file_count, priority);
        set_dirty();
    }
}

// ---

bool tr_torrent::check_piece(tr_piece_index_t const piece) const
{
    auto const pass = tr_ioTestPiece(*this, piece);
    tr_logAddTraceTor(this, fmt::format("[LAZY] tr_torrent.checkPiece tested piece {}, pass=={}", piece, pass));
    return pass;
}

// ---

bool tr_torrent::set_announce_list(std::string_view announce_list_str)
{
    auto ann = tr_announce_list{};
    return ann.parse(announce_list_str) && set_announce_list(std::move(ann));
}

bool tr_torrent::set_announce_list(tr_announce_list announce_list)
{
    auto const lock = unique_lock();

    auto& tgt = metainfo_.announce_list();

    tgt = std::move(announce_list);

    // save the changes
    auto save_error = tr_error{};
    auto filename = std::string{};
    if (has_metainfo())
    {
        filename = torrent_file();
        tgt.save(filename, &save_error);
    }
    else
    {
        filename = magnet_file();
        tr_file_save(filename, magnet(), &save_error);
    }

    on_announce_list_changed();

    if (save_error.has_value())
    {
        error().set_local_error(fmt::format(
            fmt::runtime(_("Couldn't save '{path}': {error} ({error_code})")),
            fmt::arg("path", filename),
            fmt::arg("error", save_error.message()),
            fmt::arg("error_code", save_error.code())));
        return false;
    }

    return true;
}

void tr_torrent::on_announce_list_changed()
{
    // if we had a tracker-related error on this torrent,
    // and that tracker's been removed,
    // then clear the error
    if (auto const& error_url = error_.announce_url(); !std::empty(error_url))
    {
        auto const& ann = metainfo().announce_list();
        if (std::none_of(
                std::begin(ann),
                std::end(ann),
                [error_url](auto const& tracker) { return tracker.announce == error_url; }))
        {
            error_.clear();
        }
    }

    mark_edited();

    session->announcer_->resetTorrent(this);
}

void tr_torrent::on_tracker_response(tr_tracker_event const* event)
{
    switch (event->type)
    {
    case tr_tracker_event::Type::Peers:
        tr_logAddTraceTor(this, fmt::format("Got {} peers from tracker", std::size(event->pex)));
        tr_peerMgrAddPex(this, TR_PEER_FROM_TRACKER, std::data(event->pex), std::size(event->pex));
        break;

    case tr_tracker_event::Type::Counts:
        if (is_private() && (event->leechers == 0 || event->downloaders == 0))
        {
            swarm_is_all_upload_only_.emit(this);
        }

        break;

    case tr_tracker_event::Type::Warning:
        tr_logAddWarnTor(
            this,
            fmt::format(
                fmt::runtime(_("Tracker warning: '{warning}' ({url})")),
                fmt::arg("warning", event->text),
                fmt::arg("url", tr_urlTrackerLogName(event->announce_url))));
        error_.set_tracker_warning(event->announce_url, event->text);
        break;

    case tr_tracker_event::Type::Error:
        error_.set_tracker_error(event->announce_url, event->text);
        break;

    case tr_tracker_event::Type::ErrorClear:
        error_.clear_if_tracker();
        break;
    }
}

bool tr_torrentSetTrackerList(tr_torrent* tor, char const* text)
{
    return text != nullptr && tor->set_announce_list(text);
}

std::string tr_torrentGetTrackerList(tr_torrent const* tor)
{
    return tor->announce_list().to_string();
}

size_t tr_torrentGetTrackerListToBuf(tr_torrent const* tor, char* buf, size_t buflen)
{
    return tr_strv_to_buf(tr_torrentGetTrackerList(tor), buf, buflen);
}

// ---

uint64_t tr_torrentGetBytesLeftToAllocate(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    uint64_t bytes_left = 0;

    for (tr_file_index_t i = 0, n = tor->file_count(); i < n; ++i)
    {
        if (auto const wanted = tor->files_wanted_.file_wanted(i); !wanted)
        {
            continue;
        }

        auto const length = tor->file_size(i);
        bytes_left += length;

        auto const found = tor->find_file(i);
        if (found)
        {
            bytes_left -= found->size;
        }
    }

    return bytes_left;
}

// ---

std::string_view tr_torrent::primary_mime_type() const
{
    // count up how many bytes there are for each mime-type in the torrent
    // NB: get_mime_type_for_filename() always returns the same ptr for a
    // mime_type, so its raw pointer can be used as a key.
    auto size_per_mime_type = small::unordered_map<std::string_view, size_t, 256U>{};
    for (tr_file_index_t i = 0, n = this->file_count(); i < n; ++i)
    {
        auto const mime_type = tr_get_mime_type_for_filename(this->file_subpath(i));
        size_per_mime_type[mime_type] += this->file_size(i);
    }

    if (std::empty(size_per_mime_type))
    {
        // https://developer.mozilla.org/en-US/docs/Web/HTTP/Basics_of_HTTP/MIME_types/Common_types
        // application/octet-stream is the default value for all other cases.
        // An unknown file type should use this type.
        auto constexpr Fallback = "application/octet-stream"sv;
        return Fallback;
    }

    auto const it = std::max_element(
        std::begin(size_per_mime_type),
        std::end(size_per_mime_type),
        [](auto const& a, auto const& b) { return a.second < b.second; });
    return it->first;
}

// ---

void tr_torrent::on_file_completed(tr_file_index_t const file)
{
    /* close the file so that we can reopen in read-only mode as needed */
    session->close_torrent_file(*this, file);

    /* now that the file is complete and closed, we can start watching its
     * mtime timestamp for changes to know if we need to reverify pieces */
    file_mtimes_[file] = tr_time();

    /* if the torrent's current filename isn't the same as the one in the
     * metadata -- for example, if it had the ".part" suffix appended to
     * it until now -- then rename it to match the one in the metadata */
    update_file_path(file, true);
}

void tr_torrent::on_piece_completed(tr_piece_index_t const piece)
{
    piece_completed_.emit(this, piece);

    // bookkeeping
    set_needs_completeness_check();

    // if this piece completes any file, invoke the fileCompleted func for it
    for (auto [file, file_end] = fpm_.file_span_for_piece(piece); file < file_end; ++file)
    {
        if (has_file(file))
        {
            on_file_completed(file);
        }
    }
}

void tr_torrent::on_piece_failed(tr_piece_index_t const piece)
{
    tr_logAddDebugTor(this, fmt::format("Piece {}, which was just downloaded, failed its checksum test", piece));

    auto const n = piece_size(piece);
    bytes_corrupt_ += n;
    bytes_downloaded_.reduce(n);
    got_bad_piece_.emit(this, piece);
    set_has_piece(piece, false);
}

void tr_torrent::on_block_received(tr_block_index_t const block)
{
    TR_ASSERT(session->am_in_session_thread());

    if (has_block(block))
    {
        tr_logAddDebugTor(this, "we have this block already...");
        bytes_downloaded_.reduce(block_size(block));
        return;
    }

    set_dirty();

    completion_.add_block(block);

    auto const block_loc = this->block_loc(block);
    auto const first_piece = block_loc.piece;
    auto const last_piece = byte_loc(block_loc.byte + block_size(block) - 1).piece;
    for (auto piece = first_piece; piece <= last_piece; ++piece)
    {
        if (!has_piece(piece))
        {
            continue;
        }

        if (check_piece(piece))
        {
            on_piece_completed(piece);
        }
        else
        {
            on_piece_failed(piece);
        }
    }
}

// ---

std::string tr_torrentFindFile(tr_torrent const* tor, tr_file_index_t file_num)
{
    auto const found = tor->find_file(file_num);
    return std::string{ found ? found->filename().sv() : ""sv };
}

size_t tr_torrentFindFileToBuf(tr_torrent const* tor, tr_file_index_t file_num, char* buf, size_t buflen)
{
    return tr_strv_to_buf(tr_torrentFindFile(tor, file_num), buf, buflen);
}

void tr_torrent::set_download_dir(std::string_view path, bool is_new_torrent)
{
    download_dir_ = path;
    mark_changed();
    mark_edited();
    set_dirty();
    refresh_current_dir();

    if (is_new_torrent)
    {
        if (session->shouldFullyVerifyAddedTorrents() || !is_new_torrent_a_seed())
        {
            tr_torrentVerify(this);
        }
        else
        {
            completion_.set_has_all();
            recheck_completeness();
            date_done_ = date_added_; // Must be after recheck_completeness()
        }
    }
    else if (error_.error_type() == TR_STAT_LOCAL_ERROR && !set_local_error_if_files_disappeared(this))
    {
        error_.clear();
    }
}

// decide whether we should be looking for files in downloadDir or incompleteDir
void tr_torrent::refresh_current_dir()
{
    auto dir = tr_interned_string{};

    if (std::empty(incomplete_dir()))
    {
        dir = download_dir();
    }
    else if (!has_metainfo()) // no files to find
    {
        dir = incomplete_dir();
    }
    else
    {
        auto const found = find_file(0);
        dir = found ? tr_interned_string{ found->base() } : incomplete_dir();
    }

    TR_ASSERT(!std::empty(dir));
    TR_ASSERT(dir == download_dir() || dir == incomplete_dir());

    current_dir_ = dir;
}

// --- RENAME

namespace
{
namespace rename_helpers
{
bool renameArgsAreValid(tr_torrent const* tor, std::string_view oldpath, std::string_view newname)
{
    if (std::empty(oldpath) || std::empty(newname) || newname == "."sv || newname == ".."sv || tr_strv_contains(newname, '/'))
    {
        return false;
    }

    auto const newpath = tr_strv_contains(oldpath, '/') ? tr_pathbuf{ tr_sys_path_dirname(oldpath), '/', newname } :
                                                          tr_pathbuf{ newname };

    if (newpath == oldpath)
    {
        return true;
    }

    auto const newpath_as_dir = tr_pathbuf{ newpath, '/' };
    auto const n_files = tor->file_count();

    for (tr_file_index_t i = 0; i < n_files; ++i)
    {
        auto const& name = tor->file_subpath(i);
        if (newpath == name || tr_strv_starts_with(name, newpath_as_dir))
        {
            return false;
        }
    }

    return true;
}

auto renameFindAffectedFiles(tr_torrent const* tor, std::string_view oldpath)
{
    auto indices = std::vector<tr_file_index_t>{};
    auto const oldpath_as_dir = tr_pathbuf{ oldpath, '/' };
    auto const n_files = tor->file_count();

    for (tr_file_index_t i = 0; i < n_files; ++i)
    {
        auto const& name = tor->file_subpath(i);
        if (name == oldpath || tr_strv_starts_with(name, oldpath_as_dir))
        {
            indices.push_back(i);
        }
    }

    return indices;
}

int renamePath(tr_torrent const* tor, std::string_view oldpath, std::string_view newname)
{
    int err = 0;

    auto const base = tor->is_done() || std::empty(tor->incomplete_dir()) ? tor->download_dir() : tor->incomplete_dir();

    auto src = tr_pathbuf{ base, '/', oldpath };

    if (!tr_sys_path_exists(src)) /* check for it as a partial */
    {
        src += tr_torrent_files::PartialFileSuffix;
    }

    if (tr_sys_path_exists(src))
    {
        auto const parent = tr_sys_path_dirname(src);
        auto const tgt = tr_strv_ends_with(src, tr_torrent_files::PartialFileSuffix) ?
            tr_pathbuf{ parent, '/', newname, tr_torrent_files::PartialFileSuffix } :
            tr_pathbuf{ parent, '/', newname };

        auto tmp = errno;
        bool const tgt_exists = tr_sys_path_exists(tgt);
        errno = tmp;

        if (!tgt_exists)
        {
            tmp = errno;

            if (auto error = tr_error{}; !tr_sys_path_rename(src, tgt, &error))
            {
                err = error.code();
            }

            errno = tmp;
        }
    }

    return err;
}

void renameTorrentFileString(tr_torrent* tor, std::string_view oldpath, std::string_view newname, tr_file_index_t file_index)
{
    auto name = std::string{};
    auto const subpath = std::string_view{ tor->file_subpath(file_index) };
    auto const oldpath_len = std::size(oldpath);

    if (!tr_strv_contains(oldpath, '/'))
    {
        if (oldpath_len >= std::size(subpath))
        {
            name = newname;
        }
        else
        {
            name = fmt::format("{:s}/{:s}"sv, newname, subpath.substr(oldpath_len + 1));
        }
    }
    else
    {
        auto const tmp = tr_sys_path_dirname(oldpath);

        if (std::empty(tmp))
        {
            return;
        }

        if (oldpath_len >= std::size(subpath))
        {
            name = fmt::format("{:s}/{:s}"sv, tmp, newname);
        }
        else
        {
            name = fmt::format("{:s}/{:s}/{:s}"sv, tmp, newname, subpath.substr(oldpath_len + 1));
        }
    }

    if (subpath != name)
    {
        tor->set_file_subpath(file_index, name);
    }
}

} // namespace rename_helpers
} // namespace

void tr_torrent::rename_path_in_session_thread(
    std::string_view const oldpath,
    std::string_view const newname,
    tr_torrent_rename_done_func const callback,
    void* const callback_user_data)
{
    using namespace rename_helpers;

    auto error = 0;

    if (!renameArgsAreValid(this, oldpath, newname))
    {
        error = EINVAL;
    }
    else if (auto const file_indices = renameFindAffectedFiles(this, oldpath); std::empty(file_indices))
    {
        error = EINVAL;
    }
    else
    {
        error = renamePath(this, oldpath, newname);

        if (error == 0)
        {
            /* update tr_info.files */
            for (auto const& file_index : file_indices)
            {
                renameTorrentFileString(this, oldpath, newname, file_index);
            }

            /* update tr_info.name if user changed the toplevel */
            if (std::size(file_indices) == file_count() && !tr_strv_contains(oldpath, '/'))
            {
                set_name(newname);
            }

            mark_edited();
            set_dirty();
        }
    }

    mark_changed();

    if (callback != nullptr)
    {
        auto const szold = tr_pathbuf{ oldpath };
        auto const sznew = tr_pathbuf{ newname };
        (*callback)(this, szold.c_str(), sznew.c_str(), error, callback_user_data);
    }
}

void tr_torrent::rename_path(
    std::string_view oldpath,
    std::string_view newname,
    tr_torrent_rename_done_func callback,
    void* callback_user_data)
{
    this->session->run_in_session_thread(
        [this, oldpath = std::string(oldpath), newname = std::string(newname), callback, callback_user_data]()
        { rename_path_in_session_thread(oldpath, newname, callback, callback_user_data); });
}

void tr_torrentRenamePath(
    tr_torrent* tor,
    char const* oldpath,
    char const* newname,
    tr_torrent_rename_done_func callback,
    void* callback_user_data)
{
    oldpath = oldpath != nullptr ? oldpath : "";
    newname = newname != nullptr ? newname : "";

    tor->rename_path(oldpath, newname, callback, callback_user_data);
}

// ---

void tr_torrentSetFilePriorities(
    tr_torrent* tor,
    tr_file_index_t const* files,
    tr_file_index_t file_count,
    tr_priority_t priority)
{
    tor->set_file_priorities(files, file_count, priority);
}

bool tr_torrentHasMetadata(tr_torrent const* tor)
{
    return tor->has_metainfo();
}

void tr_torrent::mark_edited()
{
    this->date_edited_ = tr_time();
}

void tr_torrent::mark_changed()
{
    this->bump_date_changed(tr_time());
}

[[nodiscard]] bool tr_torrent::ensure_piece_is_checked(tr_piece_index_t piece)
{
    TR_ASSERT(piece < this->piece_count());

    if (is_piece_checked(piece))
    {
        return true;
    }

    bool const checked = check_piece(piece);
    mark_changed();
    set_dirty();

    checked_pieces_.set(piece, checked);
    return checked;
}

// --- RESUME HELPER

tr_bitfield const& tr_torrent::ResumeHelper::checked_pieces() const noexcept
{
    return tor_.checked_pieces_;
}

void tr_torrent::ResumeHelper::load_checked_pieces(tr_bitfield const& checked, time_t const* mtimes /*file_count()*/)
{
    TR_ASSERT(std::size(checked) == tor_.piece_count());
    tor_.checked_pieces_ = checked;

    auto const n_files = tor_.file_count();
    tor_.file_mtimes_.resize(n_files);

    for (size_t file = 0; file < n_files; ++file)
    {
        auto const found = tor_.find_file(file);
        auto const mtime = found ? found->last_modified_at : 0;

        tor_.file_mtimes_[file] = mtime;

        // if a file has changed, mark its pieces as unchecked
        if (mtime == 0 || mtime != mtimes[file])
        {
            auto const [piece_begin, piece_end] = tor_.piece_span_for_file(file);
            tor_.checked_pieces_.unset_span(piece_begin, piece_end);
        }
    }
}

// ---

tr_bitfield const& tr_torrent::ResumeHelper::blocks() const noexcept
{
    return tor_.completion_.blocks();
}

void tr_torrent::ResumeHelper::load_blocks(tr_bitfield blocks)
{
    tor_.completion_.set_blocks(std::move(blocks));
}

// ---

time_t tr_torrent::ResumeHelper::date_active() const noexcept
{
    return tor_.date_active_;
}

// ---

time_t tr_torrent::ResumeHelper::date_added() const noexcept
{
    return tor_.date_added_;
}

void tr_torrent::ResumeHelper::load_date_added(time_t when) noexcept
{
    tor_.date_added_ = when;
}

// ---

time_t tr_torrent::ResumeHelper::date_done() const noexcept
{
    return tor_.date_done_;
}

void tr_torrent::ResumeHelper::load_date_done(time_t when) noexcept
{
    tor_.date_done_ = when;
}

// ---

time_t tr_torrent::ResumeHelper::seconds_downloading(time_t now) const noexcept
{
    return tor_.seconds_downloading(now);
}

void tr_torrent::ResumeHelper::load_seconds_downloading_before_current_start(time_t when) noexcept
{
    tor_.seconds_downloading_before_current_start_ = when;
}

// ---

time_t tr_torrent::ResumeHelper::seconds_seeding(time_t now) const noexcept
{
    return tor_.seconds_seeding(now);
}

void tr_torrent::ResumeHelper::load_seconds_seeding_before_current_start(time_t when) noexcept
{
    tor_.seconds_seeding_before_current_start_ = when;
}

// ---

void tr_torrent::ResumeHelper::load_download_dir(std::string_view const dir) noexcept
{
    bool const is_current_dir = tor_.current_dir_ == tor_.download_dir_;
    tor_.download_dir_ = dir;
    if (is_current_dir)
    {
        tor_.current_dir_ = tor_.download_dir_;
    }
}

void tr_torrent::ResumeHelper::load_incomplete_dir(std::string_view const dir) noexcept
{
    bool const is_current_dir = tor_.current_dir_ == tor_.incomplete_dir_;
    tor_.incomplete_dir_ = dir;
    if (is_current_dir)
    {
        tor_.current_dir_ = tor_.incomplete_dir_;
    }
}

// ---

void tr_torrent::ResumeHelper::load_queue_position(size_t pos) noexcept
{
    tor_.queue_position_ = pos;
}

// ---

void tr_torrent::ResumeHelper::load_start_when_stable(bool const val) noexcept
{
    tor_.start_when_stable_ = val;
}

bool tr_torrent::ResumeHelper::start_when_stable() const noexcept
{
    return tor_.start_when_stable_;
}

// ---

std::vector<time_t> const& tr_torrent::ResumeHelper::file_mtimes() const noexcept
{
    return tor_.file_mtimes_;
}
