// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cstddef> // size_t
#include <cstdint> // int64_t
#include <future>
#include <iterator> // std::inserter
#include <set>
#include <string_view>
#include <vector>

#include <libtransmission/transmission.h>
#include <libtransmission/rpcimpl.h>
#include <libtransmission/variant.h>

#include "gtest/gtest.h"
#include "libtransmission/quark.h"
#include "test-fixtures.h"

struct tr_session;

using namespace std::literals;

namespace libtransmission::test
{

using RpcTest = SessionTest;

TEST_F(RpcTest, list)
{
    auto i = int64_t{};
    auto sv = std::string_view{};

    auto top = tr_rpc_parse_list_str("12"sv);
    EXPECT_TRUE(top.holds_alternative<int64_t>());
    EXPECT_TRUE(tr_variantGetInt(&top, &i));
    EXPECT_EQ(12, i);

    top = tr_rpc_parse_list_str("6,7"sv);
    EXPECT_TRUE(top.holds_alternative<tr_variant::Vector>());
    EXPECT_EQ(2U, tr_variantListSize(&top));
    EXPECT_TRUE(tr_variantGetInt(tr_variantListChild(&top, 0), &i));
    EXPECT_EQ(6, i);
    EXPECT_TRUE(tr_variantGetInt(tr_variantListChild(&top, 1), &i));
    EXPECT_EQ(7, i);

    top = tr_rpc_parse_list_str("asdf"sv);
    EXPECT_TRUE(top.holds_alternative<std::string_view>());
    EXPECT_TRUE(tr_variantGetStrView(&top, &sv));
    EXPECT_EQ("asdf"sv, sv);

    top = tr_rpc_parse_list_str("1,3-5"sv);
    EXPECT_TRUE(top.holds_alternative<tr_variant::Vector>());
    EXPECT_EQ(4U, tr_variantListSize(&top));
    EXPECT_TRUE(tr_variantGetInt(tr_variantListChild(&top, 0), &i));
    EXPECT_EQ(1, i);
    EXPECT_TRUE(tr_variantGetInt(tr_variantListChild(&top, 1), &i));
    EXPECT_EQ(3, i);
    EXPECT_TRUE(tr_variantGetInt(tr_variantListChild(&top, 2), &i));
    EXPECT_EQ(4, i);
    EXPECT_TRUE(tr_variantGetInt(tr_variantListChild(&top, 3), &i));
    EXPECT_EQ(5, i);
}

TEST_F(RpcTest, tagSync)
{
    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_method, "session-stats");
    request_map.try_emplace(TR_KEY_tag, 12345);

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        tr_variant{ std::move(request_map) },
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const result = response_map->value_if<std::string_view>(TR_KEY_result);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "success"sv);
    auto const tag = response_map->value_if<int64_t>(TR_KEY_tag);
    ASSERT_TRUE(tag);
    EXPECT_EQ(*tag, 12345);
}

TEST_F(RpcTest, tagAsync)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::Complete);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_method, "torrent_rename_path");
    request_map.try_emplace(TR_KEY_tag, 12345);

    auto arguments_map = tr_variant::Map{ 2U };
    arguments_map.try_emplace(TR_KEY_path, "files-filled-with-zeroes/512");
    arguments_map.try_emplace(TR_KEY_name, "512_test");
    request_map.try_emplace(TR_KEY_arguments, std::move(arguments_map));

    auto promise = std::promise<tr_variant>{};
    auto future = promise.get_future();
    tr_rpc_request_exec(
        session_,
        tr_variant{ std::move(request_map) },
        [&promise](tr_session* /*session*/, tr_variant&& resp) { promise.set_value(std::move(resp)); });
    auto const response = future.get();

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const result = response_map->value_if<std::string_view>(TR_KEY_result);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "success"sv);
    auto const tag = response_map->value_if<int64_t>(TR_KEY_tag);
    ASSERT_TRUE(tag);
    EXPECT_EQ(*tag, 12345);

    // cleanup
    tr_torrentRemove(tor, false, nullptr, nullptr, nullptr, nullptr);
}

TEST_F(RpcTest, tagNoHandler)
{
    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_method, "sdgdhsgg");
    request_map.try_emplace(TR_KEY_tag, 12345);

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        tr_variant{ std::move(request_map) },
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const result = response_map->value_if<std::string_view>(TR_KEY_result);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "no method name"sv);
    auto const tag = response_map->value_if<int64_t>(TR_KEY_tag);
    ASSERT_TRUE(tag);
    EXPECT_EQ(*tag, 12345);
}

/***
****
***/

TEST_F(RpcTest, sessionGet)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    EXPECT_NE(nullptr, tor);

    auto request = tr_variant{};
    tr_variantInitDict(&request, 1);
    tr_variantDictAddStrView(&request, TR_KEY_method, "session-get");
    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        request,
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    EXPECT_TRUE(response.holds_alternative<tr_variant::Map>());
    tr_variant* args = nullptr;
    EXPECT_TRUE(tr_variantDictFindDict(&response, TR_KEY_arguments, &args));

    // what we expected
    static auto constexpr ExpectedKeys = std::array{
        TR_KEY_alt_speed_down_kebab,
        TR_KEY_alt_speed_enabled_kebab,
        TR_KEY_alt_speed_time_begin_kebab,
        TR_KEY_alt_speed_time_day_kebab,
        TR_KEY_alt_speed_time_enabled_kebab,
        TR_KEY_alt_speed_time_end_kebab,
        TR_KEY_alt_speed_up_kebab,
        TR_KEY_alt_speed_down,
        TR_KEY_alt_speed_enabled,
        TR_KEY_alt_speed_time_begin,
        TR_KEY_alt_speed_time_day,
        TR_KEY_alt_speed_time_enabled,
        TR_KEY_alt_speed_time_end,
        TR_KEY_alt_speed_up,
        TR_KEY_anti_brute_force_enabled,
        TR_KEY_anti_brute_force_threshold,
        TR_KEY_blocklist_enabled_kebab,
        TR_KEY_blocklist_size_kebab,
        TR_KEY_blocklist_url_kebab,
        TR_KEY_blocklist_enabled,
        TR_KEY_blocklist_size,
        TR_KEY_blocklist_url,
        TR_KEY_cache_size_mb_kebab,
        TR_KEY_cache_size_mb,
        TR_KEY_config_dir_kebab,
        TR_KEY_config_dir,
        TR_KEY_default_trackers_kebab,
        TR_KEY_default_trackers,
        TR_KEY_dht_enabled_kebab,
        TR_KEY_dht_enabled,
        TR_KEY_download_dir_kebab,
        TR_KEY_download_dir_free_space_kebab,
        TR_KEY_download_queue_enabled_kebab,
        TR_KEY_download_queue_size_kebab,
        TR_KEY_download_dir,
        TR_KEY_download_dir_free_space,
        TR_KEY_download_queue_enabled,
        TR_KEY_download_queue_size,
        TR_KEY_encryption,
        TR_KEY_idle_seeding_limit_kebab,
        TR_KEY_idle_seeding_limit_enabled_kebab,
        TR_KEY_idle_seeding_limit,
        TR_KEY_idle_seeding_limit_enabled,
        TR_KEY_incomplete_dir_kebab,
        TR_KEY_incomplete_dir_enabled_kebab,
        TR_KEY_incomplete_dir,
        TR_KEY_incomplete_dir_enabled,
        TR_KEY_lpd_enabled_kebab,
        TR_KEY_lpd_enabled,
        TR_KEY_peer_limit_global_kebab,
        TR_KEY_peer_limit_per_torrent_kebab,
        TR_KEY_peer_port_kebab,
        TR_KEY_peer_port_random_on_start_kebab,
        TR_KEY_peer_limit_global,
        TR_KEY_peer_limit_per_torrent,
        TR_KEY_peer_port,
        TR_KEY_peer_port_random_on_start,
        TR_KEY_pex_enabled_kebab,
        TR_KEY_pex_enabled,
        TR_KEY_port_forwarding_enabled_kebab,
        TR_KEY_port_forwarding_enabled,
        TR_KEY_queue_stalled_enabled,
        TR_KEY_queue_stalled_minutes,
        TR_KEY_rename_partial_files,
        TR_KEY_reqq,
        TR_KEY_rpc_version,
        TR_KEY_rpc_version_minimum,
        TR_KEY_rpc_version_semver,
        TR_KEY_script_torrent_added_enabled,
        TR_KEY_script_torrent_added_filename,
        TR_KEY_script_torrent_done_enabled,
        TR_KEY_script_torrent_done_filename,
        TR_KEY_script_torrent_done_seeding_enabled,
        TR_KEY_script_torrent_done_seeding_filename,
        TR_KEY_seed_ratio_limit_camel,
        TR_KEY_seed_queue_enabled,
        TR_KEY_seed_queue_size,
        TR_KEY_seed_ratio_limit,
        TR_KEY_seedRatioLimited,
        TR_KEY_session_id,
        TR_KEY_speed_limit_down,
        TR_KEY_speed_limit_down_enabled,
        TR_KEY_speed_limit_up,
        TR_KEY_speed_limit_up_enabled,
        TR_KEY_start_added_torrents,
        TR_KEY_tcp_enabled,
        TR_KEY_trash_original_torrent_files,
        TR_KEY_units,
        TR_KEY_utp_enabled,
        TR_KEY_version,
    };

    // what we got
    std::set<tr_quark> actual_keys;
    auto key = tr_quark{};
    tr_variant* val = nullptr;
    auto n = size_t{};
    while ((tr_variantDictChild(args, n++, &key, &val)))
    {
        actual_keys.insert(key);
    }

    auto missing_keys = std::vector<tr_quark>{};
    std::set_difference(
        std::begin(ExpectedKeys),
        std::end(ExpectedKeys),
        std::begin(actual_keys),
        std::end(actual_keys),
        std::inserter(missing_keys, std::begin(missing_keys)));
    EXPECT_EQ(decltype(missing_keys){}, missing_keys);

    auto unexpected_keys = std::vector<tr_quark>{};
    std::set_difference(
        std::begin(actual_keys),
        std::end(actual_keys),
        std::begin(ExpectedKeys),
        std::end(ExpectedKeys),
        std::inserter(unexpected_keys, std::begin(unexpected_keys)));
    EXPECT_EQ(decltype(unexpected_keys){}, unexpected_keys);

    // cleanup
    tr_torrentRemove(tor, false, nullptr, nullptr, nullptr, nullptr);
}

TEST_F(RpcTest, torrentGet)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    EXPECT_NE(nullptr, tor);

    tr_variant request;
    tr_variantInitDict(&request, 1);

    tr_variantDictAddStrView(&request, TR_KEY_method, "torrent_get");

    tr_variant* args_in = tr_variantDictAddDict(&request, TR_KEY_arguments, 1);
    tr_variant* fields = tr_variantDictAddList(args_in, TR_KEY_fields, 1);
    tr_variantListAddStrView(fields, tr_quark_get_string_view(TR_KEY_id));

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        request,
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    EXPECT_TRUE(response.holds_alternative<tr_variant::Map>());
    tr_variant* args = nullptr;
    EXPECT_TRUE(tr_variantDictFindDict(&response, TR_KEY_arguments, &args));

    tr_variant* torrents = nullptr;
    EXPECT_TRUE(tr_variantDictFindList(args, TR_KEY_torrents, &torrents));
    EXPECT_EQ(1UL, tr_variantListSize(torrents));

    tr_variant* first_torrent = tr_variantListChild(torrents, 0);
    EXPECT_TRUE(first_torrent != nullptr);
    EXPECT_TRUE(first_torrent->holds_alternative<tr_variant::Map>());
    int64_t first_torrent_id = 0;
    EXPECT_TRUE(tr_variantDictFindInt(first_torrent, TR_KEY_id, &first_torrent_id));
    EXPECT_EQ(1, first_torrent_id);

    // cleanup
    tr_torrentRemove(tor, false, nullptr, nullptr, nullptr, nullptr);
}

} // namespace libtransmission::test
