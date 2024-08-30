// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <initializer_list>
#include <optional>
#include <utility>

#include "libtransmission/transmission.h"

#include "libtransmission/file.h"
#include "libtransmission/quark.h"
#include "libtransmission/stats.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h" // for tr_getRatio(), tr_time()
#include "libtransmission/variant.h"

using namespace std::literals;

namespace
{
std::optional<tr_variant> load_stats(std::string_view config_dir)
{
    if (auto filename = tr_pathbuf{ config_dir, "/stats.json"sv }; tr_sys_path_exists(filename))
    {
        return tr_variant_serde::json().parse_file(filename);
    }

    // maybe the user just upgraded from an old version of Transmission
    // that was still using stats.benc
    if (auto filename = tr_pathbuf{ config_dir, "/stats.benc"sv }; tr_sys_path_exists(filename))
    {
        return tr_variant_serde::benc().parse_file(filename);
    }

    return {};
}
} // namespace

tr_session_stats tr_stats::load_old_stats(std::string_view config_dir)
{
    auto const stats = load_stats(config_dir);
    if (!stats)
    {
        return {};
    }

    auto const* const map = stats->get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return {};
    }

    auto const load = [map](std::initializer_list<tr_quark> keys, uint64_t& dst)
    {
        if (auto const val = map->value_if<int64_t>(keys); val)
        {
            dst = *val;
        }
    };

    auto ret = tr_session_stats{};

    load({ TR_KEY_downloaded_bytes }, ret.downloadedBytes);
    load({ TR_KEY_files_added }, ret.filesAdded);
    load({ TR_KEY_seconds_active }, ret.secondsActive);
    load({ TR_KEY_session_count }, ret.sessionCount);
    load({ TR_KEY_uploaded_bytes }, ret.uploadedBytes);

    return ret;
}

void tr_stats::save() const
{
    auto const saveme = cumulative();
    auto map = tr_variant::Map{ 5 };
    map.try_emplace(TR_KEY_downloaded_bytes, saveme.downloadedBytes);
    map.try_emplace(TR_KEY_files_added, saveme.filesAdded);
    map.try_emplace(TR_KEY_seconds_active, saveme.secondsActive);
    map.try_emplace(TR_KEY_session_count, saveme.sessionCount);
    map.try_emplace(TR_KEY_uploaded_bytes, saveme.uploadedBytes);
    tr_variant_serde::json().to_file(tr_variant{ std::move(map) }, tr_pathbuf{ config_dir_, "/stats.json"sv });
}

void tr_stats::clear()
{
    single_ = old_ = Zero;
    start_time_ = tr_time();
}

[[nodiscard]] tr_session_stats tr_stats::current() const
{
    auto ret = single_;
    ret.secondsActive = time(nullptr) - start_time_;
    ret.ratio = tr_getRatio(ret.uploadedBytes, ret.downloadedBytes);
    return ret;
}

tr_session_stats tr_stats::add(tr_session_stats const& a, tr_session_stats const& b)
{
    auto ret = tr_session_stats{};
    ret.uploadedBytes = a.uploadedBytes + b.uploadedBytes;
    ret.downloadedBytes = a.downloadedBytes + b.downloadedBytes;
    ret.filesAdded = a.filesAdded + b.filesAdded;
    ret.sessionCount = a.sessionCount + b.sessionCount;
    ret.secondsActive = a.secondsActive + b.secondsActive;
    ret.ratio = tr_getRatio(ret.uploadedBytes, ret.downloadedBytes);
    return ret;
}
