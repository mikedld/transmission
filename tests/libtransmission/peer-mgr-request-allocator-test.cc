// This file Copyright (C) 2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#define LIBTRANSMISSION_PEER_MODULE

#include "transmission.h"

#include "bandwidth.h"
#include "peer-mgr-request-allocator.h"
#include "peer-msgs.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

using namespace std::literals;

using RequestAllocatorTest = ::testing::Test;

namespace
{

using PeerKey = std::string_view;
using PoolKey = std::string_view;
using RequestAllocator = BlockRequestAllocator<PeerKey, PoolKey>;

PoolKey constexpr SessionPool = "session-pool"sv;
PoolKey constexpr Torrent1Pool = "torrent-1-pool"sv;
PoolKey constexpr Torrent2Pool = "torrent-2-pool"sv;

PeerKey constexpr Torrent1PeerA = "torrent-1-peer-a"sv;
PeerKey constexpr Torrent1PeerB = "torrent-1-peer-b"sv;
PeerKey constexpr Torrent2PeerA = "torrent-2-peer-a"sv;
PeerKey constexpr Torrent2PeerB = "torrent-2-peer-b"sv;

class MockMediator final : public RequestAllocator::Mediator
{
public:
    virtual ~MockMediator() = default;

    [[nodiscard]] std::vector<PeerKey> peers() const override
    {
        auto peer_keys = std::vector<PeerKey>{};
        peer_keys.reserve(std::size(peer_to_pools_));
        for (auto const& [peer_key, pools] : peer_to_pools_)
        {
            peer_keys.push_back(peer_key);
        }
        return peer_keys;
    }

    [[nodiscard]] size_t activeReqCount(PeerKey peer) const override
    {
        return pending_reqs_.at(peer);
    }

    [[nodiscard]] std::vector<PoolKey> pools(PeerKey peer) const override
    {
        return peer_to_pools_.at(peer);
    }

    [[nodiscard]] size_t poolBlockLimit(PoolKey pool) const override
    {
        return pool_limit_.at(pool);
    }

    [[nodiscard]] uint32_t maxObservedDlSpeedBps() const override
    {
        return max_observed_dl_speed_Bps_;
    }

    [[nodiscard]] size_t downloadReqPeriod() const override
    {
        return download_req_period_;
    }

    [[nodiscard]] std::optional<size_t> maxActiveRequests(PeerKey peer) const noexcept override
    {
        if (auto const iter = reqq_.find(peer); iter != std::end(reqq_))
        {
            return iter->second;
        }

        return std::nullopt;
    }

    template<typename... PoolKeys>
    void addPeer(PeerKey peer_key, size_t pending_reqs, PoolKeys... pool_keys)
    {
        (peer_to_pools_[peer_key].push_back(pool_keys), ...);
        pending_reqs_.try_emplace(peer_key, pending_reqs);
    }

    void setPoolLimit(PoolKey key, size_t n)
    {
        pool_limit_.try_emplace(key, n);
    }

    void setMaxObservedDlSpeedBps(uint32_t bytes_per_second)
    {
        max_observed_dl_speed_Bps_ = bytes_per_second;
    }

    void setDownloadReqPeriod(size_t secs)
    {
        download_req_period_ = secs;
    }

    void setReqQ(PeerKey peer, std::optional<size_t> reqq)
    {
        reqq_[peer] = reqq;
    }

private:
    std::map<PeerKey, std::vector<PoolKey>> peer_to_pools_;
    std::map<PeerKey, std::optional<size_t>> reqq_;
    std::map<PeerKey, size_t> pending_reqs_;
    std::map<PoolKey, size_t> pool_limit_;
    size_t download_req_period_ = 10U;
    uint32_t max_observed_dl_speed_Bps_ = 0;
};

} // namespace

TEST_F(RequestAllocatorTest, distributesEvenlyWhenAllElseIsEqual)
{
    auto mediator = MockMediator();
    mediator.addPeer(Torrent1PeerA, 0, SessionPool);
    mediator.addPeer(Torrent1PeerB, 0, SessionPool);
    mediator.addPeer(Torrent2PeerA, 0, SessionPool);
    mediator.setPoolLimit(SessionPool, std::numeric_limits<size_t>::max());

    static auto constexpr AllocCount = 12;
    static auto constexpr EvenSplit = AllocCount / 3;
    auto allocation = RequestAllocator::allocateBlockReqs(mediator, AllocCount);
    std::sort(std::begin(allocation), std::end(allocation));
    auto const expected = std::vector<std::pair<PeerKey, size_t>>{ std::make_pair(Torrent1PeerA, EvenSplit),
                                                                   std::make_pair(Torrent1PeerB, EvenSplit),
                                                                   std::make_pair(Torrent2PeerA, EvenSplit) };
    EXPECT_EQ(expected, allocation);
}

TEST_F(RequestAllocatorTest, honorsSpeedLimits)
{
    auto mediator = MockMediator();
    mediator.setPoolLimit(SessionPool, 100);
    mediator.setPoolLimit(Torrent1Pool, 50);
    mediator.addPeer(Torrent1PeerA, 0, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent1PeerB, 0, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent2PeerA, 0, SessionPool); // no per-torrent speed limit for torrent 2

    auto allocation = RequestAllocator::allocateBlockReqs(mediator, 1000);
    std::sort(std::begin(allocation), std::end(allocation));
    auto const expected = std::vector<std::pair<PeerKey, size_t>>{ std::make_pair(Torrent1PeerA, 25),
                                                                   std::make_pair(Torrent1PeerB, 25),
                                                                   std::make_pair(Torrent2PeerA, 50) };
    EXPECT_EQ(expected, allocation);
}

TEST_F(RequestAllocatorTest, considersBacklog)
{
    auto mediator = MockMediator();
    mediator.setPoolLimit(SessionPool, 100);
    mediator.setPoolLimit(Torrent1Pool, 50);
    mediator.addPeer(Torrent1PeerA, 10, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent1PeerB, 0, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent2PeerA, 0, SessionPool); // no per-torrent speed limit for torrent 2

    auto allocation = RequestAllocator::allocateBlockReqs(mediator, 1000);
    std::sort(std::begin(allocation), std::end(allocation));
    auto const expected = std::vector<std::pair<PeerKey, size_t>>{ std::make_pair(Torrent1PeerA, 15),
                                                                   std::make_pair(Torrent1PeerB, 25),
                                                                   std::make_pair(Torrent2PeerA, 50) };
    EXPECT_EQ(expected, allocation);
}

TEST_F(RequestAllocatorTest, stopsWhenOutOfReqs)
{
    auto mediator = MockMediator();
    mediator.setPoolLimit(Torrent1Pool, 50);
    mediator.setPoolLimit(SessionPool, 100);
    mediator.addPeer(Torrent1PeerA, 10, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent1PeerB, 0, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent2PeerA, 0, SessionPool); // no per-torrent speed limit for torrent 2

    auto allocation = RequestAllocator::allocateBlockReqs(mediator, 23);
    std::sort(std::begin(allocation), std::end(allocation));
    auto const expected = std::vector<std::pair<PeerKey, size_t>>{ std::make_pair(Torrent1PeerA, 1),
                                                                   std::make_pair(Torrent1PeerB, 11),
                                                                   std::make_pair(Torrent2PeerA, 11) };
    EXPECT_EQ(expected, allocation);
}

TEST_F(RequestAllocatorTest, doesNothingWhenNoReqs)
{
    auto mediator = MockMediator();
    mediator.setPoolLimit(Torrent1Pool, 50);
    mediator.setPoolLimit(SessionPool, 100);
    mediator.addPeer(Torrent1PeerA, 10, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent1PeerB, 0, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent2PeerA, 0, SessionPool);

    auto const allocation = RequestAllocator::allocateBlockReqs(mediator, 0);
    auto const expected = std::vector<std::pair<PeerKey, size_t>>{};
    EXPECT_EQ(expected, allocation);
}

TEST_F(RequestAllocatorTest, allocatesEvenly1)
{
    auto mediator = MockMediator();
    mediator.setPoolLimit(Torrent1Pool, 50);
    mediator.setPoolLimit(Torrent2Pool, 100);
    mediator.setPoolLimit(SessionPool, 200);
    mediator.addPeer(Torrent1PeerA, 10, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent1PeerB, 5, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent2PeerA, 20, Torrent2Pool, SessionPool);
    mediator.addPeer(Torrent2PeerB, 15, Torrent2Pool, SessionPool);

    auto allocation = RequestAllocator::allocateBlockReqs(mediator, 1000);
    std::sort(std::begin(allocation), std::end(allocation));
    auto const expected = std::vector<std::pair<PeerKey, size_t>>{ std::make_pair(Torrent1PeerA, 15),
                                                                   std::make_pair(Torrent1PeerB, 20),
                                                                   std::make_pair(Torrent2PeerA, 30),
                                                                   std::make_pair(Torrent2PeerB, 35) };

    EXPECT_EQ(expected, allocation);
}

TEST_F(RequestAllocatorTest, allocatesEvenly2)
{
    auto mediator = MockMediator();
    mediator.setPoolLimit(Torrent1Pool, 50);
    mediator.setPoolLimit(Torrent2Pool, 500);
    mediator.setPoolLimit(SessionPool, 200);
    mediator.addPeer(Torrent1PeerA, 10, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent1PeerB, 5, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent2PeerA, 20, Torrent2Pool, SessionPool);
    mediator.addPeer(Torrent2PeerB, 15, Torrent2Pool, SessionPool);

    auto allocation = RequestAllocator::allocateBlockReqs(mediator, 1000);
    std::sort(std::begin(allocation), std::end(allocation));
    auto const expected = std::vector<std::pair<PeerKey, size_t>>{ std::make_pair(Torrent1PeerA, 15),
                                                                   std::make_pair(Torrent1PeerB, 20),
                                                                   std::make_pair(Torrent2PeerA, 55),
                                                                   std::make_pair(Torrent2PeerB, 60) };
    EXPECT_EQ(expected, allocation);
}

TEST_F(RequestAllocatorTest, reqQAffectsCount)
{
    auto mediator = MockMediator();
    mediator.setPoolLimit(Torrent1Pool, 50);
    mediator.setPoolLimit(Torrent2Pool, 500);
    mediator.setPoolLimit(SessionPool, 200);
    mediator.addPeer(Torrent1PeerA, 10, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent1PeerB, 5, Torrent1Pool, SessionPool);
    mediator.addPeer(Torrent2PeerA, 20, Torrent2Pool, SessionPool);
    mediator.addPeer(Torrent2PeerB, 15, Torrent2Pool, SessionPool);

    mediator.setReqQ(Torrent2PeerB, 40);

    auto allocation = RequestAllocator::allocateBlockReqs(mediator, 1000);
    std::sort(std::begin(allocation), std::end(allocation));
    auto const expected = std::vector<std::pair<PeerKey, size_t>>{ std::make_pair(Torrent1PeerA, 15),
                                                                   std::make_pair(Torrent1PeerB, 20),
                                                                   std::make_pair(Torrent2PeerA, 90),
                                                                   std::make_pair(Torrent2PeerB, 25) };
    EXPECT_EQ(expected, allocation);
}

TEST_F(RequestAllocatorTest, activeReqsReduceNewReqCount)
{
    static auto constexpr ActiveReqs = size_t{ 10U };

    // count how many reqs we could make if there are no active reqs
    auto mediator = MockMediator();
    auto const baseline = RequestAllocator::decideHowManyNewReqsToSend(mediator);
    EXPECT_GT(baseline, 0U);

    // now add some active reqs and recount
    mediator.addPeer(Torrent1PeerA, ActiveReqs, SessionPool);
    auto const actual = RequestAllocator::decideHowManyNewReqsToSend(mediator);

    // we should be able to send `ActiveReqs` fewer requests
    EXPECT_EQ(actual + ActiveReqs, baseline);
}

TEST_F(RequestAllocatorTest, periodReqPeriodAffectsReqCount)
{
    // count how many reqs we could make if there are no active reqs
    auto mediator = MockMediator();
    auto period_secs = size_t{ 10U };
    mediator.setDownloadReqPeriod(period_secs);
    auto const baseline = RequestAllocator::decideHowManyNewReqsToSend(mediator);
    EXPECT_GT(baseline, 0U);

    // now double the period and recount
    mediator.setDownloadReqPeriod(period_secs * 2U);
    auto const actual = RequestAllocator::decideHowManyNewReqsToSend(mediator);
    EXPECT_NEAR(baseline * 2U, actual, 1);
}
