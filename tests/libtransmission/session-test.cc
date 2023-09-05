// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

#include <libtransmission/transmission.h>

#include <libtransmission/crypto-utils.h>
#include <libtransmission/quark.h>
#include <libtransmission/session-id.h>
#include <libtransmission/session.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>

#include "gtest/gtest.h"
#include "test-fixtures.h"

using namespace std::literals;

namespace libtransmission::test
{

TEST_F(SessionTest, propertiesApi)
{
    // Note, this test is just for confirming that the getters/setters
    // in both the tr_session class and in the C API bindings all work,
    // e.g. you can get back the same value you set in.
    //
    // Confirming that each of these settings _does_ something in the session
    // is a much broader scope and left to other tests :)

    auto* const session = session_;

    // download dir

    for (auto const& value : { "foo"sv, "bar"sv, ""sv })
    {
        session->setDownloadDir(value);
        EXPECT_EQ(value, session->downloadDir());
        EXPECT_EQ(value, tr_sessionGetDownloadDir(session));

        tr_sessionSetDownloadDir(session, std::string(value).c_str());
        EXPECT_EQ(value, session->downloadDir());
        EXPECT_EQ(value, tr_sessionGetDownloadDir(session));
    }

    tr_sessionSetDownloadDir(session, nullptr);
    EXPECT_EQ(""sv, session->downloadDir());
    EXPECT_EQ(""sv, tr_sessionGetDownloadDir(session));

    // incomplete dir

    for (auto const& value : { "foo"sv, "bar"sv, ""sv })
    {
        session->setIncompleteDir(value);
        EXPECT_EQ(value, session->incompleteDir());
        EXPECT_EQ(value, tr_sessionGetIncompleteDir(session));

        tr_sessionSetIncompleteDir(session, std::string(value).c_str());
        EXPECT_EQ(value, session->incompleteDir());
        EXPECT_EQ(value, tr_sessionGetIncompleteDir(session));
    }

    tr_sessionSetIncompleteDir(session, nullptr);
    EXPECT_EQ(""sv, session->incompleteDir());
    EXPECT_EQ(""sv, tr_sessionGetIncompleteDir(session));

    // script

    for (auto const& type : { TR_SCRIPT_ON_TORRENT_ADDED, TR_SCRIPT_ON_TORRENT_DONE })
    {
        for (auto const& value : { "foo"sv, "bar"sv, ""sv })
        {
            session->setScript(type, value);
            EXPECT_EQ(value, session->script(type));
            EXPECT_EQ(value, tr_sessionGetScript(session, type));

            tr_sessionSetScript(session, type, std::string(value).c_str());
            EXPECT_EQ(value, session->script(type));
            EXPECT_EQ(value, tr_sessionGetScript(session, type));
        }

        tr_sessionSetScript(session, type, nullptr);
        EXPECT_EQ(""sv, session->script(type));
        EXPECT_EQ(""sv, tr_sessionGetScript(session, type));

        for (auto const value : { true, false })
        {
            session->useScript(type, value);
            EXPECT_EQ(value, session->useScript(type));
            EXPECT_EQ(value, tr_sessionIsScriptEnabled(session, type));

            tr_sessionSetScriptEnabled(session, type, value);
            EXPECT_EQ(value, session->useScript(type));
            EXPECT_EQ(value, tr_sessionIsScriptEnabled(session, type));
        }
    }

    // incomplete dir enabled

    for (auto const value : { true, false })
    {
        session->useIncompleteDir(value);
        EXPECT_EQ(value, session->useIncompleteDir());
        EXPECT_EQ(value, tr_sessionIsIncompleteDirEnabled(session));

        tr_sessionSetIncompleteDirEnabled(session, value);
        EXPECT_EQ(value, session->useIncompleteDir());
        EXPECT_EQ(value, tr_sessionIsIncompleteDirEnabled(session));
    }

    // blocklist url

    for (auto const& value : { "foo"sv, "bar"sv, ""sv })
    {
        session->setBlocklistUrl(value);
        EXPECT_EQ(value, session->blocklistUrl());
        EXPECT_EQ(value, tr_blocklistGetURL(session));

        tr_blocklistSetURL(session, std::string(value).c_str());
        EXPECT_EQ(value, session->blocklistUrl());
        EXPECT_EQ(value, tr_blocklistGetURL(session));
    }

    tr_blocklistSetURL(session, nullptr);
    EXPECT_EQ(""sv, session->blocklistUrl());
    EXPECT_EQ(""sv, tr_blocklistGetURL(session));

    // rpc username

    for (auto const& value : { "foo"sv, "bar"sv, ""sv })
    {
        tr_sessionSetRPCUsername(session, std::string{ value }.c_str());
        EXPECT_EQ(value, tr_sessionGetRPCUsername(session));
    }

    tr_sessionSetRPCUsername(session, nullptr);
    EXPECT_EQ(""sv, tr_sessionGetRPCUsername(session));

    // rpc password (unsalted)

    {
        auto const value = "foo"sv;
        tr_sessionSetRPCPassword(session, std::string{ value }.c_str());
        EXPECT_NE(value, tr_sessionGetRPCPassword(session));
        EXPECT_EQ('{', tr_sessionGetRPCPassword(session)[0]);
    }

    // rpc password (salted)

    {
        auto const plaintext = "foo"sv;
        auto const salted = tr_ssha1(plaintext);
        tr_sessionSetRPCPassword(session, salted.c_str());
        EXPECT_EQ(salted, tr_sessionGetRPCPassword(session));
    }

    // blocklist enabled

    for (auto const value : { true, false })
    {
        session->useBlocklist(value);
        EXPECT_EQ(value, session->useBlocklist());
        EXPECT_EQ(value, tr_blocklistIsEnabled(session));

        tr_sessionSetIncompleteDirEnabled(session, value);
        EXPECT_EQ(value, session->useBlocklist());
        EXPECT_EQ(value, tr_blocklistIsEnabled(session));
    }
}

TEST_F(SessionTest, peerId)
{
    auto const peer_id_prefix = std::string{ PEERID_PREFIX };

    for (int i = 0; i < 100000; ++i)
    {
        // get a new peer-id
        auto const buf = tr_peerIdInit();

        // confirm that it begins with peer_id_prefix
        auto const peer_id = std::string_view{ reinterpret_cast<char const*>(buf.data()), std::size(buf) };
        EXPECT_EQ(peer_id_prefix, peer_id.substr(0, peer_id_prefix.size()));

        // confirm that its total is evenly divisible by 36
        int val = 0;
        auto const suffix = peer_id.substr(peer_id_prefix.size());
        for (char const ch : suffix)
        {
            auto const tmp = std::array<char, 2>{ ch, '\0' };
            val += strtoul(tmp.data(), nullptr, 36);
        }

        EXPECT_EQ(0, val % 36);
    }
}

namespace current_time_mock
{
namespace
{
auto value = time_t{};
}

time_t get()
{
    return value;
}

void set(time_t now)
{
    value = now;
}

} // namespace current_time_mock

TEST_F(SessionTest, sessionId)
{
#ifdef __sun
    // FIXME: File locking doesn't work as expected
    GTEST_SKIP();
#endif

    EXPECT_FALSE(tr_session_id::is_local(""));
    EXPECT_FALSE(tr_session_id::is_local("test"));

    current_time_mock::set(0U);
    auto session_id = std::make_unique<tr_session_id>(current_time_mock::get);

    EXPECT_NE(""sv, session_id->sv());
    EXPECT_EQ(session_id->sv(), session_id->c_str()) << session_id->sv() << ", " << session_id->c_str();
    EXPECT_EQ(48U, strlen(session_id->c_str()));
    auto session_id_str_1 = std::string{ session_id->sv() };
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));

    current_time_mock::set(current_time_mock::get() + (3600U - 1U));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));
    auto session_id_str_2 = std::string{ session_id->sv() };
    EXPECT_EQ(session_id_str_1, session_id_str_2);

    current_time_mock::set(3600U);
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));
    session_id_str_2 = std::string{ session_id->sv() };
    EXPECT_NE(session_id_str_1, session_id_str_2);
    EXPECT_EQ(session_id_str_2, session_id->c_str());
    EXPECT_EQ(48U, strlen(session_id->c_str()));

    EXPECT_TRUE(tr_session_id::is_local(session_id_str_2));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));
    current_time_mock::set(7200U);
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_2));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));

    auto const session_id_str_3 = std::string{ session_id->sv() };
    EXPECT_EQ(48U, std::size(session_id_str_3));
    EXPECT_NE(session_id_str_2, session_id_str_3);
    EXPECT_NE(session_id_str_1, session_id_str_3);

    EXPECT_TRUE(tr_session_id::is_local(session_id_str_3));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_2));
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_1));

    current_time_mock::set(36000U);
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_3));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_2));
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_1));

    session_id.reset();
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_3));
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_2));
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_1));
}

TEST_F(SessionTest, getDefaultSettingsIncludesSubmodules)
{
    auto settings = tr_variant::make_map();
    tr_sessionGetDefaultSettings(&settings);

    // Choose a setting from each of [tr_session, tr_session_alt_speeds, tr_rpc_server] to test all of them.
    // These are all `false` by default
    for (auto const& key : { TR_KEY_peer_port_random_on_start, TR_KEY_alt_speed_time_enabled, TR_KEY_rpc_enabled })
    {
        auto flag = bool{};
        EXPECT_TRUE(tr_variantDictFindBool(&settings, key, &flag));
        EXPECT_FALSE(flag);
    }
}

TEST_F(SessionTest, honorsSettings)
{
    // Baseline: confirm that these settings are disabled by default
    EXPECT_FALSE(session_->isPortRandom());
    EXPECT_FALSE(tr_sessionUsesAltSpeedTime(session_));
    EXPECT_FALSE(tr_sessionIsRPCEnabled(session_));

    // Choose a setting from each of [tr_session, tr_session_alt_speeds, tr_rpc_server] to test all of them.
    // These are all `false` by default
    auto settings = tr_variant::make_map();
    tr_sessionGetDefaultSettings(&settings);
    for (auto const& key : { TR_KEY_peer_port_random_on_start, TR_KEY_alt_speed_time_enabled, TR_KEY_rpc_enabled })
    {
        tr_variantDictRemove(&settings, key);
        tr_variantDictAddBool(&settings, key, true);
    }
    auto* session = tr_sessionInit(sandboxDir().data(), false, &settings);

    // confirm that these settings were enabled
    EXPECT_TRUE(session->isPortRandom());
    EXPECT_TRUE(tr_sessionUsesAltSpeedTime(session));
    EXPECT_TRUE(tr_sessionIsRPCEnabled(session));

    tr_sessionClose(session);
}

TEST_F(SessionTest, savesSettings)
{
    // Baseline: confirm that these settings are disabled by default
    EXPECT_FALSE(session_->isPortRandom());
    EXPECT_FALSE(tr_sessionUsesAltSpeedTime(session_));
    EXPECT_FALSE(tr_sessionIsRPCEnabled(session_));

    tr_sessionSetPeerPortRandomOnStart(session_, true);
    tr_sessionUseAltSpeedTime(session_, true);
    tr_sessionSetRPCEnabled(session_, true);

    // Choose a setting from each of [tr_session, tr_session_alt_speeds, tr_rpc_server] to test all of them.
    auto settings = tr_variant::make_map();
    tr_sessionGetSettings(session_, &settings);
    for (auto const& key : { TR_KEY_peer_port_random_on_start, TR_KEY_alt_speed_time_enabled, TR_KEY_rpc_enabled })
    {
        auto flag = bool{};
        EXPECT_TRUE(tr_variantDictFindBool(&settings, key, &flag));
        EXPECT_TRUE(flag);
    }
}

} // namespace libtransmission::test
