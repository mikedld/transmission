// This file Copyright (C) 2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef> // size_t, std::byte
#include <ctime> // time(), time_t
#include <fstream>
#include <iterator> // std::back_inserter
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h> // addrinfo, freeaddrinfo
#include <sys/socket.h> // AF_INET, AF_INET6, AF_UN...
#endif

#include <dht/dht.h> // dht_callback_t

#include <event2/event.h>

#include <fmt/core.h>

#include <libtransmission/transmission.h>

#include <libtransmission/crypto-utils.h> // tr_rand_obj
#include <libtransmission/file.h>
#include <libtransmission/net.h>
#include <libtransmission/quark.h>
#include <libtransmission/session-thread.h> // for tr_evthread_init();
#include <libtransmission/timer.h>
#include <libtransmission/timer-ev.h>
#include <libtransmission/tr-dht.h>
#include <libtransmission/tr-macros.h>
#include <libtransmission/tr-strbuf.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h> // tr_variantDictAddRaw

#include "gtest/gtest.h"
#include "test-fixtures.h"

#ifdef _WIN32
#undef gai_strerror
#define gai_strerror gai_strerrorA
#endif

using namespace std::literals;

namespace libtransmission::test
{

bool waitFor(struct event_base* event_base, std::chrono::milliseconds msec)
{
    return waitFor(
        event_base,
        []() { return false; },
        msec);
}

namespace
{
auto constexpr IdLength = size_t{ 20U };
auto constexpr MockTimerInterval = 40ms;

} // namespace

class DhtTest : public SandboxedTest
{
protected:
    // Helper for creating a mock dht.dat state file
    struct MockStateFile
    {
        // Fake data to be written to the test state file

        std::array<char, IdLength> const id_ = tr_rand_obj<std::array<char, IdLength>>();

        std::vector<tr_socket_address> ipv4_nodes_ = { { *tr_address::from_string("10.10.10.1"), tr_port::from_host(128) },
                                                       { *tr_address::from_string("10.10.10.2"), tr_port::from_host(129) },
                                                       { *tr_address::from_string("10.10.10.3"), tr_port::from_host(130) },
                                                       { *tr_address::from_string("10.10.10.4"), tr_port::from_host(131) },
                                                       { *tr_address::from_string("10.10.10.5"), tr_port::from_host(132) } };

        std::vector<tr_socket_address> ipv6_nodes_ = {
            { *tr_address::from_string("1002:1035:4527:3546:7854:1237:3247:3217"), tr_port::from_host(6881) },
            { *tr_address::from_string("1002:1035:4527:3546:7854:1237:3247:3218"), tr_port::from_host(6882) },
            { *tr_address::from_string("1002:1035:4527:3546:7854:1237:3247:3219"), tr_port::from_host(6883) },
            { *tr_address::from_string("1002:1035:4527:3546:7854:1237:3247:3220"), tr_port::from_host(6884) },
            { *tr_address::from_string("1002:1035:4527:3546:7854:1237:3247:3221"), tr_port::from_host(6885) }
        };

        [[nodiscard]] auto nodesString() const
        {
            auto str = std::string{};
            for (auto const& socket_address : ipv4_nodes_)
            {
                str += socket_address.display_name();
                str += ',';
            }
            for (auto const& socket_address : ipv6_nodes_)
            {
                str += socket_address.display_name();
                str += ',';
            }
            return str;
        }

        [[nodiscard]] static auto filename(std::string_view dirname)
        {
            return std::string{ dirname } + "/dht.dat";
        }

        void save(std::string_view path) const
        {
            auto const dat_file = MockStateFile::filename(path);

            auto dict = tr_variant::make_map(3U);
            tr_variantDictAddRaw(&dict, TR_KEY_id, std::data(id_), std::size(id_));
            auto compact = std::vector<std::byte>{};
            for (auto const& socket_address : ipv4_nodes_)
            {
                socket_address.to_compact(std::back_inserter(compact));
            }
            tr_variantDictAddRaw(&dict, TR_KEY_nodes, std::data(compact), std::size(compact));
            compact.clear();
            for (auto const& socket_address : ipv6_nodes_)
            {
                socket_address.to_compact(std::back_inserter(compact));
            }
            tr_variantDictAddRaw(&dict, TR_KEY_nodes6, std::data(compact), std::size(compact));
            tr_variant_serde::benc().to_file(dict, dat_file);
        }
    };

    // A fake libdht for the tests to call
    class MockDht final : public tr_dht::API
    {
    public:
        int get_nodes(struct sockaddr_in* /*sin*/, int* /*max*/, struct sockaddr_in6* /*sin6*/, int* /*max6*/) override
        {
            return 0;
        }

        int nodes(int /*af*/, int* good, int* dubious, int* cached, int* incoming) override
        {
            if (good != nullptr)
            {
                *good = good_;
            }

            if (dubious != nullptr)
            {
                *dubious = dubious_;
            }

            if (cached != nullptr)
            {
                *cached = cached_;
            }

            if (incoming != nullptr)
            {
                *incoming = incoming_;
            }

            return 0;
        }

        int periodic(
            void const* /*buf*/,
            size_t /*buflen*/,
            sockaddr const /*from*/*,
            int /*fromlen*/,
            time_t* /*tosleep*/,
            dht_callback_t /*callback*/,
            void* /*closure*/) override
        {
            ++n_periodic_calls_;
            return 0;
        }

        int ping_node(struct sockaddr const* sa, int /*salen*/) override
        {
            auto addrport = tr_socket_address::from_sockaddr(sa);
            assert(addrport);
            pinged_.push_back(Pinged{ *addrport, tr_time() });
            return 0;
        }

        int search(unsigned char const* id, int port, int af, dht_callback_t /*callback*/, void* /*closure*/) override
        {
            auto info_hash = tr_sha1_digest_t{};
            std::copy_n(reinterpret_cast<std::byte const*>(id), std::size(info_hash), std::data(info_hash));
            searched_.push_back(Searched{ info_hash, tr_port::from_host(port), af });
            return 0;
        }

        int init(int dht_socket, int dht_socket6, unsigned char const* id, unsigned char const* /*v*/) override
        {
            inited_ = true;
            dht_socket_ = dht_socket;
            dht_socket6_ = dht_socket6;
            std::copy_n(id, std::size(id_), std::begin(id_));
            return 0;
        }

        int uninit() override
        {
            inited_ = false;
            return 0;
        }

        constexpr void setHealthySwarm()
        {
            good_ = 50;
            incoming_ = 10;
        }

        constexpr void setFirewalledSwarm()
        {
            good_ = 50;
            incoming_ = 0;
        }

        constexpr void setPoorSwarm()
        {
            good_ = 10;
            incoming_ = 1;
        }

        struct Searched
        {
            tr_sha1_digest_t info_hash;
            tr_port port;
            int af;
        };

        struct Pinged
        {
            tr_socket_address addrport;
            time_t timestamp;
        };

        int good_ = 0;
        int dubious_ = 0;
        int cached_ = 0;
        int incoming_ = 0;
        size_t n_periodic_calls_ = 0;
        bool inited_ = false;
        std::vector<Pinged> pinged_;
        std::vector<Searched> searched_;
        std::array<char, IdLength> id_ = {};
        tr_socket_t dht_socket_ = TR_BAD_SOCKET;
        tr_socket_t dht_socket6_ = TR_BAD_SOCKET;
    };

    // Creates real timers, but with shortened intervals so that tests can run faster
    class MockTimer final : public libtransmission::Timer
    {
    public:
        explicit MockTimer(std::unique_ptr<Timer> real_timer)
            : real_timer_{ std::move(real_timer) }
        {
        }

        void stop() override
        {
            real_timer_->stop();
        }

        void set_callback(std::function<void()> callback) override
        {
            real_timer_->set_callback(std::move(callback));
        }

        void set_repeating(bool repeating = true) override
        {
            real_timer_->set_repeating(repeating);
        }

        void set_interval(std::chrono::milliseconds /*interval*/) override
        {
            real_timer_->set_interval(MockTimerInterval);
        }

        void start() override
        {
            real_timer_->start();
        }

        [[nodiscard]] std::chrono::milliseconds interval() const noexcept override
        {
            return real_timer_->interval();
        }

        [[nodiscard]] bool is_repeating() const noexcept override
        {
            return real_timer_->is_repeating();
        }

    private:
        std::unique_ptr<Timer> const real_timer_;
    };

    // Creates MockTimers
    class MockTimerMaker final : public libtransmission::TimerMaker
    {
    public:
        explicit MockTimerMaker(struct event_base* evb)
            : real_timer_maker_{ evb }
        {
        }

        [[nodiscard]] std::unique_ptr<Timer> create() override
        {
            return std::make_unique<MockTimer>(real_timer_maker_.create());
        }

        EvTimerMaker real_timer_maker_;
    };

    class MockMediator final : public tr_dht::Mediator
    {
    public:
        explicit MockMediator(struct event_base* event_base)
            : mock_timer_maker_{ event_base }
        {
        }

        [[nodiscard]] std::vector<tr_torrent_id_t> torrents_allowing_dht() const override
        {
            return torrents_allowing_dht_;
        }

        [[nodiscard]] tr_sha1_digest_t torrent_info_hash(tr_torrent_id_t id) const override
        {
            if (auto const iter = info_hashes_.find(id); iter != std::end(info_hashes_))
            {
                return iter->second;
            }

            return {};
        }

        [[nodiscard]] std::string_view config_dir() const override
        {
            return config_dir_;
        }

        [[nodiscard]] libtransmission::TimerMaker& timer_maker() override
        {
            return mock_timer_maker_;
        }

        [[nodiscard]] tr_dht::API& api() override
        {
            return mock_dht_;
        }

        void add_pex(tr_sha1_digest_t const& /*info_hash*/, tr_pex const* /*pex*/, size_t /*n_pex*/) override
        {
        }

        std::string config_dir_;
        std::vector<tr_torrent_id_t> torrents_allowing_dht_;
        std::map<tr_torrent_id_t, tr_sha1_digest_t> info_hashes_;
        MockDht mock_dht_;
        MockTimerMaker mock_timer_maker_;
    };

    [[nodiscard]] static tr_socket_address getSockaddr(std::string_view name, tr_port port)
    {
        auto hints = addrinfo{};
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_family = AF_UNSPEC;

        auto const szname = tr_urlbuf{ name };
        auto const port_str = std::to_string(port.host());
        addrinfo* info = nullptr;
        if (int const rc = getaddrinfo(szname.c_str(), std::data(port_str), &hints, &info); rc != 0)
        {
            tr_logAddWarn(fmt::format(
                _("Couldn't look up '{address}:{port}': {error} ({error_code})"),
                fmt::arg("address", name),
                fmt::arg("port", port.host()),
                fmt::arg("error", gai_strerror(rc)),
                fmt::arg("error_code", rc)));
            return {};
        }

        auto opt = tr_socket_address::from_sockaddr(info->ai_addr);
        freeaddrinfo(info);
        if (opt)
        {
            return *opt;
        }

        return {};
    }

    void SetUp() override
    {
        SandboxedTest::SetUp();

        init_mgr_ = tr_lib_init();

        tr_session_thread::tr_evthread_init();
        event_base_ = event_base_new();
    }

    void TearDown() override
    {
        event_base_free(event_base_);
        event_base_ = nullptr;

        SandboxedTest::TearDown();
    }

    struct event_base* event_base_ = nullptr;

    std::unique_ptr<tr_net_init_mgr> init_mgr_;

    // Arbitrary values. Several tests requires socket/port values
    // to be provided but they aren't central to the tests, so they're
    // declared here with "Arbitrary" in the name to make that clear.
    static auto constexpr ArbitrarySock4 = tr_socket_t{ 404 };
    static auto constexpr ArbitrarySock6 = tr_socket_t{ 418 };
    static auto constexpr ArbitraryPeerPort = tr_port::from_host(909);
};

TEST_F(DhtTest, initsWithCorrectSockets)
{
    static auto constexpr Sock4 = tr_socket_t{ 1000 };
    static auto constexpr Sock6 = tr_socket_t{ 2000 };

    // Make the DHT
    auto mediator = MockMediator{ event_base_ };
    mediator.config_dir_ = sandboxDir();
    auto dht = tr_dht::create(mediator, ArbitraryPeerPort, Sock4, Sock6);

    // Confirm that dht_init() was called with the right sockets
    EXPECT_EQ(Sock4, mediator.mock_dht_.dht_socket_);
    EXPECT_EQ(Sock6, mediator.mock_dht_.dht_socket6_);
}

TEST_F(DhtTest, callsUninitOnDestruct)
{
    auto mediator = MockMediator{ event_base_ };
    mediator.config_dir_ = sandboxDir();
    EXPECT_FALSE(mediator.mock_dht_.inited_);

    {
        auto dht = tr_dht::create(mediator, ArbitraryPeerPort, ArbitrarySock4, ArbitrarySock6);
        EXPECT_TRUE(mediator.mock_dht_.inited_);

        // dht goes out-of-scope here
    }

    EXPECT_FALSE(mediator.mock_dht_.inited_);
}

TEST_F(DhtTest, loadsStateFromStateFile)
{
    auto const state_file = MockStateFile{};
    state_file.save(sandboxDir());

    // Make the DHT
    auto mediator = MockMediator{ event_base_ };
    mediator.config_dir_ = sandboxDir();
    auto dht = tr_dht::create(mediator, ArbitraryPeerPort, ArbitrarySock4, ArbitrarySock6);

    // Wait for all the state nodes to be pinged
    auto& pinged = mediator.mock_dht_.pinged_;
    auto const n_expected_nodes = std::size(state_file.ipv4_nodes_) + std::size(state_file.ipv6_nodes_);
    waitFor(event_base_, [&pinged, n_expected_nodes]() { return std::size(pinged) >= n_expected_nodes; });
    auto actual_nodes_str = std::string{};
    for (auto const& [addrport, timestamp] : pinged)
    {
        actual_nodes_str += addrport.display_name();
        actual_nodes_str += ',';
    }

    /// Confirm that the state was loaded

    // dht_init() should have been called with the state file's id
    EXPECT_EQ(state_file.id_, mediator.mock_dht_.id_);

    // dht_ping_nodedht_init() should have been called with state file's nodes
    EXPECT_EQ(state_file.nodesString(), actual_nodes_str);
}

TEST_F(DhtTest, stopsBootstrappingWhenSwarmHealthIsGoodEnough)
{
    auto const state_file = MockStateFile{};
    state_file.save(sandboxDir());

    // Make the DHT
    auto mediator = MockMediator{ event_base_ };
    mediator.config_dir_ = sandboxDir();
    auto dht = tr_dht::create(mediator, ArbitraryPeerPort, ArbitrarySock4, ArbitrarySock6);

    // Wait for N pings to occur...
    auto& mock_dht = mediator.mock_dht_;
    static auto constexpr TurnGoodAfterNthPing = size_t{ 3 };
    waitFor(event_base_, [&mock_dht]() { return std::size(mock_dht.pinged_) == TurnGoodAfterNthPing; });
    EXPECT_EQ(TurnGoodAfterNthPing, std::size(mock_dht.pinged_));

    // Now fake that libdht says the swarm is healthy.
    // This should cause bootstrapping to end.
    mock_dht.setHealthySwarm();

    // Now test to see if bootstrapping is done.
    // There's not public API for `isBootstrapping()`,
    // so to test this we just a moment to confirm that no more bootstrap nodes are pinged.
    waitFor(event_base_, MockTimerInterval * 10);

    // Confirm that the number of nodes pinged is unchanged,
    // indicating that bootstrapping is done
    EXPECT_EQ(TurnGoodAfterNthPing, std::size(mock_dht.pinged_));
}

TEST_F(DhtTest, savesStateIfSwarmIsGood)
{
    auto const state_file = MockStateFile{};
    auto const dat_file = MockStateFile::filename(sandboxDir());
    EXPECT_FALSE(tr_sys_path_exists(dat_file.c_str()));

    {
        auto mediator = MockMediator{ event_base_ };
        mediator.config_dir_ = sandboxDir();
        mediator.mock_dht_.setHealthySwarm();

        auto dht = tr_dht::create(mediator, ArbitraryPeerPort, ArbitrarySock4, ArbitrarySock6);

        // as dht goes out of scope,
        // it should save its state if the swarm is healthy
        EXPECT_FALSE(tr_sys_path_exists(dat_file.c_str()));
    }

    EXPECT_TRUE(tr_sys_path_exists(dat_file.c_str()));
}

TEST_F(DhtTest, doesNotSaveStateIfSwarmIsBad)
{
    auto const state_file = MockStateFile{};
    auto const dat_file = MockStateFile::filename(sandboxDir());
    EXPECT_FALSE(tr_sys_path_exists(dat_file.c_str()));

    {
        auto mediator = MockMediator{ event_base_ };
        mediator.config_dir_ = sandboxDir();
        mediator.mock_dht_.setPoorSwarm();

        auto dht = tr_dht::create(mediator, ArbitraryPeerPort, ArbitrarySock4, ArbitrarySock6);

        // as dht goes out of scope,
        // it should save its state if the swarm is healthy
        EXPECT_FALSE(tr_sys_path_exists(dat_file.c_str()));
    }

    EXPECT_FALSE(tr_sys_path_exists(dat_file.c_str()));
}

TEST_F(DhtTest, usesBootstrapFile)
{
    // Make the 'dht.bootstrap' file.
    // This a file with each line holding `${host} ${port}`
    // which tr-dht will try to ping as nodes
    static auto constexpr BootstrapNodeName = "example.com"sv;
    static auto constexpr BootstrapNodePort = tr_port::from_host(8080);
    if (auto ofs = std::ofstream{ tr_pathbuf{ sandboxDir(), "/dht.bootstrap" } }; ofs)
    {
        ofs << BootstrapNodeName << ' ' << BootstrapNodePort.host() << std::endl;
        ofs.close();
    }

    // Make the DHT
    auto mediator = MockMediator{ event_base_ };
    mediator.config_dir_ = sandboxDir();
    auto dht = tr_dht::create(mediator, ArbitraryPeerPort, ArbitrarySock4, ArbitrarySock6);

    // We didn't create a 'dht.dat' file to load state from,
    // so 'dht.bootstrap' should be the first nodes in the bootstrap list.
    // Confirm that BootstrapNodeName gets pinged first.
    auto const expected = getSockaddr(BootstrapNodeName, BootstrapNodePort);
    auto& pinged = mediator.mock_dht_.pinged_;
    waitFor(
        event_base_,
        [&pinged]() { return !std::empty(pinged); },
        5s);
    ASSERT_EQ(1U, std::size(pinged));
    auto const [actual_addrport, time] = pinged.front();
    EXPECT_EQ(expected.address(), actual_addrport.address());
    EXPECT_EQ(expected.port(), actual_addrport.port());
    EXPECT_EQ(expected.display_name(), actual_addrport.display_name());
}

TEST_F(DhtTest, pingsAddedNodes)
{
    auto mediator = MockMediator{ event_base_ };
    mediator.config_dir_ = sandboxDir();
    auto dht = tr_dht::create(mediator, ArbitraryPeerPort, ArbitrarySock4, ArbitrarySock6);

    EXPECT_EQ(0U, std::size(mediator.mock_dht_.pinged_));

    auto const addr = tr_address::from_string("10.10.10.1");
    EXPECT_TRUE(addr.has_value());
    assert(addr.has_value());
    auto constexpr Port = tr_port::from_host(128);
    dht->add_node(*addr, Port);

    ASSERT_EQ(1U, std::size(mediator.mock_dht_.pinged_));
    EXPECT_EQ(addr, mediator.mock_dht_.pinged_.front().addrport.address());
    EXPECT_EQ(Port, mediator.mock_dht_.pinged_.front().addrport.port());
}

TEST_F(DhtTest, announcesTorrents)
{
    auto constexpr Id = tr_torrent_id_t{ 1 };
    auto constexpr PeerPort = tr_port::from_host(999);
    auto const info_hash = tr_rand_obj<tr_sha1_digest_t>();

    tr_timeUpdate(time(nullptr));

    auto mediator = MockMediator{ event_base_ };
    mediator.info_hashes_[Id] = info_hash;
    mediator.torrents_allowing_dht_ = { Id };
    mediator.config_dir_ = sandboxDir();

    // Since we're mocking a swarm that's magically healthy out-of-the-box,
    // the DHT object we create can skip bootstrapping and proceed straight
    // to announces
    auto& mock_dht = mediator.mock_dht_;
    mock_dht.setHealthySwarm();

    auto dht = tr_dht::create(mediator, PeerPort, ArbitrarySock4, ArbitrarySock6);

    waitFor(event_base_, MockTimerInterval * 10);

    ASSERT_EQ(2U, std::size(mock_dht.searched_));

    EXPECT_EQ(info_hash, mock_dht.searched_[0].info_hash);
    EXPECT_EQ(PeerPort, mock_dht.searched_[0].port);
    EXPECT_EQ(AF_INET, mock_dht.searched_[0].af);

    EXPECT_EQ(info_hash, mock_dht.searched_[1].info_hash);
    EXPECT_EQ(PeerPort, mock_dht.searched_[1].port);
    EXPECT_EQ(AF_INET6, mock_dht.searched_[1].af);
}

TEST_F(DhtTest, callsPeriodicPeriodically)
{
    auto mediator = MockMediator{ event_base_ };
    mediator.config_dir_ = sandboxDir();
    auto dht = tr_dht::create(mediator, ArbitraryPeerPort, ArbitrarySock4, ArbitrarySock6);

    auto& mock_dht = mediator.mock_dht_;
    auto const baseline = mock_dht.n_periodic_calls_;
    static auto constexpr Periods = 10;
    waitFor(event_base_, std::chrono::duration_cast<std::chrono::milliseconds>(MockTimerInterval * Periods));
    EXPECT_NEAR(mock_dht.n_periodic_calls_, baseline + Periods, Periods / 2.0);
}

} // namespace libtransmission::test
