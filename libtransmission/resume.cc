// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cstring>
#include <ctime>
#include <string_view>
#include <vector>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/magnet-metainfo.h"
#include "libtransmission/peer-mgr.h" /* pex */
#include "libtransmission/resume.h"
#include "libtransmission/session.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"

using namespace std::literals;

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
    auto const& labels = tor->labels;
    tr_variant* list = tr_variantDictAddList(dict, TR_KEY_labels, std::size(labels));
    for (auto const& label : labels)
    {
        tr_variantListAddQuark(list, label);
    }
}

auto loadLabels(tr_variant* dict, tr_torrent* tor)
{
    tr_variant* list = nullptr;
    if (!tr_variantDictFindList(dict, TR_KEY_labels, &list))
    {
        return tr_resume::fields_t{};
    }

    auto const n = tr_variantListSize(list);
    auto labels = std::vector<tr_quark>{};
    labels.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        auto sv = std::string_view{};
        if (tr_variantGetStrView(tr_variantListChild(list, i), &sv) && !std::empty(sv))
        {
            labels.emplace_back(tr_quark_new(sv));
        }
    }

    tor->setLabels(labels);
    return tr_resume::Labels;
}

// ---

void saveGroup(tr_variant* dict, tr_torrent const* tor)
{
    tr_variantDictAddStrView(dict, TR_KEY_group, tor->bandwidth_group());
}

auto loadGroup(tr_variant* dict, tr_torrent* tor)
{
    if (std::string_view group_name; tr_variantDictFindStrView(dict, TR_KEY_group, &group_name) && !std::empty(group_name))
    {
        tor->set_bandwidth_group(group_name);
        return tr_resume::Group;
    }

    return tr_resume::fields_t{};
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

auto loadDND(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};
    tr_variant* list = nullptr;

    if (auto const n = tor->file_count(); tr_variantDictFindList(dict, TR_KEY_dnd, &list) && tr_variantListSize(list) == n)
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

        ret = tr_resume::Dnd;
    }
    else
    {
        tr_logAddDebugTor(
            tor,
            fmt::format(
                "Couldn't load DND flags. DND list {} has {} children; torrent has {} files",
                fmt::ptr(list),
                tr_variantListSize(list),
                n));
    }

    return ret;
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

auto loadFilePriorities(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

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

        ret = tr_resume::FilePriorities;
    }

    return ret;
}

// ---

void saveSingleSpeedLimit(tr_variant* d, tr_torrent const* tor, tr_direction dir)
{
    tr_variantDictReserve(d, 3);
    tr_variantDictAddInt(d, TR_KEY_speed_Bps, tor->speed_limit_bps(dir));
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
    tr_variantDictAddReal(d, TR_KEY_ratio_limit, tr_torrentGetRatioLimit(tor));
    tr_variantDictAddInt(d, TR_KEY_ratio_mode, tr_torrentGetRatioMode(tor));
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
        tor->set_speed_limit_bps(dir, val);
    }
    else if (tr_variantDictFindInt(d, TR_KEY_speed, &val))
    {
        tor->set_speed_limit_bps(dir, val * 1024);
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

auto loadRatioLimits(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    if (tr_variant* d = nullptr; tr_variantDictFindDict(dict, TR_KEY_ratio_limit, &d))
    {
        if (auto dratio = double{}; tr_variantDictFindReal(d, TR_KEY_ratio_limit, &dratio))
        {
            tr_torrentSetRatioLimit(tor, dratio);
        }

        if (auto i = int64_t{}; tr_variantDictFindInt(d, TR_KEY_ratio_mode, &i))
        {
            tor->set_ratio_mode(tr_ratiolimit(i));
        }

        ret = tr_resume::Ratiolimit;
    }

    return ret;
}

auto loadIdleLimits(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    if (tr_variant* d = nullptr; tr_variantDictFindDict(dict, TR_KEY_idle_limit, &d))
    {
        if (auto imin = int64_t{}; tr_variantDictFindInt(d, TR_KEY_idle_limit, &imin))
        {
            tor->set_idle_limit(imin);
        }

        if (auto i = int64_t{}; tr_variantDictFindInt(d, TR_KEY_idle_mode, &i))
        {
            tr_torrentSetIdleMode(tor, tr_idlelimit(i));
        }

        ret = tr_resume::Idlelimit;
    }

    return ret;
}

// ---

void saveName(tr_variant* dict, tr_torrent const* tor)
{
    tr_variantDictAddStrView(dict, TR_KEY_name, tr_torrentName(tor));
}

auto loadName(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    auto name = std::string_view{};
    if (!tr_variantDictFindStrView(dict, TR_KEY_name, &name))
    {
        return ret;
    }

    name = tr_strv_strip(name);
    if (std::empty(name))
    {
        return ret;
    }

    tor->set_name(name);
    ret |= tr_resume::Name;

    return ret;
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

auto loadFilenames(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    tr_variant* list = nullptr;
    if (!tr_variantDictFindList(dict, TR_KEY_files, &list))
    {
        return ret;
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

    ret |= tr_resume::Filenames;
    return ret;
}

// ---

void bitfieldToRaw(tr_bitfield const& b, tr_variant* benc)
{
    if (b.has_none() || std::empty(b))
    {
        tr_variantInitStr(benc, "none"sv);
    }
    else if (b.has_all())
    {
        tr_variantInitStrView(benc, "all"sv);
    }
    else
    {
        auto const raw = b.raw();
        tr_variantInitRaw(benc, raw.data(), std::size(raw));
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

void saveProgress(tr_variant* dict, tr_torrent const* tor)
{
    tr_variant* const prog = tr_variantDictAddDict(dict, TR_KEY_progress, 4);

    // add the mtimes
    auto const& mtimes = tor->file_mtimes_;
    auto const n = std::size(mtimes);
    tr_variant* const l = tr_variantDictAddList(prog, TR_KEY_mtimes, n);
    for (auto const& mtime : mtimes)
    {
        tr_variantListAddInt(l, mtime);
    }

    // add the 'checked pieces' bitfield
    bitfieldToRaw(tor->checked_pieces_, tr_variantDictAdd(prog, TR_KEY_pieces));

    /* add the progress */
    if (tor->completeness == TR_SEED)
    {
        tr_variantDictAddStrView(prog, TR_KEY_have, "all"sv);
    }

    /* add the blocks bitfield */
    bitfieldToRaw(tor->blocks(), tr_variantDictAdd(prog, TR_KEY_blocks));
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
 * 'time_checked' entry which was a list with fileCount items.
 * Each item was either a list of per-piece timestamps, or a
 * single timestamp if either all or none of the pieces had been
 * tested more recently than the file's mtime.
 *
 * First approach (pre-2.20) had an "mtimes" list identical to
 * 3.10, but not the 'pieces' bitfield.
 */
auto loadProgress(tr_variant* dict, tr_torrent* tor)
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
                    auto offset = int64_t{};
                    tr_variantGetInt(tr_variantListChild(b, 0), &offset);

                    time_checked = tr_time();
                    auto const [begin, end] = tor->pieces_in_file(fi);
                    for (size_t i = 0, n = end - begin; i < n; ++i)
                    {
                        int64_t piece_time = 0;
                        tr_variantGetInt(tr_variantListChild(b, i + 1), &piece_time);
                        time_checked = std::min(time_checked, time_t(piece_time));
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

        tor->init_checked_pieces(checked, std::data(mtimes));

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
        else if (auto sv = std::string_view{}; tr_variantDictFindStrView(prog, TR_KEY_have, &sv))
        {
            if (sv == "all"sv)
            {
                blocks.set_has_all();
            }
            else
            {
                err = "Invalid value for HAVE";
            }
        }
        else if (tr_variantDictFindRaw(prog, TR_KEY_bitfield, &raw, &rawlen))
        {
            blocks.set_raw(raw, rawlen);
        }
        else
        {
            err = "Couldn't find 'pieces' or 'have' or 'bitfield'";
        }

        if (err != nullptr)
        {
            tr_logAddDebugTor(tor, fmt::format("Torrent needs to be verified - {}", err));
        }
        else
        {
            tor->set_blocks(blocks);
        }

        return tr_resume::Progress;
    }

    return tr_resume::fields_t{};
}

// ---

tr_resume::fields_t loadFromFile(tr_torrent* tor, tr_resume::fields_t fields_to_load)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const was_dirty = tor->is_dirty();

    tr_torrent_metainfo::migrate_file(tor->session->resumeDir(), tor->name(), tor->info_hash_string(), ".resume"sv);

    auto const filename = tor->resume_file();
    if (!tr_sys_path_exists(filename))
    {
        return {};
    }

    auto serde = tr_variant_serde::benc();
    auto otop = serde.parse_file(filename);
    if (!otop)
    {
        tr_logAddDebugTor(tor, fmt::format("Couldn't read '{}': {}", filename, serde.error_->message));
        return {};
    }
    auto& top = *otop;

    tr_logAddDebugTor(tor, fmt::format("Read resume file '{}'", filename));
    auto fields_loaded = tr_resume::fields_t{};
    auto i = int64_t{};
    auto sv = std::string_view{};

    if ((fields_to_load & tr_resume::Corrupt) != 0 && tr_variantDictFindInt(&top, TR_KEY_corrupt, &i))
    {
        tor->corruptPrev = i;
        fields_loaded |= tr_resume::Corrupt;
    }

    if ((fields_to_load & (tr_resume::Progress | tr_resume::DownloadDir)) != 0 &&
        tr_variantDictFindStrView(&top, TR_KEY_destination, &sv) && !std::empty(sv))
    {
        bool const is_current_dir = tor->current_dir() == tor->download_dir();
        tor->download_dir_ = sv;
        if (is_current_dir)
        {
            tor->current_dir_ = sv;
        }

        fields_loaded |= tr_resume::DownloadDir;
    }

    if ((fields_to_load & (tr_resume::Progress | tr_resume::IncompleteDir)) != 0 &&
        tr_variantDictFindStrView(&top, TR_KEY_incomplete_dir, &sv) && !std::empty(sv))
    {
        bool const is_current_dir = tor->current_dir() == tor->incomplete_dir();
        tor->incomplete_dir_ = sv;
        if (is_current_dir)
        {
            tor->current_dir_ = sv;
        }

        fields_loaded |= tr_resume::IncompleteDir;
    }

    if ((fields_to_load & tr_resume::Downloaded) != 0 && tr_variantDictFindInt(&top, TR_KEY_downloaded, &i))
    {
        tor->downloadedPrev = i;
        fields_loaded |= tr_resume::Downloaded;
    }

    if ((fields_to_load & tr_resume::Uploaded) != 0 && tr_variantDictFindInt(&top, TR_KEY_uploaded, &i))
    {
        tor->uploadedPrev = i;
        fields_loaded |= tr_resume::Uploaded;
    }

    if ((fields_to_load & tr_resume::MaxPeers) != 0 && tr_variantDictFindInt(&top, TR_KEY_max_peers, &i))
    {
        tor->max_connected_peers_ = static_cast<uint16_t>(i);
        fields_loaded |= tr_resume::MaxPeers;
    }

    if (auto val = bool{}; (fields_to_load & tr_resume::Run) != 0 && tr_variantDictFindBool(&top, TR_KEY_paused, &val))
    {
        tor->start_when_stable = !val;
        fields_loaded |= tr_resume::Run;
    }

    if ((fields_to_load & tr_resume::AddedDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_added_date, &i))
    {
        tor->addedDate = i;
        fields_loaded |= tr_resume::AddedDate;
    }

    if ((fields_to_load & tr_resume::DoneDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_done_date, &i))
    {
        tor->doneDate = i;
        fields_loaded |= tr_resume::DoneDate;
    }

    if ((fields_to_load & tr_resume::ActivityDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_activity_date, &i))
    {
        tor->set_date_active(i);
        fields_loaded |= tr_resume::ActivityDate;
    }

    if ((fields_to_load & tr_resume::TimeSeeding) != 0 && tr_variantDictFindInt(&top, TR_KEY_seeding_time_seconds, &i))
    {
        tor->seconds_seeding_before_current_start_ = i;
        fields_loaded |= tr_resume::TimeSeeding;
    }

    if ((fields_to_load & tr_resume::TimeDownloading) != 0 && tr_variantDictFindInt(&top, TR_KEY_downloading_time_seconds, &i))
    {
        tor->seconds_downloading_before_current_start_ = i;
        fields_loaded |= tr_resume::TimeDownloading;
    }

    if ((fields_to_load & tr_resume::BandwidthPriority) != 0 && tr_variantDictFindInt(&top, TR_KEY_bandwidth_priority, &i) &&
        tr_isPriority(i))
    {
        tr_torrentSetPriority(tor, i);
        fields_loaded |= tr_resume::BandwidthPriority;
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
        fields_loaded |= loadProgress(&top, tor);
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

    /* loading the resume file triggers of a lot of changes,
     * but none of them needs to trigger a re-saving of the
     * same resume information... */
    tor->set_dirty(was_dirty);

    return fields_loaded;
}

auto setFromCtor(tr_torrent* tor, tr_resume::fields_t fields, tr_ctor const* ctor, tr_ctorMode mode)
{
    auto ret = tr_resume::fields_t{};

    if ((fields & tr_resume::Run) != 0)
    {
        if (auto is_paused = bool{}; tr_ctorGetPaused(ctor, mode, &is_paused))
        {
            tor->start_when_stable = !is_paused;
            ret |= tr_resume::Run;
        }
    }

    if (((fields & tr_resume::MaxPeers) != 0) && tr_ctorGetPeerLimit(ctor, mode, &tor->max_connected_peers_))
    {
        ret |= tr_resume::MaxPeers;
    }

    if ((fields & tr_resume::DownloadDir) != 0)
    {
        char const* path = nullptr;
        if (tr_ctorGetDownloadDir(ctor, mode, &path) && !tr_str_is_empty(path))
        {
            ret |= tr_resume::DownloadDir;
            tor->download_dir_ = path;
        }
    }

    return ret;
}

auto useMandatoryFields(tr_torrent* tor, tr_resume::fields_t fields, tr_ctor const* ctor)
{
    return setFromCtor(tor, fields, ctor, TR_FORCE);
}

auto useFallbackFields(tr_torrent* tor, tr_resume::fields_t fields, tr_ctor const* ctor)
{
    return setFromCtor(tor, fields, ctor, TR_FALLBACK);
}
} // namespace

fields_t load(tr_torrent* tor, fields_t fields_to_load, tr_ctor const* ctor)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto ret = fields_t{};

    ret |= useMandatoryFields(tor, fields_to_load, ctor);
    fields_to_load &= ~ret;
    ret |= loadFromFile(tor, fields_to_load);
    fields_to_load &= ~ret;
    ret |= useFallbackFields(tor, fields_to_load, ctor);

    return ret;
}

void save(tr_torrent* tor)
{
    if (!tr_isTorrent(tor))
    {
        return;
    }

    auto const now = tr_time();
    auto top = tr_variant::make_map(50U); // arbitrary "big enough" number
    tr_variantDictAddInt(&top, TR_KEY_seeding_time_seconds, tor->seconds_seeding(now));
    tr_variantDictAddInt(&top, TR_KEY_downloading_time_seconds, tor->seconds_downloading(now));
    tr_variantDictAddInt(&top, TR_KEY_activity_date, tor->activityDate);
    tr_variantDictAddInt(&top, TR_KEY_added_date, tor->addedDate);
    tr_variantDictAddInt(&top, TR_KEY_corrupt, tor->corruptPrev + tor->corruptCur);
    tr_variantDictAddInt(&top, TR_KEY_done_date, tor->doneDate);
    tr_variantDictAddQuark(&top, TR_KEY_destination, tor->download_dir().quark());

    if (!std::empty(tor->incomplete_dir()))
    {
        tr_variantDictAddQuark(&top, TR_KEY_incomplete_dir, tor->incomplete_dir().quark());
    }

    tr_variantDictAddInt(&top, TR_KEY_downloaded, tor->downloadedPrev + tor->downloadedCur);
    tr_variantDictAddInt(&top, TR_KEY_uploaded, tor->uploadedPrev + tor->uploadedCur);
    tr_variantDictAddInt(&top, TR_KEY_max_peers, tor->peer_limit());
    tr_variantDictAddInt(&top, TR_KEY_bandwidth_priority, tor->get_priority());
    tr_variantDictAddBool(&top, TR_KEY_paused, !tor->start_when_stable);
    tr_variantDictAddBool(&top, TR_KEY_sequentialDownload, tor->is_sequential_download());
    savePeers(&top, tor);

    if (tor->has_metainfo())
    {
        saveFilePriorities(&top, tor);
        saveDND(&top, tor);
        saveProgress(&top, tor);
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
        tor->set_local_error(fmt::format("Unable to save resume file: {:s}", serde.error_->message));
    }
}

} // namespace tr_resume
