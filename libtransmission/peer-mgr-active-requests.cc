// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cstddef> // size_t
#include <ctime>
#include <memory>
#include <utility>
#include <unordered_map>
#include <vector>

#include <small/map.hpp>

#define LIBTRANSMISSION_PEER_MODULE

#include "libtransmission/transmission.h"

#include "libtransmission/peer-mgr-active-requests.h"
#include "libtransmission/peer-mgr-wishlist.h"
#include "libtransmission/tr-assert.h"

struct tr_peer;

class ActiveRequests::Impl
{
public:
    [[nodiscard]] size_t size() const
    {
        return size_;
    }

    [[nodiscard]] size_t count(tr_peer const* peer) const
    {
        auto const it = count_.find(peer);
        return it != std::end(count_) ? it->second : size_t{};
    }

    void incCount(tr_peer const* peer)
    {
        ++count_[peer];
        ++size_;
    }

    void decCount(tr_peer const* peer)
    {
        auto it = count_.find(peer);
        TR_ASSERT(it != std::end(count_));
        TR_ASSERT(it->second > 0);
        TR_ASSERT(size_ > 0);

        if (it != std::end(count_))
        {
            if (--it->second == 0)
            {
                count_.erase(it);
            }
            --size_;
        }
    }

    std::unordered_map<tr_peer const*, size_t> count_;

    std::unordered_map<tr_block_index_t, small::map<tr_peer const*, time_t, Wishlist::EndgameMaxPeers>> blocks_;

private:
    size_t size_ = 0;
};

ActiveRequests::ActiveRequests()
    : impl_{ std::make_unique<Impl>() }
{
}

ActiveRequests::~ActiveRequests() = default;

bool ActiveRequests::add(tr_block_index_t block, tr_peer* peer, time_t when)
{
    bool const added = impl_->blocks_[block].emplace(peer, when).second;

    if (added)
    {
        impl_->incCount(peer);
    }

    return added;
}

// remove a request to `peer` for `block`
bool ActiveRequests::remove(tr_block_index_t block, tr_peer const* peer)
{
    auto const it = impl_->blocks_.find(block);
    auto const removed = it != std::end(impl_->blocks_) && it->second.erase(peer) != 0;

    if (removed)
    {
        impl_->decCount(peer);

        if (std::empty(it->second))
        {
            impl_->blocks_.erase(it);
        }
    }

    return removed;
}

// remove requests to `peer` and return the associated blocks
std::vector<tr_block_index_t> ActiveRequests::remove(tr_peer const* peer)
{
    auto removed = std::vector<tr_block_index_t>{};
    removed.reserve(impl_->blocks_.size());

    for (auto const& [block, peers_at] : impl_->blocks_)
    {
        if (peers_at.contains(peer))
        {
            removed.push_back(block);
        }
    }

    for (auto block : removed)
    {
        remove(block, peer);
    }

    return removed;
}

// remove requests for `block` and return the associated peers
std::vector<tr_peer*> ActiveRequests::remove(tr_block_index_t block)
{
    auto removed = std::vector<tr_peer*>{};

    if (auto it = impl_->blocks_.find(block); it != std::end(impl_->blocks_))
    {
        auto const n = std::size(it->second);
        removed.resize(n);
        std::transform(
            std::begin(it->second),
            std::end(it->second),
            std::begin(removed),
            [](auto const& iter) { return const_cast<tr_peer*>(iter.first); });
        impl_->blocks_.erase(block);
    }

    for (auto const* const peer : removed)
    {
        impl_->decCount(peer);
    }

    return removed;
}

// return true if there's an active request to `peer` for `block`
bool ActiveRequests::has(tr_block_index_t block, tr_peer const* peer) const
{
    auto const iter = impl_->blocks_.find(block);
    return iter != std::end(impl_->blocks_) && iter->second.contains(peer);
}

// count how many peers we're asking for `block`
size_t ActiveRequests::count(tr_block_index_t block) const
{
    auto const& blocks = impl_->blocks_;
    auto const iter = blocks.find(block);
    return iter == std::end(blocks) ? 0U : std::size(iter->second);
}

// count how many active block requests we have to `peer`
size_t ActiveRequests::count(tr_peer const* peer) const
{
    return impl_->count(peer);
}

// return the total number of active requests
size_t ActiveRequests::size() const
{
    return impl_->size();
}

// returns the active requests sent before `when`
std::vector<std::pair<tr_block_index_t, tr_peer*>> ActiveRequests::sentBefore(time_t when) const
{
    auto sent_before = std::vector<std::pair<tr_block_index_t, tr_peer*>>{};
    sent_before.reserve(std::size(impl_->blocks_));

    for (auto const& [block, peers_at] : impl_->blocks_)
    {
        for (auto const& [peer, sent_at] : peers_at)
        {
            if (sent_at < when)
            {
                sent_before.emplace_back(block, const_cast<tr_peer*>(peer));
            }
        }
    }

    return sent_before;
}
