// This file Copyright (C) 2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cstddef> // size_t
#include <cstdint> // int64_t
#include <memory>
#include <string_view>

#include <libtransmission/transmission.h>

#include <libtransmission/log.h>
#include <libtransmission/net.h>
#include <libtransmission/open-files.h>
#include <libtransmission/peer-io.h>
#include <libtransmission/quark.h>
#include <libtransmission/session.h>
#include <libtransmission/variant.h>

#include "gtest/gtest.h"

using namespace std::literals;

using SettingsTest = ::testing::Test;

TEST_F(SettingsTest, canInstantiate)
{
    auto settings = tr_session::Settings{};

    auto var = settings.save();
    EXPECT_TRUE(var.has_value());
}

TEST_F(SettingsTest, canLoadBools)
{
    static auto constexpr Key = TR_KEY_seed_queue_enabled;

    auto settings = tr_session::Settings{};
    auto const expected_value = !settings.seed_queue_enabled;

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, expected_value);
    settings.load(tr_variant{ std::move(map) });

    EXPECT_EQ(expected_value, settings.seed_queue_enabled);
}

TEST_F(SettingsTest, canSaveBools)
{
    static auto constexpr Key = TR_KEY_seed_queue_enabled;

    auto settings = tr_session::Settings{};
    auto const expected_value = !settings.seed_queue_enabled;
    settings.seed_queue_enabled = expected_value;

    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<bool>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(expected_value, *val);
}

TEST_F(SettingsTest, canLoadDoubles)
{
    static auto constexpr Key = TR_KEY_ratio_limit;

    auto settings = tr_session::Settings{};
    auto const expected_value = settings.ratio_limit + 1.0;

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, expected_value);
    settings.load(tr_variant{ std::move(map) });
    EXPECT_NEAR(expected_value, settings.ratio_limit, 0.001);
}

TEST_F(SettingsTest, canSaveDoubles)
{
    static auto constexpr Key = TR_KEY_seed_queue_enabled;

    auto settings = tr_session::Settings{};
    auto const default_value = settings.seed_queue_enabled;
    auto const expected_value = !default_value;
    settings.seed_queue_enabled = expected_value;

    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<bool>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(expected_value, *val);
}

TEST_F(SettingsTest, canLoadEncryptionMode)
{
    static auto constexpr Key = TR_KEY_encryption;
    static auto constexpr ExpectedValue = TR_ENCRYPTION_REQUIRED;

    auto settings = std::make_unique<tr_session::Settings>();
    ASSERT_NE(ExpectedValue, settings->encryption_mode);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, ExpectedValue);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->encryption_mode);

    settings = std::make_unique<tr_session::Settings>();
    map = tr_variant::Map{ 1U };
    map.try_emplace(Key, "required"sv);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->encryption_mode);
}

TEST_F(SettingsTest, canSaveEncryptionMode)
{
    static auto constexpr Key = TR_KEY_encryption;
    static auto constexpr ExpectedValue = TR_ENCRYPTION_REQUIRED;

    auto settings = tr_session::Settings{};
    EXPECT_NE(ExpectedValue, settings.seed_queue_enabled);
    settings.encryption_mode = ExpectedValue;

    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<int64_t>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(ExpectedValue, *val);
}

TEST_F(SettingsTest, canLoadLogLevel)
{
    static auto constexpr Key = TR_KEY_message_level;

    auto settings = std::make_unique<tr_session::Settings>();
    auto const default_value = settings->log_level;
    auto constexpr ExpectedValue = TR_LOG_DEBUG;
    ASSERT_NE(ExpectedValue, default_value);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, ExpectedValue);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->log_level);

    settings = std::make_unique<tr_session::Settings>();
    map = tr_variant::Map{ 1U };
    map.try_emplace(Key, "debug"sv);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->log_level);
}

TEST_F(SettingsTest, canSaveLogLevel)
{
    static auto constexpr Key = TR_KEY_message_level;

    auto settings = tr_session::Settings{};
    auto const default_value = settings.log_level;
    auto constexpr ExpectedValue = TR_LOG_DEBUG;
    ASSERT_NE(ExpectedValue, default_value);

    settings.log_level = ExpectedValue;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<int64_t>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(ExpectedValue, *val);
}

TEST_F(SettingsTest, canLoadMode)
{
    static auto constexpr Key = TR_KEY_umask;

    auto settings = std::make_unique<tr_session::Settings>();
    auto const default_value = settings->umask;
    auto constexpr ExpectedValue = tr_mode_t{ 0777 };
    ASSERT_NE(ExpectedValue, default_value);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, ExpectedValue);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->umask);

    settings = std::make_unique<tr_session::Settings>();
    map = tr_variant::Map{ 1U };
    map.try_emplace(Key, "0777"sv);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->umask);
}

TEST_F(SettingsTest, canSaveMode)
{
    static auto constexpr Key = TR_KEY_umask;

    auto settings = tr_session::Settings{};
    auto const default_value = settings.log_level;
    auto constexpr ExpectedValue = tr_mode_t{ 0777 };
    ASSERT_NE(ExpectedValue, default_value);

    settings.umask = ExpectedValue;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<std::string_view>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ("0777"sv, *val);
}

TEST_F(SettingsTest, canLoadPort)
{
    static auto constexpr Key = TR_KEY_peer_port;

    auto settings = tr_session::Settings{};
    auto const default_value = settings.peer_port;
    auto constexpr ExpectedValue = tr_port::from_host(8080);
    ASSERT_NE(ExpectedValue, default_value);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, ExpectedValue.host());
    settings.load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings.peer_port);
}

TEST_F(SettingsTest, canSavePort)
{
    static auto constexpr Key = TR_KEY_peer_port;

    auto settings = tr_session::Settings{};
    auto const default_value = settings.peer_port;
    auto constexpr ExpectedValue = tr_port::from_host(8080);
    ASSERT_NE(ExpectedValue, default_value);

    settings.peer_port = ExpectedValue;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<int64_t>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(ExpectedValue.host(), *val);
}

TEST_F(SettingsTest, canLoadPreallocation)
{
    static auto constexpr Key = TR_KEY_preallocation;

    auto settings = std::make_unique<tr_session::Settings>();
    auto const default_value = settings->preallocation_mode;
    auto constexpr ExpectedValue = tr_open_files::Preallocation::Full;
    ASSERT_NE(ExpectedValue, default_value);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, static_cast<int64_t>(ExpectedValue));
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->preallocation_mode);

    settings = std::make_unique<tr_session::Settings>();
    map = tr_variant::Map{ 1U };
    map.try_emplace(Key, "full"sv);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->preallocation_mode);
}

TEST_F(SettingsTest, canSavePreallocation)
{
    static auto constexpr Key = TR_KEY_preallocation;

    auto settings = tr_session::Settings{};
    auto const default_value = settings.preallocation_mode;
    auto constexpr ExpectedValue = tr_open_files::Preallocation::Full;
    ASSERT_NE(ExpectedValue, default_value);

    settings.preallocation_mode = ExpectedValue;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<int64_t>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(static_cast<int64_t>(ExpectedValue), *val);
}

TEST_F(SettingsTest, canLoadSizeT)
{
    static auto constexpr Key = TR_KEY_queue_stalled_minutes;

    auto settings = tr_session::Settings{};
    auto const expected_value = settings.queue_stalled_minutes + 5U;

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, expected_value);
    settings.load(tr_variant{ std::move(map) });
    EXPECT_EQ(expected_value, settings.queue_stalled_minutes);
}

TEST_F(SettingsTest, canSaveSizeT)
{
    static auto constexpr Key = TR_KEY_queue_stalled_minutes;

    auto settings = tr_session::Settings{};
    auto const expected_value = settings.queue_stalled_minutes + 5U;

    settings.queue_stalled_minutes = expected_value;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<int64_t>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(expected_value, static_cast<size_t>(*val));
}

TEST_F(SettingsTest, canLoadString)
{
    static auto constexpr Key = TR_KEY_bind_address_ipv4;
    static auto constexpr ChangedValue = std::string_view{ "127.0.0.1" };

    auto settings = tr_session::Settings{};
    EXPECT_NE(ChangedValue, tr_session::Settings{}.bind_address_ipv4);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, ChangedValue);
    settings.load(tr_variant{ std::move(map) });
    EXPECT_EQ(ChangedValue, settings.bind_address_ipv4);
}

TEST_F(SettingsTest, canSaveString)
{
    static auto constexpr Key = TR_KEY_bind_address_ipv4;
    static auto constexpr ChangedValue = std::string_view{ "127.0.0.1" };

    auto settings = tr_session::Settings{};
    EXPECT_NE(ChangedValue, tr_session::Settings{}.bind_address_ipv4);

    settings.bind_address_ipv4 = ChangedValue;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<std::string_view>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(ChangedValue, *val);
}

TEST_F(SettingsTest, canLoadTos)
{
    static auto constexpr Key = TR_KEY_peer_socket_tos;
    static auto constexpr ChangedValue = tr_tos_t{ 0x20 };

    auto settings = std::make_unique<tr_session::Settings>();
    auto const default_value = settings->peer_socket_tos;
    ASSERT_NE(ChangedValue, default_value);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, 0x20);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ChangedValue, settings->peer_socket_tos);

    settings = std::make_unique<tr_session::Settings>();
    map = tr_variant::Map{ 1U };
    map.try_emplace(Key, "cs1"sv);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ChangedValue, settings->peer_socket_tos);
}

TEST_F(SettingsTest, canSaveTos)
{
    static auto constexpr Key = TR_KEY_peer_socket_tos;
    static auto constexpr ChangedValue = tr_tos_t{ 0x20 };

    auto settings = tr_session::Settings{};
    ASSERT_NE(ChangedValue, settings.peer_socket_tos);

    settings.peer_socket_tos = tr_tos_t(0x20);
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<std::string_view>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ(ChangedValue.toString(), *val);
}

TEST_F(SettingsTest, canLoadVerify)
{
    static auto constexpr Key = TR_KEY_torrent_added_verify_mode;
    static auto constexpr ChangedValue = TR_VERIFY_ADDED_FULL;

    auto settings = std::make_unique<tr_session::Settings>();
    auto const default_value = settings->torrent_added_verify_mode;
    ASSERT_NE(ChangedValue, default_value);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, "full"sv);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ChangedValue, settings->torrent_added_verify_mode);

    settings = std::make_unique<tr_session::Settings>();
    map = tr_variant::Map{ 1U };
    map.try_emplace(Key, ChangedValue);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ChangedValue, settings->torrent_added_verify_mode);
}

TEST_F(SettingsTest, canSaveVerify)
{
    static auto constexpr Key = TR_KEY_torrent_added_verify_mode;
    static auto constexpr ChangedValue = TR_VERIFY_ADDED_FULL;

    auto settings = tr_session::Settings{};
    ASSERT_NE(ChangedValue, settings.torrent_added_verify_mode);

    settings.torrent_added_verify_mode = ChangedValue;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<std::string_view>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ("full"sv, *val);
}

TEST_F(SettingsTest, canLoadPreferredTransport)
{
    static auto constexpr Key = TR_KEY_preferred_transport;
    static auto constexpr ExpectedValue = std::array{ TR_PREFER_TCP, TR_PREFER_UTP };

    auto settings = std::make_unique<tr_session::Settings>();
    auto const& default_value = settings->preferred_transport;
    ASSERT_NE(ExpectedValue, default_value);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, ExpectedValue.front());
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->preferred_transport);

    settings = std::make_unique<tr_session::Settings>();
    map = tr_variant::Map{ 1U };
    map.try_emplace(Key, "tcp"sv);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->preferred_transport);
}

TEST_F(SettingsTest, canSavePreferredTransport)
{
    static auto constexpr Key = TR_KEY_preferred_transport;
    static auto constexpr ExpectedValue = std::array{ TR_PREFER_TCP, TR_PREFER_UTP };

    auto settings = tr_session::Settings{};
    auto const& default_value = settings.preferred_transport;
    ASSERT_NE(ExpectedValue, default_value);

    settings.preferred_transport = ExpectedValue;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val = map->value_if<std::string_view>(Key);
    ASSERT_TRUE(val);
    EXPECT_EQ("tcp"sv, *val);
}

TEST_F(SettingsTest, canLoadSleepPerSecondsDuringVerify)
{
    static auto constexpr Key = TR_KEY_sleep_per_seconds_during_verify;
    auto constexpr ExpectedValue = 90ms;

    auto settings = std::make_unique<tr_session::Settings>();
    auto const default_value = settings->sleep_per_seconds_during_verify;
    ASSERT_NE(ExpectedValue, default_value);

    auto map = tr_variant::Map{ 1U };
    map.try_emplace(Key, ExpectedValue.count());
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->sleep_per_seconds_during_verify);

    settings = std::make_unique<tr_session::Settings>();
    map = tr_variant::Map{ 1U };
    map.try_emplace(Key, 90);
    settings->load(tr_variant{ std::move(map) });
    EXPECT_EQ(ExpectedValue, settings->sleep_per_seconds_during_verify);
}

TEST_F(SettingsTest, canSaveSleepPerSecondsDuringVerify)
{
    static auto constexpr Key = TR_KEY_sleep_per_seconds_during_verify;
    static auto constexpr ExpectedValue = 90ms;

    auto settings = tr_session::Settings{};
    auto const default_value = settings.sleep_per_seconds_during_verify;
    ASSERT_NE(ExpectedValue, default_value);

    settings.sleep_per_seconds_during_verify = ExpectedValue;
    auto var = settings.save();
    auto* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto const val_raw = map->value_if<int64_t>(Key);
    ASSERT_TRUE(val_raw);
    EXPECT_EQ(ExpectedValue, std::chrono::milliseconds{ *val_raw });
}
