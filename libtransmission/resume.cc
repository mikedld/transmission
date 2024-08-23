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
constexpr int MaxRememberedPeers = 200;

// ---

void savePeers(tr_variant* dict, tr_torrent const* tor)
{
    if (auto const pex = tr_peerMgrGetPeers(tor, TR_AF_INET, TR_PEERS_INTERESTING, MaxRememberedPeers); !std::empty(pex))
    {
        tr_variantDictAddRaw(dict, TR_KEY_peers2, std::data(pex), sizeof(tr_pex) * std::size(pex));
    }

    if (auto const pex = tr_peerMgrGetPeers(tor, TR_AF_INET6, TR_PEERS_INTERESTING, MaxRememberedPeers); !std::empty(pex))
    {
        tr_variantDictAddRaw(dict, TR_KEY_peers2_6, std::data(pex), sizeof(tr_pex) * std::size(pex));
    }
}

size_t addPeers(tr_torrent* tor, uint8_t const* buf, size_t buflen)
{
    size_t const n_in = buflen / sizeof(tr_pex);
    size_t const n_pex = std::min(n_in, size_t{ MaxRememberedPeers });

    auto pex = std::array<tr_pex, MaxRememberedPeers>{};
    memcpy(std::data(pex), buf, sizeof(tr_pex) * n_pex);
    return tr_peerMgrAddPex(tor, TR_PEER_FROM_RESUME, std::data(pex), n_pex);
}

auto loadPeers(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    uint8_t const* str = nullptr;
    auto len = size_t{};
    if (tr_variantDictFindRaw(dict, TR_KEY_peers2, &str, &len))
    {
        size_t const num_added = addPeers(tor, str, len);
        tr_logAddTraceTor(tor, fmt::format("Loaded {} IPv4 peers from resume file", num_added));
        ret = tr_resume::Peers;
    }

    if (tr_variantDictFindRaw(dict, TR_KEY_peers2_6, &str, &len))
    {
        size_t const num_added = addPeers(tor, str, len);
        tr_logAddTraceTor(tor, fmt::format("Loaded {} IPv6 peers from resume file", num_added));
        ret = tr_resume::Peers;
    }

    return ret;
}

// ---

void saveLabels(tr_variant* dict, tr_torrent const* tor)
{
    auto const& labels = tor->labels();
    tr_variant* list = tr_variantDictAddList(dict, TR_KEY_labels, std::size(labels));
    for (auto const& label : labels)
    {
        tr_variantListAddStrView(list, label.sv());
    }
}

tr_resume::fields_t loadLabels(tr_variant* dict, tr_torrent* tor)
{
    tr_variant* list = nullptr;
    if (!tr_variantDictFindList(dict, TR_KEY_labels, &list))
    {
        return {};
    }

    auto const n = tr_variantListSize(list);
    auto labels = tr_torrent::labels_t{};
    labels.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        auto sv = std::string_view{};
        if (tr_variantGetStrView(tr_variantListChild(list, i), &sv) && !std::empty(sv))
        {
            labels.emplace_back(sv);
        }
    }

    tor->set_labels(labels);
    return tr_resume::Labels;
}

// ---

void saveGroup(tr_variant* dict, tr_torrent const* tor)
{
    tr_variantDictAddStrView(dict, TR_KEY_group, tor->bandwidth_group());
}

tr_resume::fields_t loadGroup(tr_variant* dict, tr_torrent* tor)
{
    if (std::string_view group_name; tr_variantDictFindStrView(dict, TR_KEY_group, &group_name) && !std::empty(group_name))
    {
        tor->set_bandwidth_group(group_name);
        return tr_resume::Group;
    }

    return {};
}

// ---

void saveDND(tr_variant* dict, tr_torrent const* tor)
{
    auto const n = tor->file_count();
    tr_variant* const list = tr_variantDictAddList(dict, TR_KEY_dnd, n);

    for (tr_file_index_t i = 0; i < n; ++i)
    {
        tr_variantListAddBool(list, !tr_torrentFile(tor, i).wanted);
    }
}

tr_resume::fields_t loadDND(tr_variant* dict, tr_torrent* tor)
{
    tr_variant* list = nullptr;
    auto const n = tor->file_count();

    if (tr_variantDictFindList(dict, TR_KEY_dnd, &list) && tr_variantListSize(list) == n)
    {
        auto wanted = std::vector<tr_file_index_t>{};
        auto unwanted = std::vector<tr_file_index_t>{};
        wanted.reserve(n);
        unwanted.reserve(n);

        for (tr_file_index_t i = 0; i < n; ++i)
        {
            auto tmp = false;
            if (tr_variantGetBool(tr_variantListChild(list, i), &tmp) && tmp)
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

    tr_logAddDebugTor(
        tor,
        fmt::format(
            "Couldn't load DND flags. DND list {} has {} children; torrent has {} files",
            fmt::ptr(list),
            tr_variantListSize(list),
            n));
    return {};
}

// ---

void saveFilePriorities(tr_variant* dict, tr_torrent const* tor)
{
    auto const n = tor->file_count();

    tr_variant* const list = tr_variantDictAddList(dict, TR_KEY_priority, n);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        tr_variantListAddInt(list, tr_torrentFile(tor, i).priority);
    }
}

tr_resume::fields_t loadFilePriorities(tr_variant* dict, tr_torrent* tor)
{
    auto const n = tor->file_count();
    tr_variant* list = nullptr;
    if (tr_variantDictFindList(dict, TR_KEY_priority, &list) && tr_variantListSize(list) == n)
    {
        for (tr_file_index_t i = 0; i < n; ++i)
        {
            auto priority = int64_t{};
            if (tr_variantGetInt(tr_variantListChild(list, i), &priority))
            {
                tor->set_file_priority(i, tr_priority_t(priority));
            }
        }

        return tr_resume::FilePriorities;
    }

    return {};
}

// ---

void saveSingleSpeedLimit(tr_variant* d, tr_torrent const* tor, tr_direction dir)
{
    tr_variantDictReserve(d, 3);
    tr_variantDictAddInt(d, TR_KEY_speed_Bps, tor->speed_limit(dir).base_quantity());
    tr_variantDictAddBool(d, TR_KEY_use_global_speed_limit, tor->uses_session_limits());
    tr_variantDictAddBool(d, TR_KEY_use_speed_limit, tor->uses_speed_limit(dir));
}

void saveSpeedLimits(tr_variant* dict, tr_torrent const* tor)
{
    saveSingleSpeedLimit(tr_variantDictAddDict(dict, TR_KEY_speed_limit_down, 0), tor, TR_DOWN);
    saveSingleSpeedLimit(tr_variantDictAddDict(dict, TR_KEY_speed_limit_up, 0), tor, TR_UP);
}

void saveRatioLimits(tr_variant* dict, tr_torrent const* tor)
{
    tr_variant* d = tr_variantDictAddDict(dict, TR_KEY_ratio_limit, 2);
    tr_variantDictAddReal(d, TR_KEY_ratio_limit, tor->seed_ratio());
    tr_variantDictAddInt(d, TR_KEY_ratio_mode, tor->seed_ratio_mode());
}

void saveIdleLimits(tr_variant* dict, tr_torrent const* tor)
{
    tr_variant* d = tr_variantDictAddDict(dict, TR_KEY_idle_limit, 2);
    tr_variantDictAddInt(d, TR_KEY_idle_limit, tor->idle_limit_minutes());
    tr_variantDictAddInt(d, TR_KEY_idle_mode, tor->idle_limit_mode());
}

void loadSingleSpeedLimit(tr_variant* d, tr_direction dir, tr_torrent* tor)
{
    if (auto val = int64_t{}; tr_variantDictFindInt(d, TR_KEY_speed_Bps, &val))
    {
        tor->set_speed_limit(dir, Speed{ val, Speed::Units::Byps });
    }
    else if (tr_variantDictFindInt(d, TR_KEY_speed, &val))
    {
        tor->set_speed_limit(dir, Speed{ val, Speed::Units::KByps });
    }

    if (auto val = bool{}; tr_variantDictFindBool(d, TR_KEY_use_speed_limit, &val))
    {
        tor->use_speed_limit(dir, val);
    }

    if (auto val = bool{}; tr_variantDictFindBool(d, TR_KEY_use_global_speed_limit, &val))
    {
        tr_torrentUseSessionLimits(tor, val);
    }
}

auto loadSpeedLimits(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    if (tr_variant* child = nullptr; tr_variantDictFindDict(dict, TR_KEY_speed_limit_up, &child))
    {
        loadSingleSpeedLimit(child, TR_UP, tor);
        ret = tr_resume::Speedlimit;
    }

    if (tr_variant* child = nullptr; tr_variantDictFindDict(dict, TR_KEY_speed_limit_down, &child))
    {
        loadSingleSpeedLimit(child, TR_DOWN, tor);
        ret = tr_resume::Speedlimit;
    }

    return ret;
}

tr_resume::fields_t loadRatioLimits(tr_variant* dict, tr_torrent* tor)
{
    if (tr_variant* d = nullptr; tr_variantDictFindDict(dict, TR_KEY_ratio_limit, &d))
    {
        if (auto dratio = double{}; tr_variantDictFindReal(d, TR_KEY_ratio_limit, &dratio))
        {
            tor->set_seed_ratio(dratio);
        }

        if (auto i = int64_t{}; tr_variantDictFindInt(d, TR_KEY_ratio_mode, &i))
        {
            tor->set_seed_ratio_mode(static_cast<tr_ratiolimit>(i));
        }

        return tr_resume::Ratiolimit;
    }

    return {};
}

tr_resume::fields_t loadIdleLimits(tr_variant* dict, tr_torrent* tor)
{
    if (tr_variant* d = nullptr; tr_variantDictFindDict(dict, TR_KEY_idle_limit, &d))
    {
        if (auto imin = int64_t{}; tr_variantDictFindInt(d, TR_KEY_idle_limit, &imin))
        {
            tor->set_idle_limit_minutes(imin);
        }

        if (auto i = int64_t{}; tr_variantDictFindInt(d, TR_KEY_idle_mode, &i))
        {
            tor->set_idle_limit_mode(static_cast<tr_idlelimit>(i));
        }

        return tr_resume::Idlelimit;
    }

    return {};
}

// ---

void saveName(tr_variant* dict, tr_torrent const* tor)
{
    tr_variantDictAddStrView(dict, TR_KEY_name, tor->name());
}

tr_resume::fields_t loadName(tr_variant* dict, tr_torrent* tor)
{
    auto name = std::string_view{};
    if (!tr_variantDictFindStrView(dict, TR_KEY_name, &name))
    {
        return {};
    }

    name = tr_strv_strip(name);
    if (std::empty(name))
    {
        return {};
    }

    tor->set_name(name);

    return tr_resume::Name;
}

// ---

void saveFilenames(tr_variant* dict, tr_torrent const* tor)
{
    auto const n = tor->file_count();
    tr_variant* const list = tr_variantDictAddList(dict, TR_KEY_files, n);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        tr_variantListAddStrView(list, tor->file_subpath(i));
    }
}

tr_resume::fields_t loadFilenames(tr_variant* dict, tr_torrent* tor)
{
    tr_variant* list = nullptr;
    if (!tr_variantDictFindList(dict, TR_KEY_files, &list))
    {
        return {};
    }

    auto const n_files = tor->file_count();
    auto const n_list = tr_variantListSize(list);
    for (tr_file_index_t i = 0; i < n_files && i < n_list; ++i)
    {
        auto sv = std::string_view{};
        if (tr_variantGetStrView(tr_variantListChild(list, i), &sv) && !std::empty(sv))
        {
            tor->set_file_subpath(i, sv);
        }
    }

    return tr_resume::Filenames;
}

// ---

void bitfieldToRaw(tr_bitfield const& b, tr_variant* benc)
{
    if (b.has_none() || std::empty(b))
    {
        *benc = tr_variant::unmanaged_string("none"sv);
    }
    else if (b.has_all())
    {
        *benc = tr_variant::unmanaged_string("all"sv);
    }
    else
    {
        auto const raw = b.raw();
        *benc = std::string_view{ reinterpret_cast<char const*>(raw.data()), std::size(raw) };
    }
}

void rawToBitfield(tr_bitfield& bitfield, uint8_t const* raw, size_t rawlen)
{
    if (raw == nullptr || rawlen == 0 || (rawlen == 4 && memcmp(raw, "none", 4) == 0))
    {
        bitfield.set_has_none();
    }
    else if (rawlen == 3 && memcmp(raw, "all", 3) == 0)
    {
        bitfield.set_has_all();
    }
    else
    {
        bitfield.set_raw(raw, rawlen);
    }
}

void saveProgress(tr_variant* dict, tr_torrent::ResumeHelper const& helper)
{
    tr_variant* const prog = tr_variantDictAddDict(dict, TR_KEY_progress, 4);

    // add the mtimes
    auto const& mtimes = helper.file_mtimes();
    auto const n = std::size(mtimes);
    tr_variant* const l = tr_variantDictAddList(prog, TR_KEY_mtimes, n);
    for (auto const& mtime : mtimes)
    {
        tr_variantListAddInt(l, mtime);
    }

    // add the 'checked pieces' bitfield
    bitfieldToRaw(helper.checked_pieces(), tr_variantDictAdd(prog, TR_KEY_pieces));

    // add the blocks bitfield
    bitfieldToRaw(helper.blocks(), tr_variantDictAdd(prog, TR_KEY_blocks));
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
 * 3.10, but not the 'pieces' bitfield.
 */
tr_resume::fields_t loadProgress(tr_variant* dict, tr_torrent* tor, tr_torrent::ResumeHelper& helper)
{
    if (tr_variant* prog = nullptr; tr_variantDictFindDict(dict, TR_KEY_progress, &prog))
    {
        /// CHECKED PIECES

        auto checked = tr_bitfield(tor->piece_count());
        auto mtimes = std::vector<time_t>{};
        auto const n_files = tor->file_count();
        mtimes.reserve(n_files);

        // try to load mtimes
        tr_variant* l = nullptr;
        if (tr_variantDictFindList(prog, TR_KEY_mtimes, &l))
        {
            auto fi = size_t{};
            auto t = int64_t{};
            while (tr_variantGetInt(tr_variantListChild(l, fi++), &t))
            {
                mtimes.push_back(t);
            }
        }

        // try to load the piece-checked bitfield
        uint8_t const* raw = nullptr;
        auto rawlen = size_t{};
        if (tr_variantDictFindRaw(prog, TR_KEY_pieces, &raw, &rawlen))
        {
            rawToBitfield(checked, raw, rawlen);
        }

        // maybe it's a .resume file from [2.20 - 3.00] with the per-piece mtimes
        if (tr_variantDictFindList(prog, TR_KEY_time_checked, &l))
        {
            for (tr_file_index_t fi = 0; fi < n_files; ++fi)
            {
                tr_variant* const b = tr_variantListChild(l, fi);
                auto time_checked = time_t{};

                if (b != nullptr && b->holds_alternative<int64_t>())
                {
                    auto t = int64_t{};
                    tr_variantGetInt(b, &t);
                    time_checked = time_t(t);
                }
                else if (b != nullptr && b->holds_alternative<tr_variant::Vector>())
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

                    auto offset = int64_t{};
                    tr_variantGetInt(tr_variantListChild(b, 0), &offset);

                    auto const [piece_begin, piece_end] = tor->piece_span_for_file(fi);
                    time_checked = std::numeric_limits<time_t>::max();
                    for (tr_piece_index_t i = 1, n = piece_end - piece_begin; i <= n; ++i)
                    {
                        auto t = int64_t{};
                        tr_variantGetInt(tr_variantListChild(b, i), &t);
                        time_checked = std::min(time_checked, time_t(t != 0 ? t + offset : 0));
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
        if (tr_variant const* const b = tr_variantDictFind(prog, TR_KEY_blocks); b != nullptr)
        {
            uint8_t const* buf = nullptr;
            auto buflen = size_t{};

            if (!tr_variantGetRaw(b, &buf, &buflen))
            {
                err = "Invalid value for \"blocks\"";
            }
            else
            {
                rawToBitfield(blocks, buf, buflen);
            }
        }
        else if (tr_variantDictFindRaw(prog, TR_KEY_bitfield, &raw, &rawlen))
        {
            blocks.set_raw(raw, rawlen);
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

    return {};
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
    auto& top = *otop;

    tr_logAddDebugTor(tor, fmt::format("Read resume file '{}'", filename));
    auto fields_loaded = tr_resume::fields_t{};
    auto i = int64_t{};
    auto sv = std::string_view{};

    if ((fields_to_load & tr_resume::Corrupt) != 0 && tr_variantDictFindInt(&top, TR_KEY_corrupt, &i))
    {
        tor->bytes_corrupt_.set_prev(i);
        fields_loaded |= tr_resume::Corrupt;
    }

    if ((fields_to_load & (tr_resume::Progress | tr_resume::DownloadDir)) != 0 &&
        tr_variantDictFindStrView(&top, TR_KEY_destination, &sv) && !std::empty(sv))
    {
        helper.load_download_dir(sv);
        fields_loaded |= tr_resume::DownloadDir;
    }

    if ((fields_to_load & (tr_resume::Progress | tr_resume::IncompleteDir)) != 0 &&
        tr_variantDictFindStrView(&top, TR_KEY_incomplete_dir, &sv) && !std::empty(sv))
    {
        helper.load_incomplete_dir(sv);
        fields_loaded |= tr_resume::IncompleteDir;
    }

    if ((fields_to_load & tr_resume::Downloaded) != 0 && tr_variantDictFindInt(&top, TR_KEY_downloaded, &i))
    {
        tor->bytes_downloaded_.set_prev(i);
        fields_loaded |= tr_resume::Downloaded;
    }

    if ((fields_to_load & tr_resume::Uploaded) != 0 && tr_variantDictFindInt(&top, TR_KEY_uploaded, &i))
    {
        tor->bytes_uploaded_.set_prev(i);
        fields_loaded |= tr_resume::Uploaded;
    }

    if ((fields_to_load & tr_resume::MaxPeers) != 0 && tr_variantDictFindInt(&top, TR_KEY_max_peers, &i))
    {
        tor->set_peer_limit(static_cast<uint16_t>(i));
        fields_loaded |= tr_resume::MaxPeers;
    }

    if (auto val = bool{}; (fields_to_load & tr_resume::Run) != 0 && tr_variantDictFindBool(&top, TR_KEY_paused, &val))
    {
        helper.load_start_when_stable(!val);
        fields_loaded |= tr_resume::Run;
    }

    if ((fields_to_load & tr_resume::AddedDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_added_date, &i))
    {
        helper.load_date_added(static_cast<time_t>(i));
        fields_loaded |= tr_resume::AddedDate;
    }

    if ((fields_to_load & tr_resume::DoneDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_done_date, &i))
    {
        helper.load_date_done(static_cast<time_t>(i));
        fields_loaded |= tr_resume::DoneDate;
    }

    if ((fields_to_load & tr_resume::ActivityDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_activity_date, &i))
    {
        tor->set_date_active(i);
        fields_loaded |= tr_resume::ActivityDate;
    }

    if ((fields_to_load & tr_resume::TimeSeeding) != 0 && tr_variantDictFindInt(&top, TR_KEY_seeding_time_seconds, &i))
    {
        helper.load_seconds_seeding_before_current_start(i);
        fields_loaded |= tr_resume::TimeSeeding;
    }

    if ((fields_to_load & tr_resume::TimeDownloading) != 0 && tr_variantDictFindInt(&top, TR_KEY_downloading_time_seconds, &i))
    {
        helper.load_seconds_downloading_before_current_start(i);
        fields_loaded |= tr_resume::TimeDownloading;
    }

    if ((fields_to_load & tr_resume::BandwidthPriority) != 0 && tr_variantDictFindInt(&top, TR_KEY_bandwidth_priority, &i) &&
        tr_isPriority(static_cast<tr_priority_t>(i)))
    {
        tr_torrentSetPriority(tor, static_cast<tr_priority_t>(i));
        fields_loaded |= tr_resume::BandwidthPriority;
    }

    if (auto val = bool{};
        (fields_to_load & tr_resume::SequentialDownload) != 0 && tr_variantDictFindBool(&top, TR_KEY_sequentialDownload, &val))
    {
        tor->set_sequential_download(val);
        fields_loaded |= tr_resume::SequentialDownload;
    }

    if ((fields_to_load & tr_resume::Peers) != 0)
    {
        fields_loaded |= loadPeers(&top, tor);
    }

    // Note: loadFilenames() must come before loadProgress()
    // so that loadProgress() -> tor->initCheckedPieces() -> tor->findFile()
    // will know where to look
    if ((fields_to_load & tr_resume::Filenames) != 0)
    {
        fields_loaded |= loadFilenames(&top, tor);
    }

    // Note: loadProgress should come before loadFilePriorities()
    // so that we can skip loading priorities iff the torrent is a
    // seed or a partial seed.
    if ((fields_to_load & tr_resume::Progress) != 0)
    {
        fields_loaded |= loadProgress(&top, tor, helper);
    }

    if (!tor->is_done() && (fields_to_load & tr_resume::FilePriorities) != 0)
    {
        fields_loaded |= loadFilePriorities(&top, tor);
    }

    if ((fields_to_load & tr_resume::Dnd) != 0)
    {
        fields_loaded |= loadDND(&top, tor);
    }

    if ((fields_to_load & tr_resume::Speedlimit) != 0)
    {
        fields_loaded |= loadSpeedLimits(&top, tor);
    }

    if ((fields_to_load & tr_resume::Ratiolimit) != 0)
    {
        fields_loaded |= loadRatioLimits(&top, tor);
    }

    if ((fields_to_load & tr_resume::Idlelimit) != 0)
    {
        fields_loaded |= loadIdleLimits(&top, tor);
    }

    if ((fields_to_load & tr_resume::Name) != 0)
    {
        fields_loaded |= loadName(&top, tor);
    }

    if ((fields_to_load & tr_resume::Labels) != 0)
    {
        fields_loaded |= loadLabels(&top, tor);
    }

    if ((fields_to_load & tr_resume::Group) != 0)
    {
        fields_loaded |= loadGroup(&top, tor);
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

    if ((fields & tr_resume::SequentialDownload) != 0)
    {
        if (auto const& val = ctor.sequential_download(mode); val)
        {
            tor->set_sequential_download(*val);
            ret |= tr_resume::SequentialDownload;
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

    auto top = tr_variant{};
    auto const now = tr_time();
    tr_variantInitDict(&top, 50); /* arbitrary "big enough" number */
    tr_variantDictAddInt(&top, TR_KEY_seeding_time_seconds, helper.seconds_seeding(now));
    tr_variantDictAddInt(&top, TR_KEY_downloading_time_seconds, helper.seconds_downloading(now));
    tr_variantDictAddInt(&top, TR_KEY_activity_date, helper.date_active());
    tr_variantDictAddInt(&top, TR_KEY_added_date, helper.date_added());
    tr_variantDictAddInt(&top, TR_KEY_corrupt, tor->bytes_corrupt_.ever());
    tr_variantDictAddInt(&top, TR_KEY_done_date, helper.date_done());
    tr_variantDictAddStrView(&top, TR_KEY_destination, tor->download_dir().sv());

    if (!std::empty(tor->incomplete_dir()))
    {
        tr_variantDictAddStrView(&top, TR_KEY_incomplete_dir, tor->incomplete_dir().sv());
    }

    tr_variantDictAddInt(&top, TR_KEY_downloaded, tor->bytes_downloaded_.ever());
    tr_variantDictAddInt(&top, TR_KEY_uploaded, tor->bytes_uploaded_.ever());
    tr_variantDictAddInt(&top, TR_KEY_max_peers, tor->peer_limit());
    tr_variantDictAddInt(&top, TR_KEY_bandwidth_priority, tor->get_priority());
    tr_variantDictAddBool(&top, TR_KEY_paused, !helper.start_when_stable());
    tr_variantDictAddBool(&top, TR_KEY_sequentialDownload, tor->is_sequential_download());
    savePeers(&top, tor);

    if (tor->has_metainfo())
    {
        saveFilePriorities(&top, tor);
        saveDND(&top, tor);
        saveProgress(&top, helper);
    }

    saveSpeedLimits(&top, tor);
    saveRatioLimits(&top, tor);
    saveIdleLimits(&top, tor);
    saveFilenames(&top, tor);
    saveName(&top, tor);
    saveLabels(&top, tor);
    saveGroup(&top, tor);

    auto serde = tr_variant_serde::benc();
    if (!serde.to_file(top, tor->resume_file()))
    {
        tor->error().set_local_error(fmt::format("Unable to save resume file: {:s}", serde.error_.message()));
    }
}

} // namespace tr_resume
