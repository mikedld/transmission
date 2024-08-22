// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::min
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <string_view>
#include <vector>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/bitfield.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/peer-mgr.h" /* pex */
#include "libtransmission/quark.h"
#include "libtransmission/resume.h"
#include "libtransmission/session.h"
#include "libtransmission/torrent-ctor.h"
#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"

using namespace std::literals;
using namespace libtransmission::Values;

namespace tr_resume
{
namespace
{
constexpr auto MaxRememberedPeers = 200U;

// ---

void save_peers(tr_variant::Map& map, tr_torrent const* tor)
{
    if (auto const pex = tr_peerMgrGetPeers(tor, TR_AF_INET, TR_PEERS_INTERESTING, MaxRememberedPeers); !std::empty(pex))
    {
        map.insert_or_assign(TR_KEY_peers2, tr_pex::to_variant(std::data(pex), std::size(pex)));
    }

    if (auto const pex = tr_peerMgrGetPeers(tor, TR_AF_INET6, TR_PEERS_INTERESTING, MaxRememberedPeers); !std::empty(pex))
    {
        map.insert_or_assign(TR_KEY_peers2_6, tr_pex::to_variant(std::data(pex), std::size(pex)));
    }
}

size_t add_peers(tr_torrent* tor, tr_variant::Vector const& l)
{
    auto const n_pex = std::min(std::size(l), size_t{ MaxRememberedPeers });
    auto const pex = tr_pex::from_variant(std::data(l), n_pex);
    return tr_peerMgrAddPex(tor, TR_PEER_FROM_RESUME, std::data(pex), std::size(pex));
}

auto load_peers(tr_variant::Map const& map, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    if (auto const* l = map.find_if<tr_variant::Vector>(TR_KEY_peers2); l != nullptr)
    {
        auto const num_added = add_peers(tor, *l);
        tr_logAddTraceTor(tor, fmt::format("Loaded {} IPv4 peers from resume file", num_added));
        ret = tr_resume::Peers;
    }

    if (auto const* l = map.find_if<tr_variant::Vector>(TR_KEY_peers2_6); l != nullptr)
    {
        auto const num_added = add_peers(tor, *l);
        tr_logAddTraceTor(tor, fmt::format("Loaded {} IPv6 peers from resume file", num_added));
        ret = tr_resume::Peers;
    }

    return ret;
}

// ---

void save_labels(tr_variant::Map& map, tr_torrent const* tor)
{
    auto const& labels = tor->labels();
    auto list = tr_variant::Vector{};
    list.reserve(std::size(labels));
    for (auto const& label : labels)
    {
        list.emplace_back(tr_variant::unmanaged_string(label.sv()));
    }
    map.insert_or_assign(TR_KEY_labels, std::move(list));
}

tr_resume::fields_t load_labels(tr_variant::Map const& map, tr_torrent* tor)
{
    auto const* const list = map.find_if<tr_variant::Vector>(TR_KEY_labels);
    if (list == nullptr)
    {
        return {};
    }

    auto labels = tr_torrent::labels_t{};
    labels.reserve(std::size(*list));
    for (auto const& var : *list)
    {
        if (auto sv = var.value_if<std::string_view>(); sv && !std::empty(*sv))
        {
            labels.emplace_back(*sv);
        }
    }

    tor->set_labels(labels);
    return tr_resume::Labels;
}

// ---

void save_group(tr_variant::Map& map, tr_torrent const* tor)
{
    map.insert_or_assign(TR_KEY_group, tr_variant::unmanaged_string(tor->bandwidth_group()));
}

tr_resume::fields_t load_group(tr_variant::Map const& map, tr_torrent* tor)
{
    if (auto const sv = map.value_if<std::string_view>(TR_KEY_group); sv && !std::empty(*sv))
    {
        tor->set_bandwidth_group(*sv);
        return tr_resume::Group;
    }

    return {};
}

// ---

void save_dnd(tr_variant::Map& map, tr_torrent const* tor)
{
    auto const n = tor->file_count();
    auto list = tr_variant::Vector{};
    list.reserve(n);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        list.emplace_back(!tr_torrentFile(tor, i).wanted);
    }
    map.insert_or_assign(TR_KEY_dnd, std::move(list));
}

tr_resume::fields_t load_dnd(tr_variant::Map const& map, tr_torrent* tor)
{
    auto const* const list = map.find_if<tr_variant::Vector>(TR_KEY_dnd);
    if (list == nullptr)
    {
        tr_logAddDebugTor(tor, "Couldn't load DND flags.");
        return {};
    }

    auto const n = tor->file_count();
    if (std::size(*list) != n)
    {
        tr_logAddDebugTor(
            tor,
            fmt::format(
                "Couldn't load DND flags. DND list {} has {} children; torrent has {} files",
                fmt::ptr(list),
                std::size(*list),
                n));
        return {};
    }

    auto wanted = std::vector<tr_file_index_t>{};
    auto unwanted = std::vector<tr_file_index_t>{};
    wanted.reserve(n);
    unwanted.reserve(n);

    for (tr_file_index_t i = 0; i < n; ++i)
    {
        if ((*list)[i].value_if<bool>().value_or(false))
        {
            unwanted.push_back(i);
        }
        else
        {
            wanted.push_back(i);
        }
    }

    tor->init_files_wanted(std::data(unwanted), std::size(unwanted), false);
    tor->init_files_wanted(std::data(wanted), std::size(wanted), true);

    return tr_resume::Dnd;
}

// ---

void save_file_priorities(tr_variant::Map& map, tr_torrent const* tor)
{
    auto const n = tor->file_count();
    auto list = tr_variant::Vector{};
    list.reserve(n);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        list.emplace_back(tr_torrentFile(tor, i).priority);
    }
    map.insert_or_assign(TR_KEY_priority, std::move(list));
}

tr_resume::fields_t load_file_priorities(tr_variant::Map const& map, tr_torrent* tor)
{
    auto const* const list = map.find_if<tr_variant::Vector>(TR_KEY_priority);
    auto const n = tor->file_count();
    if (list == nullptr || std::size(*list) != n)
    {
        return {};
    }

    for (tr_file_index_t i = 0; i < n; ++i)
    {
        if (auto const priority = (*list)[i].value_if<int64_t>(); priority)
        {
            tor->set_file_priority(i, static_cast<tr_priority_t>(*priority));
        }
    }

    return tr_resume::FilePriorities;
}

// ---

tr_variant::Map save_single_speed_limit(tr_torrent const* tor, tr_direction dir)
{
    auto map = tr_variant::Map{ 3 };
    map.try_emplace(TR_KEY_speed_Bps, tor->speed_limit(dir).base_quantity());
    map.try_emplace(TR_KEY_use_global_speed_limit, tor->uses_session_limits());
    map.try_emplace(TR_KEY_use_speed_limit, tor->uses_speed_limit(dir));
    return map;
}

void save_speed_limits(tr_variant::Map& map, tr_torrent const* tor)
{
    map.insert_or_assign(TR_KEY_speed_limit_down, save_single_speed_limit(tor, TR_DOWN));
    map.insert_or_assign(TR_KEY_speed_limit_up, save_single_speed_limit(tor, TR_UP));
}

void save_ratio_limits(tr_variant::Map& map, tr_torrent const* tor)
{
    auto d = tr_variant::Map{ 2 };
    d.try_emplace(TR_KEY_ratio_limit, tor->seed_ratio());
    d.try_emplace(TR_KEY_ratio_mode, tor->seed_ratio_mode());
    map.insert_or_assign(TR_KEY_ratio_limit, std::move(d));
}

void save_idle_limits(tr_variant::Map& map, tr_torrent const* tor)
{
    auto d = tr_variant::Map{ 2 };
    d.try_emplace(TR_KEY_idle_limit, tor->idle_limit_minutes());
    d.try_emplace(TR_KEY_idle_mode, tor->idle_limit_mode());
    map.insert_or_assign(TR_KEY_idle_limit, std::move(d));
}

void load_single_speed_limit(tr_variant::Map const& map, tr_direction dir, tr_torrent* tor)
{
    if (auto const i = map.value_if<int64_t>(TR_KEY_speed_Bps); i)
    {
        tor->set_speed_limit(dir, Speed{ *i, Speed::Units::Byps });
    }
    else if (auto const i2 = map.value_if<int64_t>(TR_KEY_speed); i2)
    {
        tor->set_speed_limit(dir, Speed{ *i2, Speed::Units::KByps });
    }

    if (auto const b = map.value_if<bool>(TR_KEY_use_speed_limit); b)
    {
        tor->use_speed_limit(dir, *b);
    }

    if (auto const b = map.value_if<bool>(TR_KEY_use_global_speed_limit); b)
    {
        tr_torrentUseSessionLimits(tor, *b);
    }
}

auto load_speed_limits(tr_variant::Map const& map, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    if (auto const* child = map.find_if<tr_variant::Map>(TR_KEY_speed_limit_up); child != nullptr)
    {
        load_single_speed_limit(*child, TR_UP, tor);
        ret = tr_resume::Speedlimit;
    }

    if (auto const* child = map.find_if<tr_variant::Map>(TR_KEY_speed_limit_down); child != nullptr)
    {
        load_single_speed_limit(*child, TR_DOWN, tor);
        ret = tr_resume::Speedlimit;
    }

    return ret;
}

tr_resume::fields_t load_ratio_limits(tr_variant::Map const& map, tr_torrent* tor)
{
    auto const* const d = map.find_if<tr_variant::Map>(TR_KEY_ratio_limit);
    if (d == nullptr)
    {
        return {};
    }

    if (auto const dratio = d->value_if<double>(TR_KEY_ratio_limit); dratio)
    {
        tor->set_seed_ratio(*dratio);
    }

    if (auto const i = d->value_if<int64_t>(TR_KEY_ratio_mode); i)
    {
        tor->set_seed_ratio_mode(static_cast<tr_ratiolimit>(*i));
    }

    return tr_resume::Ratiolimit;
}

tr_resume::fields_t load_idle_limits(tr_variant::Map const& map, tr_torrent* tor)
{
    auto const* const d = map.find_if<tr_variant::Map>(TR_KEY_idle_limit);
    if (d == nullptr)
    {
        return {};
    }

    if (auto const imin = d->value_if<int64_t>(TR_KEY_idle_limit); imin)
    {
        tor->set_idle_limit_minutes(*imin);
    }

    if (auto const i = d->value_if<int64_t>(TR_KEY_idle_mode); i)
    {
        tor->set_idle_limit_mode(static_cast<tr_idlelimit>(*i));
    }

    return tr_resume::Idlelimit;
}

// ---

void save_name(tr_variant::Map& map, tr_torrent const* tor)
{
    map.insert_or_assign(TR_KEY_name, tr_variant::unmanaged_string(tor->name()));
}

tr_resume::fields_t load_name(tr_variant::Map const& map, tr_torrent* tor)
{
    auto const o_name = map.value_if<std::string_view>(TR_KEY_name);
    if (!o_name)
    {
        return {};
    }

    auto const& name = tr_strv_strip(*o_name);
    if (std::empty(name))
    {
        return {};
    }

    tor->set_name(name);

    return tr_resume::Name;
}

// ---

void save_filenames(tr_variant::Map& map, tr_torrent const* tor)
{
    auto const n = tor->file_count();
    auto list = tr_variant::Vector{};
    list.reserve(n);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        list.emplace_back(tr_variant::unmanaged_string(tor->file_subpath(i)));
    }
    map.insert_or_assign(TR_KEY_files, std::move(list));
}

tr_resume::fields_t load_filenames(tr_variant::Map const& map, tr_torrent* tor)
{
    auto const* const list = map.find_if<tr_variant::Vector>(TR_KEY_files);
    if (list == nullptr)
    {
        return {};
    }

    auto const n_files = tor->file_count();
    auto const n_list = std::size(*list);
    for (tr_file_index_t i = 0; i < n_files && i < n_list; ++i)
    {
        if (auto const sv = (*list)[i].value_if<std::string_view>(); sv && !std::empty(*sv))
        {
            tor->set_file_subpath(i, *sv);
        }
    }

    return tr_resume::Filenames;
}

// ---

void save_queue_state(tr_variant::Map& map, tr_torrent const* tor)
{
    map.insert_or_assign(TR_KEY_queue_position, tor->queue_position());
    map.insert_or_assign(TR_KEY_is_queued, tor->is_queued(tor->queue_direction()));
}

auto load_queue_state(tr_variant::Map const& map, tr_torrent* tor, tr_torrent::ResumeHelper& helper)
{
    auto ret = tr_resume::fields_t{};
    if (auto val = map.value_if<int64_t>({ TR_KEY_queue_position, TR_KEY_queue_position_camel }))
    {
        helper.load_queue_position(*val);
        ret = tr_resume::QueueState;
    }

    if (auto val = map.value_if<bool>(TR_KEY_is_queued))
    {
        tor->set_is_queued(*val);
        ret = tr_resume::QueueState;
    }

    return ret;
}

// ---

tr_variant bitfield_to_raw(tr_bitfield const& b)
{
    if (b.has_none() || std::empty(b))
    {
        return tr_variant::unmanaged_string("none"sv);
    }

    if (b.has_all())
    {
        return tr_variant::unmanaged_string("all"sv);
    }

    return tr_variant::make_raw(b.raw());
}

void raw_to_bitfield(tr_bitfield& bitfield, std::string_view const raw)
{
    if (std::empty(raw) || raw == "none"sv)
    {
        bitfield.set_has_none();
    }
    else if (raw == "all"sv)
    {
        bitfield.set_has_all();
    }
    else
    {
        bitfield.set_raw(reinterpret_cast<uint8_t const*>(std::data(raw)), std::size(raw));
    }
}

void save_progress(tr_variant::Map& map, tr_torrent::ResumeHelper const& helper)
{
    auto prog = tr_variant::Map{ 3 };

    // add the mtimes
    auto const& mtimes = helper.file_mtimes();
    auto const n = std::size(mtimes);
    auto l = tr_variant::Vector{};
    l.reserve(n);
    for (auto const& mtime : mtimes)
    {
        l.emplace_back(mtime);
    }
    prog.try_emplace(TR_KEY_mtimes, std::move(l));

    // add the 'checked pieces' bitfield
    prog.try_emplace(TR_KEY_pieces, bitfield_to_raw(helper.checked_pieces()));

    // add the blocks bitfield
    prog.try_emplace(TR_KEY_blocks, bitfield_to_raw(helper.blocks()));

    map.insert_or_assign(TR_KEY_progress, std::move(prog));
}

/*
 * Transmission has iterated through a few strategies here, so the
 * code has some added complexity to support older approaches.
 *
 * Current approach: 'progress' is a dict with two entries:
 * - 'pieces' a bitfield for whether each piece has been checked.
 * - 'mtimes', an array of per-file timestamps
 * On startup, 'pieces' is loaded. Then we check to see if the disk
 * mtimes differ from the 'mtimes' list. Changed files have their
 * pieces cleared from the bitset.
 *
 * Second approach (2.20 - 3.00): the 'progress' dict had a
 * 'time_checked' entry which was a list with file_count items.
 * Each item was either a list of per-piece timestamps, or a
 * single timestamp if either all or none of the pieces had been
 * tested more recently than the file's mtime.
 *
 * First approach (pre-2.20) had an "mtimes" list identical to
 * the current approach, but not the 'pieces' bitfield.
 */
tr_resume::fields_t load_progress(tr_variant::Map const& map, tr_torrent* tor, tr_torrent::ResumeHelper& helper)
{
    auto const* const prog = map.find_if<tr_variant::Map>(TR_KEY_progress);
    if (prog == nullptr)
    {
        return {};
    }

    /// CHECKED PIECES

    auto checked = tr_bitfield{ tor->piece_count() };
    auto mtimes = std::vector<time_t>{};
    auto const n_files = tor->file_count();
    mtimes.reserve(n_files);

    // try to load mtimes
    if (auto const* l = prog->find_if<tr_variant::Vector>(TR_KEY_mtimes); l != nullptr)
    {
        for (auto const& var : *l)
        {
            auto const t = var.value_if<int64_t>();
            if (!t)
            {
                break;
            }

            mtimes.push_back(*t);
        }
    }

    // try to load the piece-checked bitfield
    if (auto const sv = prog->value_if<std::string_view>(TR_KEY_pieces); sv)
    {
        raw_to_bitfield(checked, *sv);
    }

    // maybe it's a .resume file from [2.20 - 3.00] with the per-piece mtimes
    if (auto const* l = prog->find_if<tr_variant::Vector>(TR_KEY_time_checked); l != nullptr)
    {
        for (tr_file_index_t fi = 0, n_l = std::min(n_files, std::size(*l)); fi < n_l; ++fi)
        {
            auto const& b = (*l)[fi];
            auto time_checked = time_t{};

            if (auto const t = b.value_if<int64_t>(); t)
            {
                time_checked = static_cast<time_t>(*t);
            }
            else if (auto const* ll = b.get_if<tr_variant::Vector>(); ll != nullptr)
            {
                // The first element (idx 0) stores a base value for all piece timestamps,
                // which would be the value of the smallest piece timestamp minus 1.
                //
                // The rest of the elements are the timestamp of each piece, stored as
                // an offset to the base value.
                // i.e. idx 1 <-> piece 0, idx 2 <-> piece 1, ...
                //      timestamp of piece n = idx 0 + idx n+1
                //
                // Pieces that haven't been checked will have a timestamp offset of 0.
                // They can be differentiated from the oldest checked piece(s) since the
                // offset for any checked pieces will be at least 1.

                auto const base = (*ll)[0].value_if<int64_t>().value_or(0);

                auto const [piece_begin, piece_end] = tor->piece_span_for_file(fi);
                auto const n_ll = std::size(*ll);
                auto const n_pieces = piece_end - piece_begin;
                time_checked = std::numeric_limits<time_t>::max();
                for (tr_piece_index_t i = 1; time_checked > time_t{} && i <= n_pieces && i < n_ll; ++i)
                {
                    auto const offset = (*ll)[i].value_if<int64_t>().value_or(0);
                    time_checked = std::min(time_checked, offset != 0 ? static_cast<time_t>(base + offset) : time_t{});
                }
            }

            mtimes.push_back(time_checked);
        }
    }

    if (std::size(mtimes) != n_files)
    {
        tr_logAddDebugTor(tor, fmt::format("Couldn't load mtimes: expected {} got {}", std::size(mtimes), n_files));
        // if resizing grows the vector, we'll get 0 mtimes for the
        // new items which is exactly what we want since the pieces
        // in an unknown state should be treated as untested
        mtimes.resize(n_files);
    }

    helper.load_checked_pieces(checked, std::data(mtimes));

    /// COMPLETION

    auto blocks = tr_bitfield{ tor->block_count() };
    char const* err = nullptr;
    if (auto const b = prog->find(TR_KEY_blocks); b != std::end(*prog))
    {
        if (auto const sv = b->second.value_if<std::string_view>(); sv)
        {
            raw_to_bitfield(blocks, *sv);
        }
        else
        {
            err = "Invalid value for 'blocks'";
        }
    }
    else if (auto const raw = prog->value_if<std::string_view>(TR_KEY_bitfield); raw)
    {
        blocks.set_raw(reinterpret_cast<uint8_t const*>(std::data(*raw)), std::size(*raw));
    }
    else
    {
        err = "Couldn't find 'blocks' or 'bitfield'";
    }

    if (err != nullptr)
    {
        tr_logAddDebugTor(tor, fmt::format("Torrent needs to be verified - {}", err));
    }
    else
    {
        helper.load_blocks(blocks);
    }

    return tr_resume::Progress;
}

// ---

tr_resume::fields_t load_from_file(tr_torrent* tor, tr_torrent::ResumeHelper& helper, tr_resume::fields_t fields_to_load)
{
    TR_ASSERT(tr_isTorrent(tor));

    tr_torrent_metainfo::migrate_file(tor->session->resumeDir(), tor->name(), tor->info_hash_string(), ".resume"sv);

    auto const filename = tor->resume_file();
    auto benc = std::vector<char>{};
    if (!tr_sys_path_exists(filename) || !tr_file_read(filename, benc))
    {
        return {};
    }

    auto serde = tr_variant_serde::benc();
    auto otop = serde.inplace().parse(benc);
    if (!otop)
    {
        tr_logAddDebugTor(tor, fmt::format("Couldn't read '{}': {}", filename, serde.error_.message()));
        return {};
    }

    auto const* const p_map = otop->get_if<tr_variant::Map>();
    if (p_map == nullptr)
    {
        tr_logAddDebugTor(tor, fmt::format("Resume file '{}' does not contain a benc dict", filename));
        return {};
    }
    auto const& map = *p_map;

    tr_logAddDebugTor(tor, fmt::format("Read resume file '{}'", filename));
    auto fields_loaded = tr_resume::fields_t{};

    if (auto i = map.value_if<int64_t>(TR_KEY_corrupt); i && (fields_to_load & tr_resume::Corrupt) != 0)
    {
        tor->bytes_corrupt_.set_prev(*i);
        fields_loaded |= tr_resume::Corrupt;
    }

    if (auto sv = map.value_if<std::string_view>(TR_KEY_destination);
        sv && !std::empty(*sv) && (fields_to_load & (tr_resume::Progress | tr_resume::DownloadDir)) != 0)
    {
        helper.load_download_dir(*sv);
        fields_loaded |= tr_resume::DownloadDir;
    }

    if (auto sv = map.value_if<std::string_view>(TR_KEY_incomplete_dir);
        sv && !std::empty(*sv) && (fields_to_load & (tr_resume::Progress | tr_resume::IncompleteDir)) != 0)
    {
        helper.load_incomplete_dir(*sv);
        fields_loaded |= tr_resume::IncompleteDir;
    }

    if (auto i = map.value_if<int64_t>(TR_KEY_downloaded); i && (fields_to_load & tr_resume::Downloaded) != 0)
    {
        tor->bytes_downloaded_.set_prev(*i);
        fields_loaded |= tr_resume::Downloaded;
    }

    if (auto i = map.value_if<int64_t>(TR_KEY_uploaded); i && (fields_to_load & tr_resume::Uploaded) != 0)
    {
        tor->bytes_uploaded_.set_prev(*i);
        fields_loaded |= tr_resume::Uploaded;
    }

    if (auto i = map.value_if<int64_t>(TR_KEY_max_peers); i && (fields_to_load & tr_resume::MaxPeers) != 0)
    {
        tor->set_peer_limit(static_cast<uint16_t>(*i));
        fields_loaded |= tr_resume::MaxPeers;
    }

    if (auto b = map.value_if<bool>(TR_KEY_paused); b && (fields_to_load & tr_resume::Run) != 0)
    {
        helper.load_start_when_stable(!*b);
        fields_loaded |= tr_resume::Run;
    }

    if (auto i = map.value_if<int64_t>(TR_KEY_added_date); i && (fields_to_load & tr_resume::AddedDate) != 0)
    {
        helper.load_date_added(static_cast<time_t>(*i));
        fields_loaded |= tr_resume::AddedDate;
    }

    if (auto i = map.value_if<int64_t>(TR_KEY_done_date); i && (fields_to_load & tr_resume::DoneDate) != 0)
    {
        helper.load_date_done(static_cast<time_t>(*i));
        fields_loaded |= tr_resume::DoneDate;
    }

    if (auto i = map.value_if<int64_t>(TR_KEY_activity_date); i && (fields_to_load & tr_resume::ActivityDate) != 0)
    {
        tor->set_date_active(*i);
        fields_loaded |= tr_resume::ActivityDate;
    }

    if (auto i = map.value_if<int64_t>(TR_KEY_seeding_time_seconds); i && (fields_to_load & tr_resume::TimeSeeding) != 0)
    {
        helper.load_seconds_seeding_before_current_start(*i);
        fields_loaded |= tr_resume::TimeSeeding;
    }

    if (auto i = map.value_if<int64_t>(TR_KEY_downloading_time_seconds);
        i && (fields_to_load & tr_resume::TimeDownloading) != 0)
    {
        helper.load_seconds_downloading_before_current_start(*i);
        fields_loaded |= tr_resume::TimeDownloading;
    }

    if (auto i = map.value_if<int64_t>({ TR_KEY_bandwidth_priority, TR_KEY_bandwidth_priority_kebab });
        i && (fields_to_load & tr_resume::BandwidthPriority) != 0 && tr_isPriority(static_cast<tr_priority_t>(*i)))
    {
        tr_torrentSetPriority(tor, static_cast<tr_priority_t>(*i));
        fields_loaded |= tr_resume::BandwidthPriority;
    }

    if (auto b = map.value_if<bool>(TR_KEY_sequentialDownload); b && (fields_to_load & tr_resume::SequentialDownload) != 0)
    {
        tor->set_sequential_download(*b);
        fields_loaded |= tr_resume::SequentialDownload;
    }

    if ((fields_to_load & tr_resume::Peers) != 0)
    {
        fields_loaded |= load_peers(map, tor);
    }

    // Note: load_filenames() must come before load_progress()
    // so that load_progress() -> helper.load_checked_pieces() -> tor_.find_file()
    // will know where to look
    if ((fields_to_load & tr_resume::Filenames) != 0)
    {
        fields_loaded |= load_filenames(map, tor);
    }

    // Note: load_progress() should come before load_file_priorities()
    // so that we can skip loading priorities iff the torrent is a
    // seed or a partial seed.
    if ((fields_to_load & tr_resume::Progress) != 0)
    {
        fields_loaded |= load_progress(map, tor, helper);
    }

    if (!tor->is_done() && (fields_to_load & tr_resume::FilePriorities) != 0)
    {
        fields_loaded |= load_file_priorities(map, tor);
    }

    if ((fields_to_load & tr_resume::Dnd) != 0)
    {
        fields_loaded |= load_dnd(map, tor);
    }

    if ((fields_to_load & tr_resume::Speedlimit) != 0)
    {
        fields_loaded |= load_speed_limits(map, tor);
    }

    if ((fields_to_load & tr_resume::Ratiolimit) != 0)
    {
        fields_loaded |= load_ratio_limits(map, tor);
    }

    if ((fields_to_load & tr_resume::Idlelimit) != 0)
    {
        fields_loaded |= load_idle_limits(map, tor);
    }

    if ((fields_to_load & tr_resume::Name) != 0)
    {
        fields_loaded |= load_name(map, tor);
    }

    if ((fields_to_load & tr_resume::Labels) != 0)
    {
        fields_loaded |= load_labels(map, tor);
    }

    if ((fields_to_load & tr_resume::Group) != 0)
    {
        fields_loaded |= load_group(map, tor);
    }

    if ((fields_to_load & tr_resume::QueueState) != 0)
    {
        fields_loaded |= load_queue_state(map, tor, helper);
    }

    return fields_loaded;
}

auto set_from_ctor(
    tr_torrent* tor,
    tr_torrent::ResumeHelper& helper,
    tr_resume::fields_t const fields,
    tr_ctor const& ctor,
    tr_ctorMode const mode)
{
    auto ret = tr_resume::fields_t{};

    if ((fields & tr_resume::Run) != 0)
    {
        if (auto const val = ctor.paused(mode); val)
        {
            helper.load_start_when_stable(!*val);
            ret |= tr_resume::Run;
        }
    }

    if ((fields & tr_resume::MaxPeers) != 0)
    {
        if (auto const val = ctor.peer_limit(mode); val)
        {
            tor->set_peer_limit(*val);
            ret |= tr_resume::MaxPeers;
        }
    }

    if ((fields & tr_resume::DownloadDir) != 0)
    {
        if (auto const& val = ctor.download_dir(mode); !std::empty(val))
        {
            helper.load_download_dir(val);
            ret |= tr_resume::DownloadDir;
        }
    }

    return ret;
}

auto use_mandatory_fields(
    tr_torrent* const tor,
    tr_torrent::ResumeHelper& helper,
    tr_resume::fields_t const fields,
    tr_ctor const& ctor)
{
    return set_from_ctor(tor, helper, fields, ctor, TR_FORCE);
}

auto use_fallback_fields(
    tr_torrent* const tor,
    tr_torrent::ResumeHelper& helper,
    tr_resume::fields_t const fields,
    tr_ctor const& ctor)
{
    return set_from_ctor(tor, helper, fields, ctor, TR_FALLBACK);
}
} // namespace

fields_t load(tr_torrent* tor, tr_torrent::ResumeHelper& helper, fields_t fields_to_load, tr_ctor const& ctor)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto ret = fields_t{};

    ret |= use_mandatory_fields(tor, helper, fields_to_load, ctor);
    fields_to_load &= ~ret;
    ret |= load_from_file(tor, helper, fields_to_load);
    fields_to_load &= ~ret;
    ret |= use_fallback_fields(tor, helper, fields_to_load, ctor);

    return ret;
}

void save(tr_torrent* const tor, tr_torrent::ResumeHelper const& helper)
{
    if (!tr_isTorrent(tor))
    {
        return;
    }

    auto map = tr_variant::Map{ 50 }; // arbitrary "big enough" number
    auto const now = tr_time();
    map.try_emplace(TR_KEY_seeding_time_seconds, helper.seconds_seeding(now));
    map.try_emplace(TR_KEY_downloading_time_seconds, helper.seconds_downloading(now));
    map.try_emplace(TR_KEY_activity_date, helper.date_active());
    map.try_emplace(TR_KEY_added_date, helper.date_added());
    map.try_emplace(TR_KEY_corrupt, tor->bytes_corrupt_.ever());
    map.try_emplace(TR_KEY_done_date, helper.date_done());
    map.try_emplace(TR_KEY_destination, tr_variant::unmanaged_string(tor->download_dir().sv()));

    if (!std::empty(tor->incomplete_dir()))
    {
        map.try_emplace(TR_KEY_incomplete_dir, tr_variant::unmanaged_string(tor->incomplete_dir().sv()));
    }

    map.try_emplace(TR_KEY_downloaded, tor->bytes_downloaded_.ever());
    map.try_emplace(TR_KEY_uploaded, tor->bytes_uploaded_.ever());
    map.try_emplace(TR_KEY_max_peers, tor->peer_limit());
    map.try_emplace(TR_KEY_bandwidth_priority, tor->get_priority());
    map.try_emplace(TR_KEY_paused, !helper.start_when_stable());
    map.try_emplace(TR_KEY_sequentialDownload, tor->is_sequential_download());
    save_peers(map, tor);

    if (tor->has_metainfo())
    {
        save_file_priorities(map, tor);
        save_dnd(map, tor);
        save_progress(map, helper);
    }

    save_speed_limits(map, tor);
    save_ratio_limits(map, tor);
    save_idle_limits(map, tor);
    save_filenames(map, tor);
    save_name(map, tor);
    save_labels(map, tor);
    save_group(map, tor);
    save_queue_state(map, tor);

    auto serde = tr_variant_serde::benc();
    if (!serde.to_file(tr_variant{ std::move(map) }, tor->resume_file()))
    {
        tor->error().set_local_error(fmt::format("Unable to save resume file: {:s}", serde.error_.message()));
    }
}

} // namespace tr_resume
