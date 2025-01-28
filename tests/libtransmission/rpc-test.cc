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
    auto top = tr_rpc_parse_list_str("12"sv);
    auto i = top.value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(12, *i);

    top = tr_rpc_parse_list_str("6,7"sv);
    auto* v = top.get_if<tr_variant::Vector>();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(2U, std::size(*v));
    i = (*v)[0].value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(6, *i);
    i = (*v)[1].value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(7, *i);

    top = tr_rpc_parse_list_str("asdf"sv);
    auto sv = top.value_if<std::string_view>();
    ASSERT_TRUE(sv);
    EXPECT_EQ("asdf"sv, *sv);

    top = tr_rpc_parse_list_str("1,3-5"sv);
    v = top.get_if<tr_variant::Vector>();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(4U, std::size(*v));
    i = (*v)[0].value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(1, *i);
    i = (*v)[1].value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(3, *i);
    i = (*v)[2].value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(4, *i);
    i = (*v)[3].value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(5, *i);
}

TEST_F(RpcTest, EmptyRequest)
{
    static auto constexpr Request = ""sv;

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        Request,
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const* const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    EXPECT_EQ(result, nullptr);
    auto const* const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto const error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32700); // don't use constants here in case they are wrong
    auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Parse error"sv);
    auto const id = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id);
}

TEST_F(RpcTest, NotArrayOrObject)
{
    auto requests = std::vector<tr_variant>{};
    requests.emplace_back(12345);
    requests.emplace_back(0.5);
    requests.emplace_back("12345"sv);
    requests.emplace_back(nullptr);
    requests.emplace_back(true);

    for (auto const& req : requests)
    {
        auto response = tr_variant{};
        tr_rpc_request_exec(
            session_,
            req,
            [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

        auto const* const response_map = response.get_if<tr_variant::Map>();
        ASSERT_NE(response_map, nullptr);
        auto const result = response_map->find(TR_KEY_result);
        EXPECT_EQ(result, std::end(*response_map));
        auto const* const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
        ASSERT_NE(error, nullptr);
        auto const error_code = error->value_if<int64_t>(TR_KEY_code);
        ASSERT_TRUE(error_code);
        EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
        auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
        ASSERT_TRUE(error_message);
        EXPECT_EQ(*error_message, "Invalid Request"sv);
        auto const error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
        ASSERT_NE(error_data, nullptr);
        auto const error_string = error_data->value_if<std::string_view>(TR_KEY_errorString);
        ASSERT_TRUE(error_string);
        EXPECT_EQ(*error_string, "request must be an Array or Object"sv);
        auto const id = response_map->value_if<std::nullptr_t>(TR_KEY_id);
        EXPECT_TRUE(id);
    }
}

TEST_F(RpcTest, JsonRpcWrongVersion)
{
    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, "1.0");
    request_map.try_emplace(TR_KEY_method, "session_stats");
    request_map.try_emplace(TR_KEY_id, 12345);

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        std::move(request_map),
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const result = response_map->find(TR_KEY_result);
    EXPECT_EQ(result, std::end(*response_map));
    auto const* const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto const error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
    auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Invalid Request"sv);
    auto const error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
    ASSERT_NE(error_data, nullptr);
    auto const error_string = error_data->value_if<std::string_view>(TR_KEY_errorString);
    ASSERT_TRUE(error_string);
    EXPECT_EQ(*error_string, "JSON-RPC version is not 2.0"sv);
    auto const id = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id);
}

TEST_F(RpcTest, idSync)
{
    auto ids = std::vector<tr_variant>{};
    ids.emplace_back(12345);
    ids.emplace_back(0.5);
    ids.emplace_back("12345"sv);
    ids.emplace_back(nullptr);

    for (auto const& request_id : ids)
    {
        auto request_map = tr_variant::Map{ 3U };
        request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
        request_map.try_emplace(TR_KEY_method, "session-stats");
        request_map[TR_KEY_id].merge(request_id); // copy

        auto response = tr_variant{};
        tr_rpc_request_exec(
            session_,
            std::move(request_map),
            [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

        auto const* const response_map = response.get_if<tr_variant::Map>();
        ASSERT_NE(response_map, nullptr);
        auto const* const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
        EXPECT_NE(result, nullptr);
        auto const error = response_map->find(TR_KEY_error);
        EXPECT_EQ(error, std::end(*response_map));
        switch (request_id.index())
        {
        case tr_variant::IntIndex:
            EXPECT_EQ(request_id.value_if<int64_t>(), response_map->value_if<int64_t>(TR_KEY_id));
            break;
        case tr_variant::DoubleIndex:
            EXPECT_EQ(request_id.value_if<double>(), response_map->value_if<double>(TR_KEY_id));
            break;
        case tr_variant::StringIndex:
            EXPECT_EQ(request_id.value_if<std::string_view>(), response_map->value_if<std::string_view>(TR_KEY_id));
            break;
        case tr_variant::NullIndex:
            EXPECT_EQ(request_id.value_if<std::nullptr_t>(), response_map->value_if<std::nullptr_t>(TR_KEY_id));
            break;
        default:
            break;
        }
    }
}

TEST_F(RpcTest, idWrongType)
{
    auto ids = std::vector<tr_variant>{};
    ids.emplace_back(tr_variant::Map{});
    ids.emplace_back(tr_variant::Vector{});
    ids.emplace_back(true);

    for (auto const& request_id : ids)
    {
        auto request_map = tr_variant::Map{ 3U };
        request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
        request_map.try_emplace(TR_KEY_method, "session_stats");
        request_map[TR_KEY_id].merge(request_id); // copy

        auto response = tr_variant{};
        tr_rpc_request_exec(
            session_,
            std::move(request_map),
            [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

        auto const* const response_map = response.get_if<tr_variant::Map>();
        ASSERT_NE(response_map, nullptr);
        auto const result = response_map->find(TR_KEY_result);
        EXPECT_EQ(result, std::end(*response_map));
        auto const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
        ASSERT_NE(error, nullptr);
        auto const error_code = error->value_if<int64_t>(TR_KEY_code);
        ASSERT_TRUE(error_code);
        EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
        auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
        ASSERT_TRUE(error_message);
        EXPECT_EQ(*error_message, "Invalid Request"sv);
        auto const error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
        ASSERT_NE(error_data, nullptr);
        auto const error_string = error_data->value_if<std::string_view>(TR_KEY_errorString);
        ASSERT_TRUE(error_string);
        EXPECT_EQ(*error_string, "id type must be String, Number, or Null"sv);
        auto const id = response_map->value_if<std::nullptr_t>(TR_KEY_id);
        EXPECT_TRUE(id);
    }
}

TEST_F(RpcTest, tagSyncLegacy)
{
    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_method, "session-stats");
    request_map.try_emplace(TR_KEY_tag, 12345);

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        std::move(request_map),
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

TEST_F(RpcTest, idAsync)
{
    auto ids = std::vector<tr_variant>{};
    ids.emplace_back(12345);
    ids.emplace_back(0.5);
    ids.emplace_back("12345"sv);
    ids.emplace_back(nullptr);

    for (auto const& request_id : ids)
    {
        auto* tor = zeroTorrentInit(ZeroTorrentState::Complete);
        EXPECT_NE(nullptr, tor);

        auto request_map = tr_variant::Map{ 3U };
        request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
        request_map.try_emplace(TR_KEY_method, "torrent-rename-path");
        request_map[TR_KEY_id].merge(request_id); // copy

        auto params_map = tr_variant::Map{ 2U };
        params_map.try_emplace(TR_KEY_path, "files-filled-with-zeroes/512");
        params_map.try_emplace(TR_KEY_name, "512_test");
        request_map.try_emplace(TR_KEY_params, std::move(params_map));

        auto promise = std::promise<tr_variant>{};
        auto future = promise.get_future();
        tr_rpc_request_exec(
            session_,
            std::move(request_map),
            [&promise](tr_session* /*session*/, tr_variant&& resp) { promise.set_value(std::move(resp)); });
        auto const response = future.get();

        auto const* const response_map = response.get_if<tr_variant::Map>();
        ASSERT_NE(response_map, nullptr);
        auto const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
        EXPECT_NE(result, nullptr);
        auto const error = response_map->find(TR_KEY_error);
        EXPECT_EQ(error, std::end(*response_map));
        switch (request_id.index())
        {
        case tr_variant::IntIndex:
            EXPECT_EQ(request_id.value_if<int64_t>(), response_map->value_if<int64_t>(TR_KEY_id));
            break;
        case tr_variant::DoubleIndex:
            EXPECT_EQ(request_id.value_if<double>(), response_map->value_if<double>(TR_KEY_id));
            break;
        case tr_variant::StringIndex:
            EXPECT_EQ(request_id.value_if<std::string_view>(), response_map->value_if<std::string_view>(TR_KEY_id));
            break;
        case tr_variant::NullIndex:
            EXPECT_EQ(request_id.value_if<std::nullptr_t>(), response_map->value_if<std::nullptr_t>(TR_KEY_id));
            break;
        default:
            break;
        }

        // cleanup
        tr_torrentRemove(tor, false, nullptr, nullptr, nullptr, nullptr);
    }
}

TEST_F(RpcTest, tagAsyncLegacy)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::Complete);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_method, "torrent-rename-path");
    request_map.try_emplace(TR_KEY_tag, 12345);

    auto arguments_map = tr_variant::Map{ 2U };
    arguments_map.try_emplace(TR_KEY_path, "files-filled-with-zeroes/512");
    arguments_map.try_emplace(TR_KEY_name, "512_test");
    request_map.try_emplace(TR_KEY_arguments, std::move(arguments_map));

    auto promise = std::promise<tr_variant>{};
    auto future = promise.get_future();
    tr_rpc_request_exec(
        session_,
        std::move(request_map),
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

TEST_F(RpcTest, NotificationSync)
{
    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, "session_stats");

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        std::move(request_map),
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    EXPECT_FALSE(response.has_value());
}

TEST_F(RpcTest, NotificationAsync)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::Complete);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, "torrent_rename_path");

    auto params_map = tr_variant::Map{ 2U };
    params_map.try_emplace(TR_KEY_path, "files-filled-with-zeroes/512");
    params_map.try_emplace(TR_KEY_name, "512_test");
    request_map.try_emplace(TR_KEY_params, std::move(params_map));

    auto promise = std::promise<tr_variant>{};
    auto future = promise.get_future();
    tr_rpc_request_exec(
        session_,
        std::move(request_map),
        [&promise](tr_session* /*session*/, tr_variant&& resp) { promise.set_value(std::move(resp)); });
    auto const response = future.get();

    EXPECT_FALSE(response.has_value());

    // cleanup
    tr_torrentRemove(tor, false, nullptr, nullptr, nullptr, nullptr);
}

TEST_F(RpcTest, tagNoHandler)
{
    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, "sdgdhsgg");
    request_map.try_emplace(TR_KEY_id, 12345);

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        std::move(request_map),
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const jsonrpc = response_map->value_if<std::string_view>(TR_KEY_jsonrpc);
    ASSERT_TRUE(jsonrpc);
    EXPECT_EQ(*jsonrpc, JsonRpc::Version);
    auto const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    EXPECT_EQ(result, nullptr);
    auto const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto const error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, JsonRpc::Error::METHOD_NOT_FOUND);
    auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Method not found"sv);
    auto const id = response_map->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(id);
    EXPECT_EQ(*id, 12345);
}

TEST_F(RpcTest, tagNoHandlerLegacy)
{
    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_method, "sdgdhsgg");
    request_map.try_emplace(TR_KEY_tag, 12345);

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        std::move(request_map),
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

TEST_F(RpcTest, batch)
{
    auto request_vec = tr_variant::Vector{};
    request_vec.reserve(8U);

    auto request = tr_variant::Map{ 3U };
    request.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request.try_emplace(TR_KEY_method, "session-stats");
    request.try_emplace(TR_KEY_id, 12345);
    request_vec.emplace_back(std::move(request));

    request = tr_variant::Map{ 2U };
    request.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request.try_emplace(TR_KEY_method, "session-set");
    request_vec.emplace_back(std::move(request));

    request = tr_variant::Map{ 3U };
    request.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request.try_emplace(TR_KEY_method, "session-stats");
    request.try_emplace(TR_KEY_id, "12345"sv);
    request_vec.emplace_back(std::move(request));

    request = tr_variant::Map{ 1U };
    request.try_emplace(tr_quark_new("foo"sv), "boo"sv);
    request_vec.emplace_back(std::move(request));

    request_vec.emplace_back(1);

    request = tr_variant::Map{ 3U };
    request.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request.try_emplace(TR_KEY_method, "dnfsojnsdkjf");
    request.try_emplace(TR_KEY_id, 12345);
    request_vec.emplace_back(std::move(request));

    request = tr_variant::Map{ 1U };
    request.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request.try_emplace(TR_KEY_method, "dnfsojnsdkjf");
    request_vec.emplace_back(std::move(request));

    request = tr_variant::Map{ 2U };
    request.try_emplace(TR_KEY_method, "session-stats");
    request.try_emplace(TR_KEY_tag, 12345);
    request_vec.emplace_back(std::move(request));

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        std::move(request_vec),
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    auto* const response_vec_ptr = response.get_if<tr_variant::Vector>();
    ASSERT_NE(response_vec_ptr, nullptr);
    auto const& response_vec = *response_vec_ptr;

    ASSERT_EQ(std::size(response_vec), 6U);

    auto const* response_map = response_vec[0].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const* result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    EXPECT_NE(result, nullptr);
    auto error_it = response_map->find(TR_KEY_error);
    EXPECT_EQ(error_it, std::end(*response_map));
    auto id_int = response_map->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(id_int);
    EXPECT_EQ(*id_int, 12345);

    response_map = response_vec[1].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    EXPECT_NE(result, nullptr);
    error_it = response_map->find(TR_KEY_error);
    EXPECT_EQ(error_it, std::end(*response_map));
    auto id_str = response_map->value_if<std::string_view>(TR_KEY_id);
    ASSERT_TRUE(id_str);
    EXPECT_EQ(*id_str, "12345"sv);

    response_map = response_vec[2].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto result_it = response_map->find(TR_KEY_result);
    EXPECT_EQ(result_it, std::end(*response_map));
    auto error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
    auto error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Invalid Request"sv);
    auto id_null = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id_null);

    response_map = response_vec[3].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    result_it = response_map->find(TR_KEY_result);
    EXPECT_EQ(result_it, std::end(*response_map));
    error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
    error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Invalid Request"sv);
    auto error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
    ASSERT_NE(error_data, nullptr);
    auto error_string = error_data->value_if<std::string_view>(TR_KEY_errorString);
    ASSERT_TRUE(error_string);
    EXPECT_EQ(*error_string, "request must be an Object"sv);
    id_null = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id_null);

    response_map = response_vec[4].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    result_it = response_map->find(TR_KEY_result);
    EXPECT_EQ(result_it, std::end(*response_map));
    error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32601); // don't use constants here in case they are wrong
    error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Method not found"sv);
    id_int = response_map->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(id_int);
    EXPECT_EQ(*id_int, 12345);

    response_map = response_vec[5].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    result_it = response_map->find(TR_KEY_result);
    EXPECT_EQ(result_it, std::end(*response_map));
    error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
    error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Invalid Request"sv);
    error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
    ASSERT_NE(error_data, nullptr);
    error_string = error_data->value_if<std::string_view>(TR_KEY_errorString);
    ASSERT_TRUE(error_string);
    EXPECT_EQ(*error_string, "JSON-RPC version is not 2.0"sv);
    id_null = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id_null);
}

/***
****
***/

TEST_F(RpcTest, sessionGet)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, "session-get"sv);
    request_map.try_emplace(TR_KEY_id, 12345);
    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        std::move(request_map),
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    auto* response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto* args_map = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    ASSERT_NE(args_map, nullptr);

    // what we expected
    static auto constexpr ExpectedKeys = std::array{
        TR_KEY_alt_speed_down,
        TR_KEY_alt_speed_enabled,
        TR_KEY_alt_speed_time_begin,
        TR_KEY_alt_speed_time_day,
        TR_KEY_alt_speed_time_enabled,
        TR_KEY_alt_speed_time_end,
        TR_KEY_alt_speed_up,
        TR_KEY_anti_brute_force_enabled,
        TR_KEY_anti_brute_force_threshold,
        TR_KEY_blocklist_enabled,
        TR_KEY_blocklist_size,
        TR_KEY_blocklist_url,
        TR_KEY_cache_size_mb,
        TR_KEY_config_dir,
        TR_KEY_default_trackers,
        TR_KEY_dht_enabled,
        TR_KEY_download_dir,
        TR_KEY_download_dir_free_space,
        TR_KEY_download_queue_enabled,
        TR_KEY_download_queue_size,
        TR_KEY_encryption,
        TR_KEY_idle_seeding_limit,
        TR_KEY_idle_seeding_limit_enabled,
        TR_KEY_incomplete_dir,
        TR_KEY_incomplete_dir_enabled,
        TR_KEY_lpd_enabled,
        TR_KEY_peer_limit_global,
        TR_KEY_peer_limit_per_torrent,
        TR_KEY_peer_port,
        TR_KEY_peer_port_random_on_start,
        TR_KEY_pex_enabled,
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
        TR_KEY_seed_queue_enabled,
        TR_KEY_seed_queue_size,
        TR_KEY_seedRatioLimit,
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
    for (auto const& [key, val] : *args_map)
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

    auto request = tr_variant::Map{ 3U };

    request.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request.try_emplace(TR_KEY_method, "torrent-get"sv);
    request.try_emplace(TR_KEY_id, 12345);

    auto params = tr_variant::Map{ 1U };
    auto fields = tr_variant::Vector{};
    fields.emplace_back(tr_quark_get_string_view(TR_KEY_id));
    params.try_emplace(TR_KEY_fields, std::move(fields));
    request.try_emplace(TR_KEY_params, std::move(params));

    auto response = tr_variant{};
    tr_rpc_request_exec(
        session_,
        std::move(request),
        [&response](tr_session* /*session*/, tr_variant&& resp) { response = std::move(resp); });

    auto* response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto* result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    ASSERT_NE(result, nullptr);

    auto* torrents = result->find_if<tr_variant::Vector>(TR_KEY_torrents);
    ASSERT_NE(torrents, nullptr);
    EXPECT_EQ(1UL, std::size(*torrents));

    auto* first_torrent = (*torrents)[0].get_if<tr_variant::Map>();
    ASSERT_NE(first_torrent, nullptr);
    auto first_torrent_id = first_torrent->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(first_torrent_id);
    EXPECT_EQ(1, *first_torrent_id);

    // cleanup
    tr_torrentRemove(tor, false, nullptr, nullptr, nullptr, nullptr);
}

} // namespace libtransmission::test
