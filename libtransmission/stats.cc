// This file Copyright © 2007-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

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
    auto ret = tr_session_stats{};

    if (auto stats = load_stats(config_dir); stats)
    {
        auto const key_tgts = std::array<std::pair<tr_quark, uint64_t*>, 5>{
            { { TR_KEY_downloaded_bytes, &ret.downloadedBytes },
              { TR_KEY_files_added, &ret.filesAdded },
              { TR_KEY_seconds_active, &ret.secondsActive },
              { TR_KEY_session_count, &ret.sessionCount },
              { TR_KEY_uploaded_bytes, &ret.uploadedBytes } }
        };

        for (auto& [key, tgt] : key_tgts)
        {
            if (auto val = int64_t{}; tr_variantDictFindInt(&*stats, key, &val))
            {
                *tgt = val;
            }
        }
    }

    return ret;
}

void tr_stats::save() const
{
    auto const saveme = cumulative();
    auto top = tr_variant::make_map(5U);
    tr_variantDictAddInt(&top, TR_KEY_downloaded_bytes, saveme.downloadedBytes);
    tr_variantDictAddInt(&top, TR_KEY_files_added, saveme.filesAdded);
    tr_variantDictAddInt(&top, TR_KEY_seconds_active, saveme.secondsActive);
    tr_variantDictAddInt(&top, TR_KEY_session_count, saveme.sessionCount);
    tr_variantDictAddInt(&top, TR_KEY_uploaded_bytes, saveme.uploadedBytes);
    tr_variant_serde::json().to_file(top, tr_pathbuf{ config_dir_, "/stats.json"sv });
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
