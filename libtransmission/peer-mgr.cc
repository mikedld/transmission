// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno> /* error codes ERANGE, ... */
#include <chrono>
#include <climits> /* INT_MAX */
#include <cmath>
#include <cstdint>
#include <ctime> // time_t
#include <deque>
#include <map>
#include <iterator> // std::back_inserter
#include <memory>
#include <optional>
#include <tuple> // std::tie
#include <utility>
#include <vector>

#include <fmt/format.h>

#define LIBTRANSMISSION_PEER_MODULE
#include "transmission.h"

#include "announcer.h"
#include "bandwidth.h"
#include "blocklist.h"
#include "cache.h"
#include "clients.h"
#include "completion.h"
#include "crypto-utils.h"
#include "handshake.h"
#include "log.h"
#include "net.h"
#include "peer-io.h"
#include "peer-mgr-active-requests.h"
#include "peer-mgr-wishlist.h"
#include "peer-mgr.h"
#include "peer-msgs.h"
#include "session.h"
#include "timer.h"
#include "torrent.h"
#include "tr-assert.h"
#include "tr-utp.h"
#include "utils.h"
#include "webseed.h"

using namespace std::literals;

// use for bitwise operations w/peer_atom.flags2
static auto constexpr MyflagBanned = int{ 1 };

// use for bitwise operations w/peer_atom.flags2
// unreachable for now... but not banned.
// if they try to connect to us it's okay
static auto constexpr MyflagUnreachable = int{ 2 };

static auto constexpr CancelHistorySec = int{ 60 };

// ---

class HandshakeMediator final : public tr_handshake::Mediator
{
private:
    [[nodiscard]] static std::optional<TorrentInfo> torrent(tr_torrent* tor)
    {
        if (tor == nullptr)
        {
            return {};
        }

        auto info = TorrentInfo{};
        info.info_hash = tor->infoHash();
        info.client_peer_id = tr_torrentGetPeerId(tor);
        info.id = tor->id();
        info.is_done = tor->isDone();
        return info;
    }

public:
    explicit HandshakeMediator(tr_session& session) noexcept
        : session_{ session }
    {
    }

    [[nodiscard]] std::optional<TorrentInfo> torrent(tr_sha1_digest_t const& info_hash) const override
    {
        return torrent(session_.torrents().get(info_hash));
    }

    [[nodiscard]] std::optional<TorrentInfo> torrent_from_obfuscated(tr_sha1_digest_t const& info_hash) const override
    {
        return torrent(tr_torrentFindFromObfuscatedHash(&session_, info_hash));
    }

    [[nodiscard]] bool allows_dht() const override
    {
        return session_.allowsDHT();
    }

    [[nodiscard]] bool allows_tcp() const override
    {
        return session_.allowsTCP();
    }

    void set_utp_failed(tr_sha1_digest_t const& info_hash, tr_address const& addr) override
    {
        if (auto* const tor = session_.torrents().get(info_hash); tor != nullptr)
        {
            tr_peerMgrSetUtpFailed(tor, addr, true);
        }
    }

    [[nodiscard]] bool is_peer_known_seed(tr_torrent_id_t tor_id, tr_address const& addr) const override;

    [[nodiscard]] libtransmission::TimerMaker& timer_maker() override
    {
        return session_.timerMaker();
    }

    [[nodiscard]] size_t pad(void* setme, size_t maxlen) const override
    {
        auto const len = tr_rand_int(maxlen);
        tr_rand_buffer(setme, len);
        return len;
    }

private:
    tr_session& session_;
};

/**
 * Peer information that should be kept even before we've connected and
 * after we've disconnected. These are kept in a pool of peer_atoms to decide
 * which ones would make good candidates for connecting to, and to watch out
 * for banned peers.
 *
 * @see tr_peer
 * @see tr_peerMsgs
 */
struct peer_atom
{
    peer_atom(tr_address addr_in, tr_port port_in, uint8_t flags_in, uint8_t from)
        : addr{ addr_in }
        , port{ port_in }
        , fromFirst{ from }
        , fromBest{ from }
        , flags{ flags_in }
    {
        ++n_atoms;
    }

    peer_atom(peer_atom&&) = delete;
    peer_atom(peer_atom const&) = delete;
    peer_atom& operator=(peer_atom&&) = delete;
    peer_atom& operator=(peer_atom const&) = delete;

    ~peer_atom()
    {
        [[maybe_unused]] auto const n_prev = n_atoms--;
        TR_ASSERT(n_prev > 0U);
    }

    [[nodiscard]] static auto atom_count() noexcept
    {
        return n_atoms.load();
    }

    [[nodiscard]] constexpr auto isSeed() const noexcept
    {
        return (flags & ADDED_F_SEED_FLAG) != 0;
    }

    [[nodiscard]] auto display_name() const
    {
        return addr.display_name(port);
    }

    [[nodiscard]] bool isBlocklisted(tr_session const* session) const
    {
        if (blocklisted_)
        {
            return *blocklisted_;
        }

        auto const value = session->addressIsBlocked(addr);
        blocklisted_ = value;
        return value;
    }

    [[nodiscard]] constexpr int getReconnectIntervalSecs(time_t const now) const noexcept
    {
        auto sec = int{};
        bool const unreachable = (this->flags2 & MyflagUnreachable) != 0;

        /* if we were recently connected to this peer and transferring piece
         * data, try to reconnect to them sooner rather that later -- we don't
         * want network troubles to get in the way of a good peer. */
        if (!unreachable && now - this->piece_data_time <= MinimumReconnectIntervalSecs * 2)
        {
            sec = MinimumReconnectIntervalSecs;
        }
        /* otherwise, the interval depends on how many times we've tried
         * and failed to connect to the peer */
        else
        {
            auto step = this->num_fails;

            /* penalize peers that were unreachable the last time we tried */
            if (unreachable)
            {
                step += 2;
            }

            switch (step)
            {
            case 0:
                sec = 0;
                break;

            case 1:
                sec = 10;
                break;

            case 2:
                sec = 60 * 2;
                break;

            case 3:
                sec = 60 * 15;
                break;

            case 4:
                sec = 60 * 30;
                break;

            case 5:
                sec = 60 * 60;
                break;

            default:
                sec = 60 * 120;
                break;
            }
        }

        return sec;
    }

    void setBlocklistedDirty()
    {
        blocklisted_.reset();
    }

    [[nodiscard]] constexpr std::optional<bool> isReachable() const
    {
        if ((flags2 & MyflagUnreachable) != 0)
        {
            return false;
        }

        if ((flags & ADDED_F_CONNECTABLE) != 0)
        {
            return true;
        }

        return std::nullopt;
    }

    tr_address const addr;

    tr_port port = {};

    uint16_t num_fails = {};

    time_t time = {}; /* when the peer's connection status last changed */
    time_t piece_data_time = {};

    time_t lastConnectionAttemptAt = {};
    time_t lastConnectionAt = {};

    uint8_t const fromFirst; /* where the peer was first found */
    uint8_t fromBest; /* the "best" value of where the peer has been found */
    uint8_t flags = {}; /* these match the added_f flags */
    uint8_t flags2 = {}; /* flags that aren't defined in added_f */

    bool utp_failed = false; /* We recently failed to connect over µTP */
    bool is_connected = false;

private:
    mutable std::optional<bool> blocklisted_;

    // the minimum we'll wait before attempting to reconnect to a peer
    static auto constexpr MinimumReconnectIntervalSecs = int{ 5 };

    static auto inline n_atoms = std::atomic<size_t>{};
};

using Handshakes = std::map<tr_address, tr_handshake>;

#define tr_logAddDebugSwarm(swarm, msg) tr_logAddDebugTor((swarm)->tor, msg)
#define tr_logAddTraceSwarm(swarm, msg) tr_logAddTraceTor((swarm)->tor, msg)

/** @brief Opaque, per-torrent data structure for peer connection information */
class tr_swarm
{
public:
    [[nodiscard]] auto unique_lock() const
    {
        return tor->unique_lock();
    }

    tr_swarm(tr_peerMgr* manager_in, tr_torrent* tor_in) noexcept
        : manager{ manager_in }
        , tor{ tor_in }
    {
        rebuildWebseeds();
    }

    tr_swarm(tr_swarm&&) = delete;
    tr_swarm(tr_swarm const&) = delete;
    tr_swarm& operator=(tr_swarm&&) = delete;
    tr_swarm& operator=(tr_swarm const&) = delete;

    ~tr_swarm()
    {
        auto const lock = unique_lock();
        TR_ASSERT(!is_running);
        TR_ASSERT(std::empty(outgoing_handshakes));
        TR_ASSERT(std::empty(peers));
    }

    [[nodiscard]] bool peer_is_in_use(peer_atom const& atom) const;

    void cancelOldRequests()
    {
        auto const now = tr_time();
        auto const oldest = now - RequestTtlSecs;

        for (auto const& [block, peer] : active_requests.sentBefore(oldest))
        {
            maybeSendCancelRequest(peer, block, nullptr);
            active_requests.remove(block, peer);
        }
    }

    void cancelAllRequestsForBlock(tr_block_index_t block, tr_peer const* no_notify)
    {
        for (auto* peer : active_requests.remove(block))
        {
            maybeSendCancelRequest(peer, block, no_notify);
        }
    }

    [[nodiscard]] uint16_t countActiveWebseeds(uint64_t now) const noexcept
    {
        if (!tor->isRunning || tor->isDone())
        {
            return {};
        }

        return std::count_if(
            std::begin(webseeds),
            std::end(webseeds),
            [&now](auto const& webseed) { return webseed->isTransferringPieces(now, TR_DOWN, nullptr); });
    }

    [[nodiscard]] auto peerCount() const noexcept
    {
        return std::size(peers);
    }

    void stop()
    {
        auto const lock = unique_lock();

        is_running = false;
        removeAllPeers();
        outgoing_handshakes.clear();
    }

    void removePeer(tr_peer* peer)
    {
        auto const lock = unique_lock();

        auto* const atom = peer->atom;
        TR_ASSERT(atom != nullptr);

        atom->time = tr_time();

        if (auto iter = std::find(std::begin(peers), std::end(peers), peer); iter != std::end(peers))
        {
            peers.erase(iter);
        }

        --stats.peer_count;
        --stats.peer_from_count[atom->fromFirst];

        TR_ASSERT(stats.peer_count == peerCount());

        delete peer;
    }

    void removeAllPeers()
    {
        auto tmp = peers;

        for (auto* peer : tmp)
        {
            removePeer(peer);
        }

        TR_ASSERT(stats.peer_count == 0);
    }

    void updateEndgame()
    {
        /* we consider ourselves to be in endgame if the number of bytes
           we've got requested is >= the number of bytes left to download */
        is_endgame_ = uint64_t(std::size(active_requests)) * tr_block_info::BlockSize >= tor->leftUntilDone();
    }

    [[nodiscard]] constexpr auto isEndgame() const noexcept
    {
        return is_endgame_;
    }

    void addStrike(tr_peer* peer) const
    {
        tr_logAddTraceSwarm(
            this,
            fmt::format("increasing peer {} strike count to {}", peer->display_name(), peer->strikes + 1));

        if (++peer->strikes >= MaxBadPiecesPerPeer)
        {
            peer->atom->flags2 |= MyflagBanned;
            peer->do_purge = true;
            tr_logAddTraceSwarm(this, fmt::format("banning peer {}", peer->display_name()));
        }
    }

    void rebuildWebseeds()
    {
        auto const n = tor->webseedCount();

        webseeds.clear();
        webseeds.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            webseeds.emplace_back(tr_webseedNew(tor, tor->webseed(i), &tr_peer::clientCallback));
        }
        webseeds.shrink_to_fit();

        stats.active_webseed_count = 0;
    }

    [[nodiscard]] TR_CONSTEXPR20 auto isAllSeeds() const noexcept
    {
        if (!pool_is_all_seeds_)
        {
            pool_is_all_seeds_ = std::all_of(std::begin(pool), std::end(pool), [](auto const& atom) { return atom.isSeed(); });
        }

        return *pool_is_all_seeds_;
    }

    void markAllSeedsFlagDirty() noexcept
    {
        pool_is_all_seeds_.reset();
    }

    [[nodiscard]] peer_atom* get_existing_atom(tr_address const& addr) noexcept
    {
        auto const iter = std::find_if(
            std::begin(pool),
            std::end(pool),
            [&addr](auto const& atom) { return atom.addr == addr; });
        return iter != std::end(pool) ? &*iter : nullptr;
    }

    [[nodiscard]] peer_atom const* get_existing_atom(tr_address const& addr) const noexcept
    {
        auto const iter = std::find_if(
            std::begin(pool),
            std::end(pool),
            [&addr](auto const& atom) { return atom.addr == addr; });
        return iter != std::end(pool) ? &*iter : nullptr;
    }

    [[nodiscard]] bool peer_is_a_seed(tr_address const& addr) const noexcept
    {
        auto const* const atom = get_existing_atom(addr);
        return atom != nullptr && atom->isSeed();
    }

    peer_atom* ensure_atom_exists(tr_address const& addr, tr_port const port, uint8_t const flags, uint8_t const from)
    {
        TR_ASSERT(addr.is_valid());
        TR_ASSERT(from < TR_PEER_FROM__MAX);

        peer_atom* atom = get_existing_atom(addr);

        if (atom == nullptr)
        {
            atom = &pool.emplace_back(addr, port, flags, from);
        }
        else
        {
            atom->fromBest = std::min(atom->fromBest, from);
            atom->flags |= flags;
        }

        markAllSeedsFlagDirty();

        return atom;
    }

    void mark_atom_as_seed(peer_atom& atom)
    {
        tr_logAddTraceSwarm(this, fmt::format("marking peer {} as a seed", atom.display_name()));
        atom.flags |= ADDED_F_SEED_FLAG;
        markAllSeedsFlagDirty();
    }

    Handshakes outgoing_handshakes;

    uint16_t interested_count = 0;
    uint16_t max_peers = 0;

    mutable tr_swarm_stats stats = {};

    uint8_t optimistic_unchoke_time_scaler = 0;

    bool is_running = false;

    tr_peerMgr* const manager;

    tr_torrent* const tor;

    std::vector<std::unique_ptr<tr_peer>> webseeds;
    std::vector<tr_peerMsgs*> peers;

    // tr_peers hold pointers to the items in this container,
    // so use a deque instead of vector to prevent insertion from
    // invalidating those pointers
    std::deque<peer_atom> pool;

    tr_peerMsgs* optimistic = nullptr; /* the optimistic peer, or nullptr if none */

    time_t lastCancel = 0;

    ActiveRequests active_requests;

private:
    static void maybeSendCancelRequest(tr_peer* peer, tr_block_index_t block, tr_peer const* muted)
    {
        auto* msgs = dynamic_cast<tr_peerMsgs*>(peer);
        if (msgs != nullptr && msgs != muted)
        {
            peer->cancels_sent_to_peer.add(tr_time(), 1);
            msgs->cancel_block_request(block);
        }
    }

    // number of bad pieces a peer is allowed to send before we ban them
    static auto constexpr MaxBadPiecesPerPeer = int{ 5 };

    // how long we'll let requests we've made linger before we cancel them
    static auto constexpr RequestTtlSecs = int{ 90 };

    mutable std::optional<bool> pool_is_all_seeds_;

    bool is_endgame_ = false;
};

struct tr_peerMgr
{
    explicit tr_peerMgr(tr_session* session_in)
        : session{ session_in }
        , handshake_mediator_{ *session }
        , bandwidth_timer_{ session->timerMaker().create([this]() { bandwidthPulse(); }) }
        , rechoke_timer_{ session->timerMaker().create([this]() { rechokePulseMarshall(); }) }
        , refill_upkeep_timer_{ session->timerMaker().create([this]() { refillUpkeep(); }) }
    {
        bandwidth_timer_->startRepeating(BandwidthPeriod);
        rechoke_timer_->startRepeating(RechokePeriod);
        refill_upkeep_timer_->startRepeating(RefillUpkeepPeriod);
    }

    tr_peerMgr(tr_peerMgr&&) = delete;
    tr_peerMgr(tr_peerMgr const&) = delete;
    tr_peerMgr& operator=(tr_peerMgr&&) = delete;
    tr_peerMgr& operator=(tr_peerMgr const&) = delete;

    [[nodiscard]] auto unique_lock() const
    {
        return session->unique_lock();
    }

    ~tr_peerMgr()
    {
        auto const lock = unique_lock();
        incoming_handshakes.clear();
    }

    void rechokeSoon() noexcept
    {
        rechoke_timer_->setInterval(100ms);
    }

    void bandwidthPulse();
    void rechokePulse() const;
    void reconnectPulse();
    void refillUpkeep() const;
    void makeNewPeerConnections(size_t max);

    [[nodiscard]] tr_swarm* get_existing_swarm(tr_sha1_digest_t const& hash) const
    {
        auto* const tor = session->torrents().get(hash);
        return tor == nullptr ? nullptr : tor->swarm;
    }

    tr_session* const session;
    Handshakes incoming_handshakes;

    HandshakeMediator handshake_mediator_;

private:
    void rechokePulseMarshall()
    {
        rechokePulse();
        rechoke_timer_->setInterval(RechokePeriod);
    }

    std::unique_ptr<libtransmission::Timer> const bandwidth_timer_;
    std::unique_ptr<libtransmission::Timer> const rechoke_timer_;
    std::unique_ptr<libtransmission::Timer> const refill_upkeep_timer_;

    static auto constexpr BandwidthPeriod = 500ms;
    static auto constexpr RechokePeriod = 10s;
    static auto constexpr RefillUpkeepPeriod = 10s;

    // how frequently to decide which peers live and die
    static auto constexpr ReconnectPeriodMsec = int{ 500 };

    // max number of peers to ask for per second overall.
    // this throttle is to avoid overloading the router
    static auto constexpr MaxConnectionsPerSecond = size_t{ 12 };
};

// --- tr_peer virtual functions

tr_peer::tr_peer(tr_torrent* tor, peer_atom* atom_in)
    : torrent{ tor }
    , atom{ atom_in }
    , blame{ tor->blockCount() }
{
}

tr_peer::~tr_peer()
{
    if (auto* const swarm = getSwarm(); swarm != nullptr)
    {
        swarm->active_requests.remove(this);
    }

    if (atom != nullptr)
    {
        atom->is_connected = false;
    }
}

void tr_peer::clientCallback(tr_peer_event const& event)
{
    TR_ASSERT(torrent != nullptr);
    auto* const swarm = getSwarm();
    auto const lock = torrent->unique_lock();

    switch (event.type)
    {
    case tr_peer_event::Type::ClientSentPieceData:
        {
            auto const now = tr_time();

            torrent->uploadedCur += event.length;
            tr_announcerAddBytes(torrent, TR_ANN_UP, event.length);
            torrent->setDateActive(now);
            torrent->setDirty();
            torrent->session->addUploaded(event.length);

            if (atom != nullptr)
            {
                atom->piece_data_time = now;
            }
            break;
        }

    case tr_peer_event::Type::ClientGotPieceData:
        {
            auto const now = tr_time();

            torrent->downloadedCur += event.length;
            torrent->setDateActive(now);
            torrent->setDirty();
            torrent->session->addDownloaded(event.length);

            if (atom != nullptr)
            {
                atom->piece_data_time = now;
            }
            break;
        }

    case tr_peer_event::Type::ClientGotHave:
    case tr_peer_event::Type::ClientGotHaveAll:
    case tr_peer_event::Type::ClientGotHaveNone:
    case tr_peer_event::Type::ClientGotBitfield:
        /* TODO: if we don't need these, should these events be removed? */
        /* noop */
        break;

    case tr_peer_event::Type::ClientGotRej:
        swarm->active_requests.remove(torrent->pieceLoc(event.pieceIndex, event.offset).block, this);
        break;

    case tr_peer_event::Type::ClientGotChoke:
        swarm->active_requests.remove(this);
        break;

    case tr_peer_event::Type::ClientGotPort:
        if (atom != nullptr)
        {
            atom->port = event.port;
        }
        break;

    case tr_peer_event::Type::ClientGotSuggest:
    case tr_peer_event::Type::ClientGotAllowedFast:
        // not currently supported
        break;

    case tr_peer_event::Type::ClientGotBlock:
        {
            auto const loc = torrent->pieceLoc(event.pieceIndex, event.offset);
            swarm->cancelAllRequestsForBlock(loc.block, this);
            blocks_sent_to_client.add(tr_time(), 1);
            tr_torrentGotBlock(torrent, loc.block);
            break;
        }

    case tr_peer_event::Type::Error:
        if (event.err == ERANGE || event.err == EMSGSIZE || event.err == ENOTCONN)
        {
            /* some protocol error from the peer */
            do_purge = true;
            tr_logAddDebugSwarm(
                swarm,
                fmt::format("setting {} do_purge flag because we got an ERANGE, EMSGSIZE, or ENOTCONN error", display_name()));
        }
        else
        {
            tr_logAddDebugSwarm(swarm, fmt::format("unhandled error: {}", tr_strerror(event.err)));
        }
        break;
    }
}

// ---

tr_peerMgr* tr_peerMgrNew(tr_session* session)
{
    return new tr_peerMgr{ session };
}

void tr_peerMgrFree(tr_peerMgr* manager)
{
    delete manager;
}

// ---

void tr_peerMgrOnBlocklistChanged(tr_peerMgr* mgr)
{
    /* we cache whether or not a peer is blocklisted...
       since the blocklist has changed, erase that cached value */
    for (auto* const tor : mgr->session->torrents())
    {
        for (auto& atom : tor->swarm->pool)
        {
            atom.setBlocklistedDirty();
        }
    }
}

// ---

void tr_peerMgrSetUtpSupported(tr_torrent* tor, tr_address const& addr)
{
    if (auto* const atom = tor->swarm->get_existing_atom(addr); atom != nullptr)
    {
        atom->flags |= ADDED_F_UTP_FLAGS;
    }
}

void tr_peerMgrSetUtpFailed(tr_torrent* tor, tr_address const& addr, bool failed)
{
    if (auto* const atom = tor->swarm->get_existing_atom(addr); atom != nullptr)
    {
        atom->utp_failed = failed;
    }
}

/**
 * REQUESTS
 *
 * There are two data structures associated with managing block requests:
 * 
 * 1. tr_swarm::active_requests, an opaque class that tracks what requests
 *    we currently have, i.e. which blocks and from which peers.
 *    This is used for cancelling requests that have been waiting
 *    for too long and avoiding duplicate requests.
 *
 * 2. tr_swarm::pieces, an array of "struct weighted_piece" which lists the
 *    pieces that we want to request. It's used to decide which blocks to
 *    return next when tr_peerMgrGetBlockRequests() is called.
 */

// --- struct block_request

// TODO: if we keep this, add equivalent API to ActiveRequest
void tr_peerMgrClientSentRequests(tr_torrent* torrent, tr_peer* peer, tr_block_span_t span)
{
    auto const now = tr_time();

    for (tr_block_index_t block = span.begin; block < span.end; ++block)
    {
        torrent->swarm->active_requests.add(block, peer, now);
    }
}

std::vector<tr_block_span_t> tr_peerMgrGetNextRequests(tr_torrent* torrent, tr_peer const* peer, size_t numwant)
{
    class MediatorImpl final : public Wishlist::Mediator
    {
    public:
        MediatorImpl(tr_torrent const* torrent_in, tr_peer const* peer_in)
            : torrent_{ torrent_in }
            , swarm_{ torrent_in->swarm }
            , peer_{ peer_in }
        {
        }

        MediatorImpl(MediatorImpl&&) = delete;
        MediatorImpl(MediatorImpl const&) = delete;
        MediatorImpl& operator=(MediatorImpl&&) = delete;
        MediatorImpl& operator=(MediatorImpl const&) = delete;

        ~MediatorImpl() override = default;

        [[nodiscard]] bool clientCanRequestBlock(tr_block_index_t block) const override
        {
            return !torrent_->hasBlock(block) && !swarm_->active_requests.has(block, peer_);
        }

        [[nodiscard]] bool clientCanRequestPiece(tr_piece_index_t piece) const override
        {
            return torrent_->pieceIsWanted(piece) && peer_->hasPiece(piece);
        }

        [[nodiscard]] bool isEndgame() const override
        {
            return swarm_->isEndgame();
        }

        [[nodiscard]] size_t countActiveRequests(tr_block_index_t block) const override
        {
            return swarm_->active_requests.count(block);
        }

        [[nodiscard]] size_t countMissingBlocks(tr_piece_index_t piece) const override
        {
            return torrent_->countMissingBlocksInPiece(piece);
        }

        [[nodiscard]] tr_block_span_t blockSpan(tr_piece_index_t piece) const override
        {
            return torrent_->blockSpanForPiece(piece);
        }

        [[nodiscard]] tr_piece_index_t countAllPieces() const override
        {
            return torrent_->pieceCount();
        }

        [[nodiscard]] tr_priority_t priority(tr_piece_index_t piece) const override
        {
            return torrent_->piecePriority(piece);
        }

    private:
        tr_torrent const* const torrent_;
        tr_swarm const* const swarm_;
        tr_peer const* const peer_;
    };

    torrent->swarm->updateEndgame();
    return Wishlist::next(MediatorImpl(torrent, peer), numwant);
}

// --- Piece List Manipulation / Accessors

bool tr_peerMgrDidPeerRequest(tr_torrent const* tor, tr_peer const* peer, tr_block_index_t block)
{
    return tor->swarm->active_requests.has(block, peer);
}

size_t tr_peerMgrCountActiveRequestsToPeer(tr_torrent const* tor, tr_peer const* peer)
{
    return tor->swarm->active_requests.count(peer);
}

void tr_peerMgr::refillUpkeep() const
{
    auto const lock = unique_lock();

    for (auto* const tor : session->torrents())
    {
        tor->swarm->cancelOldRequests();
    }
}

void tr_peerMgrPieceCompleted(tr_torrent* tor, tr_piece_index_t p)
{
    bool piece_came_from_peers = false;

    for (auto* const peer : tor->swarm->peers)
    {
        // notify the peer that we now have this piece
        peer->on_piece_completed(p);

        if (!piece_came_from_peers)
        {
            piece_came_from_peers = peer->blame.test(p);
        }
    }

    if (piece_came_from_peers) /* webseed downloads don't belong in announce totals */
    {
        tr_announcerAddBytes(tor, TR_ANN_DOWN, tor->pieceSize(p));
    }

    // bookkeeping
    tor->set_needs_completeness_check();
}

namespace
{
namespace handshake_helpers
{
void create_bit_torrent_peer(tr_torrent* tor, std::shared_ptr<tr_peerIo> io, struct peer_atom* atom, tr_quark client)
{
    TR_ASSERT(atom != nullptr);
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm != nullptr);

    tr_swarm* swarm = tor->swarm;

    auto* peer = tr_peerMsgsNew(tor, atom, std::move(io), &tr_peer::clientCallback);
    peer->client = client;
    atom->is_connected = true;

    swarm->peers.push_back(peer);

    ++swarm->stats.peer_count;
    ++swarm->stats.peer_from_count[atom->fromFirst];

    TR_ASSERT(swarm->stats.peer_count == swarm->peerCount());
    TR_ASSERT(swarm->stats.peer_from_count[atom->fromFirst] <= swarm->stats.peer_count);

    // TODO is this needed?
    // isn't it already initialized in tr_peerMsgsImpl's ctor?
    peer->update_active(TR_UP);
    peer->update_active(TR_DOWN);
}

/* FIXME: this is kind of a mess. */
[[nodiscard]] bool on_handshake_done(tr_peerMgr* manager, tr_handshake::Result const& result)
{
    TR_ASSERT(result.io != nullptr);

    bool const ok = result.is_connected;
    bool success = false;

    auto* const s = manager->get_existing_swarm(result.io->torrent_hash());

    auto const [addr, port] = result.io->socket_address();

    if (result.io->is_incoming())
    {
        manager->incoming_handshakes.erase(addr);
    }
    else if (s != nullptr)
    {
        s->outgoing_handshakes.erase(addr);
    }

    auto const lock = manager->unique_lock();

    if (!ok || s == nullptr || !s->is_running)
    {
        if (s != nullptr)
        {
            struct peer_atom* atom = s->get_existing_atom(addr);

            if (atom != nullptr)
            {
                ++atom->num_fails;

                if (!result.read_anything_from_peer)
                {
                    tr_logAddTraceSwarm(
                        s,
                        fmt::format(
                            "marking peer {} as unreachable... num_fails is {}",
                            atom->display_name(),
                            atom->num_fails));
                    atom->flags2 |= MyflagUnreachable;
                }
            }
        }
    }
    else /* looking good */
    {
        struct peer_atom* atom = s->ensure_atom_exists(addr, port, 0, TR_PEER_FROM_INCOMING);

        atom->time = tr_time();
        atom->piece_data_time = 0;
        atom->lastConnectionAt = tr_time();

        if (!result.io->is_incoming())
        {
            atom->flags |= ADDED_F_CONNECTABLE;
            atom->flags2 &= ~MyflagUnreachable;
        }

        /* In principle, this flag specifies whether the peer groks µTP,
           not whether it's currently connected over µTP. */
        if (result.io->is_utp())
        {
            atom->flags |= ADDED_F_UTP_FLAGS;
        }

        if ((atom->flags2 & MyflagBanned) != 0)
        {
            tr_logAddTraceSwarm(s, fmt::format("banned peer {} tried to reconnect", atom->display_name()));
        }
        else if (result.io->is_incoming() && s->peerCount() >= s->tor->peerLimit())
        {
            /* too many peers already */
        }
        else if (atom->is_connected)
        {
            // we're already connected to this peer; do nothing
        }
        else
        {
            auto client = tr_quark{ TR_KEY_NONE };
            if (result.peer_id)
            {
                auto buf = std::array<char, 128>{};
                tr_clientForId(std::data(buf), sizeof(buf), *result.peer_id);
                client = tr_quark_new(std::data(buf));
            }

            result.io->set_bandwidth(&s->tor->bandwidth_);
            create_bit_torrent_peer(s->tor, result.io, atom, client);

            success = true;
        }
    }

    return success;
}
} // namespace handshake_helpers
} // namespace

void tr_peerMgrAddIncoming(tr_peerMgr* manager, tr_peer_socket&& socket)
{
    using namespace handshake_helpers;

    TR_ASSERT(manager->session != nullptr);
    auto const lock = manager->unique_lock();

    tr_session* session = manager->session;

    if (session->addressIsBlocked(socket.address()))
    {
        tr_logAddTrace(fmt::format("Banned IP address '{}' tried to connect to us", socket.display_name()));
        socket.close();
    }
    else if (manager->incoming_handshakes.count(socket.address()) != 0U)
    {
        socket.close();
    }
    else /* we don't have a connection to them yet... */
    {
        auto address = socket.address();
        manager->incoming_handshakes.try_emplace(
            address,
            &manager->handshake_mediator_,
            tr_peerIo::new_incoming(session, &session->top_bandwidth_, std::move(socket)),
            session->encryptionMode(),
            [manager](tr_handshake::Result const& result) { return on_handshake_done(manager, result); });
    }
}

void tr_peerMgrSetSwarmIsAllSeeds(tr_torrent* tor)
{
    auto const lock = tor->unique_lock();

    auto* const swarm = tor->swarm;

    for (auto& atom : swarm->pool)
    {
        swarm->mark_atom_as_seed(atom);
    }

    swarm->markAllSeedsFlagDirty();
}

size_t tr_peerMgrAddPex(tr_torrent* tor, uint8_t from, tr_pex const* pex, size_t n_pex)
{
    size_t n_used = 0;
    tr_swarm* s = tor->swarm;
    auto const lock = s->manager->unique_lock();

    for (tr_pex const* const end = pex + n_pex; pex != end; ++pex)
    {
        if (tr_isPex(pex) && /* safeguard against corrupt data */
            !s->manager->session->addressIsBlocked(pex->addr) && pex->is_valid_for_peers())
        {
            s->ensure_atom_exists(pex->addr, pex->port, pex->flags, from);
            ++n_used;
        }
    }

    return n_used;
}

std::vector<tr_pex> tr_pex::from_compact_ipv4(
    void const* compact,
    size_t compact_len,
    uint8_t const* added_f,
    size_t added_f_len)
{
    size_t const n = compact_len / 6;
    auto const* walk = static_cast<std::byte const*>(compact);
    auto pex = std::vector<tr_pex>(n);

    for (size_t i = 0; i < n; ++i)
    {
        std::tie(pex[i].addr, walk) = tr_address::from_compact_ipv4(walk);
        std::tie(pex[i].port, walk) = tr_port::fromCompact(walk);

        if (added_f != nullptr && n == added_f_len)
        {
            pex[i].flags = added_f[i];
        }
    }

    return pex;
}

std::vector<tr_pex> tr_pex::from_compact_ipv6(
    void const* compact,
    size_t compact_len,
    uint8_t const* added_f,
    size_t added_f_len)
{
    size_t const n = compact_len / 18;
    auto const* walk = static_cast<std::byte const*>(compact);
    auto pex = std::vector<tr_pex>(n);

    for (size_t i = 0; i < n; ++i)
    {
        std::tie(pex[i].addr, walk) = tr_address::from_compact_ipv6(walk);
        std::tie(pex[i].port, walk) = tr_port::fromCompact(walk);

        if (added_f != nullptr && n == added_f_len)
        {
            pex[i].flags = added_f[i];
        }
    }

    return pex;
}

// ---

void tr_peerMgrGotBadPiece(tr_torrent* tor, tr_piece_index_t piece_index)
{
    auto* const swarm = tor->swarm;
    auto const byte_count = tor->pieceSize(piece_index);

    for (auto* const peer : swarm->peers)
    {
        if (peer->blame.test(piece_index))
        {
            tr_logAddTraceSwarm(
                swarm,
                fmt::format(
                    "peer {} contributed to corrupt piece ({}); now has {} strikes",
                    peer->display_name(),
                    piece_index,
                    peer->strikes + 1));
            swarm->addStrike(peer);
        }
    }

    tr_announcerAddBytes(tor, TR_ANN_CORRUPT, byte_count);
}

namespace get_peers_helpers
{

/* better goes first */
struct CompareAtomsByUsefulness
{
    [[nodiscard]] constexpr static int compare(peer_atom const& a, peer_atom const& b) noexcept // <=>
    {
        if (a.piece_data_time != b.piece_data_time)
        {
            return a.piece_data_time > b.piece_data_time ? -1 : 1;
        }

        if (a.fromBest != b.fromBest)
        {
            return a.fromBest < b.fromBest ? -1 : 1;
        }

        if (a.num_fails != b.num_fails)
        {
            return a.num_fails < b.num_fails ? -1 : 1;
        }

        return 0;
    }

    [[nodiscard]] constexpr bool operator()(peer_atom const& a, peer_atom const& b) const noexcept
    {
        return compare(a, b) < 0;
    }

    [[nodiscard]] constexpr bool operator()(peer_atom const* a, peer_atom const* b) const noexcept
    {
        return compare(*a, *b) < 0;
    }
};

[[nodiscard]] bool isAtomInteresting(tr_torrent const* tor, peer_atom const& atom)
{
    if (tor->isDone() && atom.isSeed())
    {
        return false;
    }

    if (tor->swarm->peer_is_in_use(atom))
    {
        return true;
    }

    if (atom.isBlocklisted(tor->session))
    {
        return false;
    }

    if ((atom.flags2 & MyflagBanned) != 0)
    {
        return false;
    }

    return true;
}

} // namespace get_peers_helpers

std::vector<tr_pex> tr_peerMgrGetPeers(tr_torrent const* tor, uint8_t address_type, uint8_t list_mode, size_t max_peer_count)
{
    using namespace get_peers_helpers;

    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    TR_ASSERT(address_type == TR_AF_INET || address_type == TR_AF_INET6);
    TR_ASSERT(list_mode == TR_PEERS_CONNECTED || list_mode == TR_PEERS_INTERESTING);

    tr_swarm const* s = tor->swarm;

    // build a list of atoms

    auto atoms = std::vector<peer_atom const*>{};
    if (list_mode == TR_PEERS_CONNECTED) /* connected peers only */
    {
        atoms.reserve(s->peerCount());
        std::transform(
            std::begin(s->peers),
            std::end(s->peers),
            std::back_inserter(atoms),
            [](auto const* peer) { return peer->atom; });
    }
    else /* TR_PEERS_INTERESTING */
    {
        for (auto const& atom : s->pool)
        {
            if (isAtomInteresting(tor, atom))
            {
                atoms.push_back(&atom);
            }
        }
    }

    std::sort(std::begin(atoms), std::end(atoms), CompareAtomsByUsefulness{});

    // add the first N of them into our return list

    auto const n = std::min(std::size(atoms), max_peer_count);
    auto pex = std::vector<tr_pex>{};
    pex.reserve(n);

    for (size_t i = 0; i < std::size(atoms) && std::size(pex) < n; ++i)
    {
        auto const* const atom = atoms[i];

        if (atom->addr.type == address_type)
        {
            TR_ASSERT(atom->addr.is_valid());
            pex.emplace_back(atom->addr, atom->port, atom->flags);
        }
    }

    std::sort(std::begin(pex), std::end(pex));
    return pex;
}

void tr_peerMgrStartTorrent(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    tr_swarm* const swarm = tor->swarm;

    swarm->is_running = true;
    swarm->max_peers = tor->peerLimit();

    swarm->manager->rechokeSoon();
}

void tr_peerMgrStopTorrent(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->swarm->stop();
}

void tr_peerMgrAddTorrent(tr_peerMgr* manager, tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();
    TR_ASSERT(tor->swarm == nullptr);

    tor->swarm = new tr_swarm{ manager, tor };
}

void tr_peerMgrRemoveTorrent(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    tor->swarm->stop();
    delete tor->swarm;
    tor->swarm = nullptr;
}

void tr_peerMgrOnTorrentGotMetainfo(tr_torrent* tor)
{
    auto* const swarm = tor->swarm;

    /* the webseed list may have changed... */
    swarm->rebuildWebseeds();

    /* some peer_msgs' progress fields may not be accurate if we
       didn't have the metadata before now... so refresh them all... */
    for (auto* peer : swarm->peers)
    {
        peer->onTorrentGotMetainfo();

        if (peer->isSeed())
        {
            swarm->mark_atom_as_seed(*peer->atom);
        }
    }

    /* update the bittorrent peers' willingness... */
    for (auto* peer : swarm->peers)
    {
        peer->update_active(TR_UP);
        peer->update_active(TR_DOWN);
    }
}

int8_t tr_peerMgrPieceAvailability(tr_torrent const* tor, tr_piece_index_t piece)
{
    if (!tor->hasMetainfo())
    {
        return 0;
    }

    if (tor->isSeed() || tor->hasPiece(piece))
    {
        return -1;
    }

    auto const& peers = tor->swarm->peers;
    return std::count_if(std::begin(peers), std::end(peers), [piece](auto const* peer) { return peer->hasPiece(piece); });
}

void tr_peerMgrTorrentAvailability(tr_torrent const* tor, int8_t* tab, unsigned int n_tabs)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tab != nullptr);
    TR_ASSERT(n_tabs > 0);

    std::fill_n(tab, n_tabs, int8_t{});

    auto const interval = tor->pieceCount() / static_cast<float>(n_tabs);
    for (tr_piece_index_t i = 0; i < n_tabs; ++i)
    {
        auto const piece = static_cast<tr_piece_index_t>(i * interval);
        tab[i] = tr_peerMgrPieceAvailability(tor, piece);
    }
}

tr_swarm_stats tr_swarmGetStats(tr_swarm const* swarm)
{
    TR_ASSERT(swarm != nullptr);
    auto& stats = swarm->stats;
    stats.active_webseed_count = swarm->countActiveWebseeds(tr_time_msec());
    return stats;
}

void tr_swarmIncrementActivePeers(tr_swarm* swarm, tr_direction direction, bool is_active)
{
    int n = swarm->stats.active_peer_count[direction];

    if (is_active)
    {
        ++n;
    }
    else
    {
        --n;
    }

    TR_ASSERT(n >= 0);
    TR_ASSERT(n <= swarm->stats.peer_count);

    swarm->stats.active_peer_count[direction] = n;
}

/* count how many bytes we want that connected peers have */
uint64_t tr_peerMgrGetDesiredAvailable(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    // common shortcuts...

    if (!tor->isRunning || tor->isStopping || tor->isDone() || !tor->hasMetainfo())
    {
        return 0;
    }

    tr_swarm const* const swarm = tor->swarm;
    if (swarm == nullptr || swarm->peerCount() == 0U)
    {
        return 0;
    }

    auto available = swarm->peers.front()->has();
    for (auto const* const peer : swarm->peers)
    {
        available |= peer->has();
    }

    if (available.hasAll())
    {
        return tor->leftUntilDone();
    }

    auto desired_available = uint64_t{};

    for (tr_piece_index_t i = 0, n = tor->pieceCount(); i < n; ++i)
    {
        if (tor->pieceIsWanted(i) && available.test(i))
        {
            desired_available += tor->countMissingBytesInPiece(i);
        }
    }

    TR_ASSERT(desired_available <= tor->totalSize());
    return desired_available;
}

tr_webseed_view tr_peerMgrWebseed(tr_torrent const* tor, size_t i)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm != nullptr);
    size_t const n = std::size(tor->swarm->webseeds);
    TR_ASSERT(i < n);

    return i >= n ? tr_webseed_view{} : tr_webseedView(tor->swarm->webseeds[i].get());
}

namespace peer_stat_helpers
{

[[nodiscard]] auto getPeerStats(tr_peerMsgs const* peer, time_t now, uint64_t now_msec)
{
    auto stats = tr_peer_stat{};
    auto const* const atom = peer->atom;

    auto const [addr, port] = peer->socketAddress();

    addr.display_name(stats.addr, sizeof(stats.addr));
    stats.client = peer->client.c_str();
    stats.port = port.host();
    stats.from = atom->fromFirst;
    stats.progress = peer->percentDone();
    stats.isUTP = peer->is_utp_connection();
    stats.isEncrypted = peer->is_encrypted();
    stats.rateToPeer_KBps = tr_toSpeedKBps(peer->get_piece_speed_bytes_per_second(now_msec, TR_CLIENT_TO_PEER));
    stats.rateToClient_KBps = tr_toSpeedKBps(peer->get_piece_speed_bytes_per_second(now_msec, TR_PEER_TO_CLIENT));
    stats.peerIsChoked = peer->is_peer_choked();
    stats.peerIsInterested = peer->is_peer_interested();
    stats.clientIsChoked = peer->is_client_choked();
    stats.clientIsInterested = peer->is_client_interested();
    stats.isIncoming = peer->is_incoming_connection();
    stats.isDownloadingFrom = peer->is_active(TR_PEER_TO_CLIENT);
    stats.isUploadingTo = peer->is_active(TR_CLIENT_TO_PEER);
    stats.isSeed = peer->isSeed();

    stats.blocksToPeer = peer->blocks_sent_to_peer.count(now, CancelHistorySec);
    stats.blocksToClient = peer->blocks_sent_to_client.count(now, CancelHistorySec);
    stats.cancelsToPeer = peer->cancels_sent_to_peer.count(now, CancelHistorySec);
    stats.cancelsToClient = peer->cancels_sent_to_client.count(now, CancelHistorySec);

    stats.activeReqsToPeer = peer->activeReqCount(TR_CLIENT_TO_PEER);
    stats.activeReqsToClient = peer->activeReqCount(TR_PEER_TO_CLIENT);

    char* pch = stats.flagStr;

    if (stats.isUTP)
    {
        *pch++ = 'T';
    }

    if (peer->getSwarm()->optimistic == peer)
    {
        *pch++ = 'O';
    }

    if (stats.isDownloadingFrom)
    {
        *pch++ = 'D';
    }
    else if (stats.clientIsInterested)
    {
        *pch++ = 'd';
    }

    if (stats.isUploadingTo)
    {
        *pch++ = 'U';
    }
    else if (stats.peerIsInterested)
    {
        *pch++ = 'u';
    }

    if (!stats.clientIsChoked && !stats.clientIsInterested)
    {
        *pch++ = 'K';
    }

    if (!stats.peerIsChoked && !stats.peerIsInterested)
    {
        *pch++ = '?';
    }

    if (stats.isEncrypted)
    {
        *pch++ = 'E';
    }

    if (stats.from == TR_PEER_FROM_DHT)
    {
        *pch++ = 'H';
    }
    else if (stats.from == TR_PEER_FROM_PEX)
    {
        *pch++ = 'X';
    }

    if (stats.isIncoming)
    {
        *pch++ = 'I';
    }

    *pch = '\0';

    return stats;
}

} // namespace peer_stat_helpers

tr_peer_stat* tr_peerMgrPeerStats(tr_torrent const* tor, size_t* setme_count)
{
    using namespace peer_stat_helpers;

    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm->manager != nullptr);

    auto const n = tor->swarm->peerCount();
    auto* const ret = new tr_peer_stat[n];

    auto const now = tr_time();
    auto const now_msec = tr_time_msec();
    std::transform(
        std::begin(tor->swarm->peers),
        std::end(tor->swarm->peers),
        ret,
        [&now, &now_msec](auto const* peer) { return getPeerStats(peer, now, now_msec); });

    *setme_count = n;
    return ret;
}

void tr_peerMgrClearInterest(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    auto& peers = tor->swarm->peers;
    std::for_each(std::begin(peers), std::end(peers), [](auto* const peer) { peer->set_interested(false); });
}

namespace rechoke_downloads_helpers
{
namespace
{

/* does this peer have any pieces that we want? */
[[nodiscard]] bool isPeerInteresting(
    tr_torrent const* const tor,
    std::vector<bool> const& piece_is_interesting,
    tr_peerMsgs const* const peer)
{
    /* these cases should have already been handled by the calling code... */
    TR_ASSERT(!tor->isDone());
    TR_ASSERT(tor->clientCanDownload());

    if (peer->isSeed())
    {
        return true;
    }

    for (tr_piece_index_t i = 0; i < tor->pieceCount(); ++i)
    {
        if (piece_is_interesting[i] && peer->hasPiece(i))
        {
            return true;
        }
    }

    return false;
}

enum tr_rechoke_state
{
    RECHOKE_STATE_GOOD,
    RECHOKE_STATE_UNTESTED,
    RECHOKE_STATE_BAD
};

struct tr_rechoke_info
{
    tr_rechoke_info(tr_peerMsgs* peer_in, int rechoke_state_in, uint8_t salt_in)
        : peer{ peer_in }
        , rechoke_state{ rechoke_state_in }
        , salt{ salt_in }
    {
    }

    [[nodiscard]] constexpr auto compare(tr_rechoke_info const& that) const noexcept // <=>
    {
        if (this->rechoke_state != that.rechoke_state)
        {
            return this->rechoke_state - that.rechoke_state;
        }

        if (this->salt != that.salt)
        {
            return this->salt < that.salt ? -1 : 1;
        }

        return 0;
    }

    [[nodiscard]] constexpr auto operator<(tr_rechoke_info const& that) const noexcept
    {
        return compare(that) < 0;
    }

    tr_peerMsgs* peer;
    int rechoke_state;
    uint8_t salt;
};

} // namespace

/* determines who we send "interested" messages to */
void rechokeDownloads(tr_swarm* s)
{
    static auto constexpr MinInterestingPeers = uint16_t{ 5 };

    auto const peer_count = s->peerCount();
    auto const& peers = s->peers;
    auto const now = tr_time();

    uint16_t max_peers = 0;
    auto rechoke = std::vector<tr_rechoke_info>{};
    auto salter = tr_salt_shaker{};

    /* some cases where this function isn't necessary */
    if (s->tor->isDone() || !s->tor->clientCanDownload())
    {
        return;
    }

    /* decide HOW MANY peers to be interested in */
    {
        int blocks = 0;
        int cancels = 0;

        /* Count up how many blocks & cancels each peer has.
         *
         * There are two situations where we send out cancels --
         *
         * 1. We've got unresponsive peers, which is handled by deciding
         *    -which- peers to be interested in.
         *
         * 2. We've hit our bandwidth cap, which is handled by deciding
         *    -how many- peers to be interested in.
         *
         * We're working on 2. here, so we need to ignore unresponsive
         * peers in our calculations lest they confuse Transmission into
         * thinking it's hit its bandwidth cap.
         */
        for (auto const* const peer : peers)
        {
            auto const b = peer->blocks_sent_to_client.count(now, CancelHistorySec);

            if (b == 0) /* ignore unresponsive peers, as described above */
            {
                continue;
            }

            blocks += b;
            cancels += peer->cancels_sent_to_peer.count(now, CancelHistorySec);
        }

        if (cancels > 0)
        {
            /* cancel_rate: of the block requests we've recently made, the percentage we cancelled.
             * higher values indicate more congestion. */
            double const cancel_rate = cancels / (double)(cancels + blocks);
            double const mult = 1 - std::min(cancel_rate, 0.5);
            max_peers = s->interested_count * mult;
            tr_logAddTraceSwarm(
                s,
                fmt::format(
                    "cancel rate is {} -- reducing the number of peers we're interested in by {} percent",
                    cancel_rate,
                    mult * 100));
            s->lastCancel = now;
        }

        time_t const time_since_cancel = now - s->lastCancel;

        if (time_since_cancel != 0)
        {
            int const max_increase = 15;
            time_t const max_history = 2 * CancelHistorySec;
            double const mult = std::min(time_since_cancel, max_history) / static_cast<double>(max_history);
            int const inc = max_increase * mult;
            max_peers = s->max_peers + inc;
            tr_logAddTraceSwarm(
                s,
                fmt::format(
                    "time since last cancel is {} -- increasing the number of peers we're interested in by {}",
                    time_since_cancel,
                    inc));
        }
    }

    /* don't let the previous section's number tweaking go too far... */
    max_peers = std::clamp(max_peers, MinInterestingPeers, s->tor->peerLimit());

    s->max_peers = max_peers;

    if (peer_count > 0)
    {
        rechoke.reserve(peer_count);

        auto const* const tor = s->tor;
        int const n = tor->pieceCount();

        /* build a bitfield of interesting pieces... */
        auto piece_is_interesting = std::vector<bool>{};
        piece_is_interesting.resize(n);

        for (int i = 0; i < n; ++i)
        {
            piece_is_interesting[i] = tor->pieceIsWanted(i) && !tor->hasPiece(i);
        }

        /* decide WHICH peers to be interested in (based on their cancel-to-block ratio) */
        for (auto* const peer : peers)
        {
            if (!isPeerInteresting(s->tor, piece_is_interesting, peer))
            {
                peer->set_interested(false);
            }
            else
            {
                auto rechoke_state = tr_rechoke_state{};
                auto const blocks = peer->blocks_sent_to_client.count(now, CancelHistorySec);
                auto const cancels = peer->cancels_sent_to_peer.count(now, CancelHistorySec);

                if (blocks == 0 && cancels == 0)
                {
                    rechoke_state = RECHOKE_STATE_UNTESTED;
                }
                else if (cancels == 0)
                {
                    rechoke_state = RECHOKE_STATE_GOOD;
                }
                else if (blocks == 0)
                {
                    rechoke_state = RECHOKE_STATE_BAD;
                }
                else if (cancels * 10 < blocks)
                {
                    rechoke_state = RECHOKE_STATE_GOOD;
                }
                else
                {
                    rechoke_state = RECHOKE_STATE_BAD;
                }

                rechoke.emplace_back(peer, rechoke_state, salter());
            }
        }
    }

    std::sort(std::begin(rechoke), std::end(rechoke));

    /* now that we know which & how many peers to be interested in... update the peer interest */

    s->interested_count = std::min(max_peers, static_cast<uint16_t>(std::size(rechoke)));

    for (size_t i = 0, n = std::size(rechoke); i < n; ++i)
    {
        rechoke[i].peer->set_interested(i < s->interested_count);
    }
}

} // namespace rechoke_downloads_helpers

// ---

namespace rechoke_uploads_helpers
{
namespace
{

struct ChokeData
{
    ChokeData(tr_peerMsgs* msgs_in, int rate_in, uint8_t salt_in, bool is_interested_in, bool was_choked_in, bool is_choked_in)
        : msgs{ msgs_in }
        , rate{ rate_in }
        , salt{ salt_in }
        , is_interested{ is_interested_in }
        , was_choked{ was_choked_in }
        , is_choked{ is_choked_in }
    {
    }

    tr_peerMsgs* msgs;
    int rate;
    uint8_t salt;
    bool is_interested;
    bool was_choked;
    bool is_choked;

    [[nodiscard]] constexpr auto compare(ChokeData const& that) const noexcept // <=>
    {
        if (this->rate != that.rate) // prefer higher overall speeds
        {
            return this->rate > that.rate ? -1 : 1;
        }

        if (this->was_choked != that.was_choked) // prefer unchoked
        {
            return this->was_choked ? 1 : -1;
        }

        if (this->salt != that.salt) // random order
        {
            return this->salt < that.salt ? -1 : 1;
        }

        return 0;
    }

    [[nodiscard]] constexpr auto operator<(ChokeData const& that) const noexcept
    {
        return compare(that) < 0;
    }
};

/* get a rate for deciding which peers to choke and unchoke. */
[[nodiscard]] auto getRateBps(tr_torrent const* tor, tr_peer const* peer, uint64_t now)
{
    if (tor->isDone())
    {
        return peer->get_piece_speed_bytes_per_second(now, TR_CLIENT_TO_PEER);
    }

    /* downloading a private torrent... take upload speed into account
     * because there may only be a small window of opportunity to share */
    if (tor->isPrivate())
    {
        return peer->get_piece_speed_bytes_per_second(now, TR_PEER_TO_CLIENT) +
            peer->get_piece_speed_bytes_per_second(now, TR_CLIENT_TO_PEER);
    }

    /* downloading a public torrent */
    return peer->get_piece_speed_bytes_per_second(now, TR_PEER_TO_CLIENT);
}

// an optimistically unchoked peer is immune from rechoking
// for this many calls to rechokeUploads().
auto constexpr OptimisticUnchokeMultiplier = uint8_t{ 4 };

} // namespace

void rechokeUploads(tr_swarm* s, uint64_t const now)
{
    auto const lock = s->unique_lock();

    auto const peer_count = s->peerCount();
    auto& peers = s->peers;
    auto choked = std::vector<ChokeData>{};
    choked.reserve(peer_count);
    auto const* const session = s->manager->session;
    bool const choke_all = !s->tor->clientCanUpload();
    bool const is_maxed_out = s->tor->bandwidth_.is_maxed_out(TR_UP, now);

    /* an optimistic unchoke peer's "optimistic"
     * state lasts for N calls to rechokeUploads(). */
    if (s->optimistic_unchoke_time_scaler > 0)
    {
        --s->optimistic_unchoke_time_scaler;
    }
    else
    {
        s->optimistic = nullptr;
    }

    /* sort the peers by preference and rate */
    auto salter = tr_salt_shaker{};
    for (auto* const peer : peers)
    {
        if (peer->isSeed())
        {
            /* choke seeds and partial seeds */
            peer->set_choke(true);
        }
        else if (choke_all)
        {
            /* choke everyone if we're not uploading */
            peer->set_choke(true);
        }
        else if (peer != s->optimistic)
        {
            choked.emplace_back(
                peer,
                getRateBps(s->tor, peer, now),
                salter(),
                peer->is_peer_interested(),
                peer->is_peer_choked(),
                true);
        }
    }

    std::sort(std::begin(choked), std::end(choked));

    /**
     * Reciprocation and number of uploads capping is managed by unchoking
     * the N peers which have the best upload rate and are interested.
     * This maximizes the client's download rate. These N peers are
     * referred to as downloaders, because they are interested in downloading
     * from the client.
     *
     * Peers which have a better upload rate (as compared to the downloaders)
     * but aren't interested get unchoked. If they become interested, the
     * downloader with the worst upload rate gets choked. If a client has
     * a complete file, it uses its upload rate rather than its download
     * rate to decide which peers to unchoke.
     *
     * If our bandwidth is maxed out, don't unchoke any more peers.
     */
    auto checked_choke_count = size_t{ 0U };
    auto unchoked_interested = size_t{ 0U };

    for (auto& item : choked)
    {
        if (unchoked_interested >= session->uploadSlotsPerTorrent())
        {
            break;
        }

        item.is_choked = is_maxed_out ? item.was_choked : false;

        ++checked_choke_count;

        if (item.is_interested)
        {
            ++unchoked_interested;
        }
    }

    /* optimistic unchoke */
    if (s->optimistic == nullptr && !is_maxed_out && checked_choke_count < std::size(choked))
    {
        auto rand_pool = std::vector<ChokeData*>{};

        for (auto i = checked_choke_count, n = std::size(choked); i < n; ++i)
        {
            if (choked[i].is_interested)
            {
                rand_pool.push_back(&choked[i]);
            }
        }

        if (auto const n = std::size(rand_pool); n != 0)
        {
            auto* c = rand_pool[tr_rand_int(n)];
            c->is_choked = false;
            s->optimistic = c->msgs;
            s->optimistic_unchoke_time_scaler = OptimisticUnchokeMultiplier;
        }
    }

    for (auto& item : choked)
    {
        item.msgs->set_choke(item.is_choked);
    }
}

} // namespace rechoke_uploads_helpers

void tr_peerMgr::rechokePulse() const
{
    using namespace rechoke_downloads_helpers;
    using namespace rechoke_uploads_helpers;

    auto const lock = unique_lock();
    auto const now = tr_time_msec();

    for (auto* const tor : session->torrents())
    {
        if (tor->isRunning)
        {
            // possibly stop torrents that have seeded enough
            tr_torrentCheckSeedLimit(tor);
        }

        if (tor->isRunning)
        {
            if (auto* const swarm = tor->swarm; swarm->stats.peer_count > 0)
            {
                rechokeUploads(swarm, now);
                rechokeDownloads(swarm);
            }
        }
    }
}

// --- Life and Death

namespace disconnect_helpers
{
namespace
{
// when many peers are available, keep idle ones this long
auto constexpr MinUploadIdleSecs = time_t{ 60 };

// when few peers are available, keep idle ones this long
auto constexpr MaxUploadIdleSecs = time_t{ 60 * 5 };

[[nodiscard]] bool shouldPeerBeClosed(tr_swarm const* s, tr_peerMsgs const* peer, size_t peer_count, time_t const now)
{
    /* if it's marked for purging, close it */
    if (peer->do_purge)
    {
        tr_logAddTraceSwarm(s, fmt::format("purging peer {} because its do_purge flag is set", peer->display_name()));
        return true;
    }

    auto const* tor = s->tor;
    auto const* const atom = peer->atom;

    /* disconnect if we're both seeds and enough time has passed for PEX */
    if (tor->isDone() && peer->isSeed())
    {
        return !tor->allowsPex() || now - atom->time >= 30;
    }

    /* disconnect if it's been too long since piece data has been transferred.
     * this is on a sliding scale based on number of available peers... */
    {
        auto const relax_strictness_if_fewer_than_n = static_cast<size_t>(std::lround(tor->peerLimit() * 0.9));
        /* if we have >= relaxIfFewerThan, strictness is 100%.
         * if we have zero connections, strictness is 0% */
        float const strictness = peer_count >= relax_strictness_if_fewer_than_n ?
            1.0 :
            peer_count / (float)relax_strictness_if_fewer_than_n;
        auto const lo = MinUploadIdleSecs;
        auto const hi = MaxUploadIdleSecs;
        time_t const limit = hi - (hi - lo) * strictness;
        time_t const idle_time = now - std::max(atom->time, atom->piece_data_time);

        if (idle_time > limit)
        {
            tr_logAddTraceSwarm(
                s,
                fmt::format(
                    "purging peer {} because it's been {} secs since we shared anything",
                    peer->display_name(),
                    idle_time));
            return true;
        }
    }

    return false;
}

void closePeer(tr_peer* peer)
{
    TR_ASSERT(peer != nullptr);
    auto* const swarm = peer->getSwarm();

    /* if we transferred piece data, then they might be good peers,
       so reset their `num_fails' weight to zero. otherwise we connected
       to them fruitlessly, so mark it as another fail */
    if (auto* const atom = peer->atom; atom->piece_data_time != 0)
    {
        tr_logAddTraceSwarm(swarm, fmt::format("resetting atom {} num_fails to 0", peer->display_name()));
        atom->num_fails = 0;
    }
    else
    {
        ++atom->num_fails;
        tr_logAddTraceSwarm(swarm, fmt::format("incremented atom {} num_fails to {}", peer->display_name(), atom->num_fails));
    }

    tr_logAddTraceSwarm(swarm, fmt::format("removing bad peer {}", peer->display_name()));
    swarm->removePeer(peer);
}

struct ComparePeerByActivity
{
    [[nodiscard]] constexpr static int compare(tr_peer const* a, tr_peer const* b) // <=>
    {
        if (a->do_purge != b->do_purge)
        {
            return a->do_purge ? 1 : -1;
        }

        /* the one to give us data more recently goes first */
        if (a->atom->piece_data_time != b->atom->piece_data_time)
        {
            return a->atom->piece_data_time > b->atom->piece_data_time ? -1 : 1;
        }

        /* the one we connected to most recently goes first */
        if (a->atom->time != b->atom->time)
        {
            return a->atom->time > b->atom->time ? -1 : 1;
        }

        return 0;
    }

    [[nodiscard]] constexpr bool operator()(tr_peer const* a, tr_peer const* b) const // less than
    {
        return compare(a, b) < 0;
    }
};

[[nodiscard]] auto getPeersToClose(tr_swarm const* const swarm, time_t const now_sec)
{
    auto peers_to_close = std::vector<tr_peer*>{};

    auto const peer_count = swarm->peerCount();
    for (auto* peer : swarm->peers)
    {
        if (shouldPeerBeClosed(swarm, peer, peer_count, now_sec))
        {
            peers_to_close.push_back(peer);
        }
    }

    return peers_to_close;
}

} // namespace

void closeBadPeers(tr_swarm* s, time_t const now_sec)
{
    for (auto* peer : getPeersToClose(s, now_sec))
    {
        closePeer(peer);
    }
}

void enforceSwarmPeerLimit(tr_swarm* swarm, size_t max)
{
    // do we have too many peers?
    if (auto const n = swarm->peerCount(); n <= max)
    {
        return;
    }

    // close all but the `max` most active
    auto peers = swarm->peers;
    std::partial_sort(std::begin(peers), std::begin(peers) + max, std::end(peers), ComparePeerByActivity{});
    std::for_each(std::begin(peers) + max, std::end(peers), closePeer);
}

void enforceSessionPeerLimit(tr_session* session)
{
    // No need to disconnect if we are under the peer limit
    auto const max = session->peerLimit();
    if (tr_peerMsgs::size() <= max)
    {
        return;
    }

    // Make a list of all the peers.
    auto peers = std::vector<tr_peer*>{};
    peers.reserve(tr_peerMsgs::size());
    for (auto const* const tor : session->torrents())
    {
        peers.insert(std::end(peers), std::begin(tor->swarm->peers), std::end(tor->swarm->peers));
    }

    TR_ASSERT(tr_peerMsgs::size() == std::size(peers));
    if (std::size(peers) > max)
    {
        std::partial_sort(std::begin(peers), std::begin(peers) + max, std::end(peers), ComparePeerByActivity{});
        std::for_each(std::begin(peers) + max, std::end(peers), closePeer);
    }
}

} // namespace disconnect_helpers

void tr_peerMgr::reconnectPulse()
{
    using namespace disconnect_helpers;

    auto const lock = session->unique_lock();
    auto const now_sec = tr_time();

    // remove crappy peers
    for (auto* const tor : session->torrents())
    {
        auto* const swarm = tor->swarm;

        if (!swarm->is_running)
        {
            swarm->removeAllPeers();
        }
        else
        {
            closeBadPeers(swarm, now_sec);
        }
    }

    // if we're over the per-torrent peer limits, cull some peers
    for (auto* const tor : session->torrents())
    {
        if (tor->isRunning)
        {
            enforceSwarmPeerLimit(tor->swarm, tor->peerLimit());
        }
    }

    // if we're over the per-session peer limits, cull some peers
    enforceSessionPeerLimit(session);

    // try to make new peer connections
    auto const max_connections_per_pulse = int(MaxConnectionsPerSecond * (ReconnectPeriodMsec / 1000.0));
    makeNewPeerConnections(max_connections_per_pulse);
}

// --- Bandwidth Allocation

namespace bandwidth_helpers
{

void pumpAllPeers(tr_peerMgr* mgr)
{
    for (auto* const tor : mgr->session->torrents())
    {
        for (auto* const peer : tor->swarm->peers)
        {
            peer->pulse();
        }
    }
}

void queuePulse(tr_session* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    if (!session->queueEnabled(dir))
    {
        return;
    }

    auto const n = session->countQueueFreeSlots(dir);
    for (auto* tor : session->getNextQueuedTorrents(dir, n))
    {
        tr_torrentStartNow(tor);
        session->onQueuedTorrentStarted(tor);
    }
}

} // namespace bandwidth_helpers

void tr_peerMgr::bandwidthPulse()
{
    using namespace bandwidth_helpers;

    auto const lock = unique_lock();

    pumpAllPeers(this);

    // allocate bandwidth to the peers
    static auto constexpr Msec = std::chrono::duration_cast<std::chrono::milliseconds>(BandwidthPeriod).count();
    session->top_bandwidth_.allocate(Msec);

    // torrent upkeep
    auto& torrents = session->torrents();
    std::for_each(std::begin(torrents), std::end(torrents), [](auto* tor) { tor->do_idle_work(); });

    /* pump the queues */
    queuePulse(session, TR_UP);
    queuePulse(session, TR_DOWN);

    reconnectPulse();
}

// ---

bool tr_swarm::peer_is_in_use(peer_atom const& atom) const
{
    return atom.is_connected || outgoing_handshakes.count(atom.addr) != 0U ||
        manager->incoming_handshakes.count(atom.addr) != 0U;
}

namespace connect_helpers
{
namespace
{

/* is this atom someone that we'd want to initiate a connection to? */
[[nodiscard]] bool isPeerCandidate(tr_torrent const* tor, peer_atom const& atom, time_t const now)
{
    // have we already tried and failed to connect?
    if (auto const reachable = atom.isReachable(); reachable && !*reachable)
    {
        return false;
    }

    // not if we're both seeds
    if (tor->isDone() && atom.isSeed())
    {
        return false;
    }

    // not if we've already got a connection to them...
    if (tor->swarm->peer_is_in_use(atom))
    {
        return false;
    }

    // not if we just tried them already
    if (now - atom.time < atom.getReconnectIntervalSecs(now))
    {
        return false;
    }

    // not if they're blocklisted
    if (atom.isBlocklisted(tor->session))
    {
        return false;
    }

    // not if they're banned...
    if ((atom.flags2 & MyflagBanned) != 0)
    {
        return false;
    }

    return true;
}

struct peer_candidate
{
    uint64_t score;
    tr_torrent* tor;
    peer_atom* atom;
};

[[nodiscard]] bool torrentWasRecentlyStarted(tr_torrent const* tor)
{
    return difftime(tr_time(), tor->startDate) < 120;
}

[[nodiscard]] constexpr uint64_t addValToKey(uint64_t value, int width, uint64_t addme)
{
    value = value << (uint64_t)width;
    value |= addme;
    return value;
}

/* smaller value is better */
[[nodiscard]] uint64_t getPeerCandidateScore(tr_torrent const* tor, peer_atom const& atom, uint8_t salt)
{
    auto i = uint64_t{};
    auto score = uint64_t{};
    bool const failed = atom.lastConnectionAt < atom.lastConnectionAttemptAt;

    /* prefer peers we've connected to, or never tried, over peers we failed to connect to. */
    i = failed ? 1 : 0;
    score = addValToKey(score, 1, i);

    /* prefer the one we attempted least recently (to cycle through all peers) */
    i = atom.lastConnectionAttemptAt;
    score = addValToKey(score, 32, i);

    /* prefer peers belonging to a torrent of a higher priority */
    switch (tor->getPriority())
    {
    case TR_PRI_HIGH:
        i = 0;
        break;

    case TR_PRI_NORMAL:
        i = 1;
        break;

    case TR_PRI_LOW:
        i = 2;
        break;
    }

    score = addValToKey(score, 4, i);

    /* prefer recently-started torrents */
    i = torrentWasRecentlyStarted(tor) ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* prefer torrents we're downloading with */
    i = tor->isDone() ? 1 : 0;
    score = addValToKey(score, 1, i);

    /* prefer peers that are known to be connectible */
    i = (atom.flags & ADDED_F_CONNECTABLE) != 0 ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* prefer peers that we might be able to upload to */
    i = (atom.flags & ADDED_F_SEED_FLAG) == 0 ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* Prefer peers that we got from more trusted sources.
     * lower `fromBest` values indicate more trusted sources */
    score = addValToKey(score, 4, atom.fromBest);

    /* salt */
    score = addValToKey(score, 8, salt);

    return score;
}

} // namespace

/** @return an array of all the atoms we might want to connect to */
[[nodiscard]] std::vector<peer_candidate> getPeerCandidates(tr_session* session, size_t max)
{
    auto const now = tr_time();
    auto const now_msec = tr_time_msec();

    // leave 5% of connection slots for incoming connections -- ticket #2609
    if (auto const max_candidates = static_cast<size_t>(session->peerLimit() * 0.95); max_candidates <= tr_peerMsgs::size())
    {
        return {};
    }

    auto candidates = std::vector<peer_candidate>{};
    candidates.reserve(peer_atom::atom_count());

    /* populate the candidate array */
    auto salter = tr_salt_shaker{};
    for (auto* const tor : session->torrents())
    {
        auto* const swarm = tor->swarm;

        if (!swarm->is_running)
        {
            continue;
        }

        /* if everyone in the swarm is seeds and pex is disabled because
         * the torrent is private, then don't initiate connections */
        bool const seeding = tor->isDone();
        if (seeding && swarm->isAllSeeds() && tor->isPrivate())
        {
            continue;
        }

        /* if we've already got enough peers in this torrent... */
        if (tor->peerLimit() <= swarm->peerCount())
        {
            continue;
        }

        /* if we've already got enough speed in this torrent... */
        if (seeding && tor->bandwidth_.is_maxed_out(TR_UP, now_msec))
        {
            continue;
        }

        for (auto& atom : swarm->pool)
        {
            if (isPeerCandidate(tor, atom, now))
            {
                candidates.push_back({ getPeerCandidateScore(tor, atom, salter()), tor, &atom });
            }
        }
    }

    // only keep the best `max` candidates
    if (std::size(candidates) > max)
    {
        std::partial_sort(
            std::begin(candidates),
            std::begin(candidates) + max,
            std::end(candidates),
            [](auto const& a, auto const& b) { return a.score < b.score; });
        candidates.resize(max);
    }

    return candidates;
}

void initiateConnection(tr_peerMgr* mgr, tr_swarm* s, peer_atom& atom)
{
    using namespace handshake_helpers;

    auto const now = tr_time();
    bool utp = mgr->session->allowsUTP() && !atom.utp_failed;

    if (atom.fromFirst == TR_PEER_FROM_PEX)
    {
        /* PEX has explicit signalling for µTP support.  If an atom
           originally came from PEX and doesn't have the µTP flag, skip the
           µTP connection attempt.  Are we being optimistic here? */
        utp = utp && (atom.flags & ADDED_F_UTP_FLAGS) != 0;
    }

    auto* const session = mgr->session;

    if (tr_peer_socket::limit_reached(session) || (!utp && !session->allowsTCP()))
    {
        return;
    }

    tr_logAddTraceSwarm(
        s,
        fmt::format("Starting an OUTGOING {} connection with {}", utp ? " µTP" : "TCP", atom.display_name()));

    auto peer_io = tr_peerIo::new_outgoing(
        session,
        &session->top_bandwidth_,
        atom.addr,
        atom.port,
        s->tor->infoHash(),
        s->tor->completeness == TR_SEED,
        utp);

    if (!peer_io)
    {
        tr_logAddTraceSwarm(s, fmt::format("peerIo not created; marking peer {} as unreachable", atom.display_name()));
        atom.flags2 |= MyflagUnreachable;
        ++atom.num_fails;
    }
    else
    {
        s->outgoing_handshakes.try_emplace(
            atom.addr,
            &mgr->handshake_mediator_,
            peer_io,
            session->encryptionMode(),
            [mgr](tr_handshake::Result const& result) { return on_handshake_done(mgr, result); });
    }

    atom.lastConnectionAttemptAt = now;
    atom.time = now;
}

} // namespace connect_helpers

void tr_peerMgr::makeNewPeerConnections(size_t max)
{
    using namespace connect_helpers;

    auto const lock = session->unique_lock();

    for (auto& candidate : getPeerCandidates(session, max))
    {
        initiateConnection(this, candidate.tor->swarm, *candidate.atom);
    }
}

// ---

bool HandshakeMediator::is_peer_known_seed(tr_torrent_id_t tor_id, tr_address const& addr) const
{
    auto const* const tor = session_.torrents().get(tor_id);
    return tor != nullptr && tor->swarm != nullptr && tor->swarm->peer_is_a_seed(addr);
}
