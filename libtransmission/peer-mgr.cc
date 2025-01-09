// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno> // error codes ERANGE, ...
#include <chrono>
#include <cmath>
#include <cstddef> // std::byte
#include <cstdint>
#include <ctime> // time_t
#include <iterator> // std::back_inserter
#include <memory>
#include <optional>
#include <tuple> // std::tie
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <small/map.hpp>
#include <small/vector.hpp>

#include <fmt/core.h>

#define LIBTRANSMISSION_PEER_MODULE
#include "libtransmission/transmission.h"

#include "libtransmission/announcer.h"
#include "libtransmission/block-info.h" // tr_block_info
#include "libtransmission/clients.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/handshake.h"
#include "libtransmission/interned-string.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/observable.h"
#include "libtransmission/peer-common.h"
#include "libtransmission/peer-io.h"
#include "libtransmission/peer-mgr-wishlist.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/peer-msgs.h"
#include "libtransmission/peer-socket.h"
#include "libtransmission/quark.h"
#include "libtransmission/session.h"
#include "libtransmission/timer.h"
#include "libtransmission/torrent-magnet.h"
#include "libtransmission/torrent.h"
#include "libtransmission/torrents.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/utils.h"
#include "libtransmission/values.h"
#include "libtransmission/webseed.h"

using namespace std::literals;
using namespace libtransmission::Values;

static auto constexpr CancelHistorySec = 60;

// ---

namespace
{

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
        info.info_hash = tor->info_hash();
        info.client_peer_id = tor->peer_id();
        info.id = tor->id();
        info.is_done = tor->is_done();
        return info;
    }

public:
    explicit HandshakeMediator(
        tr_session const& session,
        libtransmission::TimerMaker& timer_maker,
        tr_torrents& torrents) noexcept
        : session_{ session }
        , timer_maker_{ timer_maker }
        , torrents_{ torrents }
    {
    }

    [[nodiscard]] std::optional<TorrentInfo> torrent(tr_sha1_digest_t const& info_hash) const override
    {
        return torrent(torrents_.get(info_hash));
    }

    [[nodiscard]] std::optional<TorrentInfo> torrent_from_obfuscated(
        tr_sha1_digest_t const& obfuscated_info_hash) const override
    {
        return torrent(torrents_.find_from_obfuscated_hash(obfuscated_info_hash));
    }

    [[nodiscard]] bool allows_dht() const override
    {
        return session_.allowsDHT();
    }

    [[nodiscard]] bool allows_tcp() const override
    {
        return session_.allowsTCP();
    }

    void set_utp_failed(tr_sha1_digest_t const& info_hash, tr_socket_address const& socket_address) override;

    [[nodiscard]] libtransmission::TimerMaker& timer_maker() override
    {
        return timer_maker_;
    }

    [[nodiscard]] size_t pad(void* setme, size_t maxlen) const override
    {
        auto const len = tr_rand_int(maxlen);
        tr_rand_buffer(setme, len);
        return len;
    }

private:
    tr_session const& session_;
    libtransmission::TimerMaker& timer_maker_;
    tr_torrents& torrents_;
};

using Handshakes = std::unordered_map<tr_socket_address, tr_handshake>;

} // anonymous namespace

void tr_peer_info::merge(tr_peer_info& that) noexcept
{
    TR_ASSERT(is_connectable_.value_or(true) || !is_connected());
    TR_ASSERT(that.is_connectable_.value_or(true) || !that.is_connected());

    connection_attempted_at_ = std::max(connection_attempted_at_, that.connection_attempted_at_);
    connection_changed_at_ = std::max(connection_changed_at_, that.connection_changed_at_);
    piece_data_at_ = std::max(piece_data_at_, that.piece_data_at_);

    /* no need to merge blocklist since it gets updated elsewhere */

    {
        // This part is frankly convoluted and confusing, but the idea is:
        // 1. If the two peer info objects agree that this peer is connectable/non-connectable,
        //    then the answer is straightforward: We keep the agreed value.
        // 2. If the two peer info objects disagrees as to whether this peer is connectable,
        //    then we reset the flag to an empty value, so that we can try for ourselves when
        //    initiating outgoing connections.
        // 3. If one object has knowledge and the other doesn't, then we take the word of the
        //    peer info object with knowledge with one exception:
        //    - If the object with knowledge says the peer is not connectable, but we are
        //      currently connected to the peer, then we give it the benefit of the doubt.
        //      The connectable flag will be reset to an empty value.
        // 4. In case both objects have no knowledge about whether this peer is connectable,
        //    we shall not make any assumptions: We keep the flag empty.
        //
        // Truth table:
        //   +-----------------+---------------+----------------------+--------------------+---------+
        //   | is_connectable_ | is_connected_ | that.is_connectable_ | that.is_connected_ | Result  |
        //   +=================+===============+======================+====================+=========+
        //   | T               | T             | T                    | T                  | T       |
        //   | T               | T             | T                    | F                  | T       |
        //   | T               | T             | F                    | F                  | ?       |
        //   | T               | T             | ?                    | T                  | T       |
        //   | T               | T             | ?                    | F                  | T       |
        //   | T               | F             | T                    | T                  | T       |
        //   | T               | F             | T                    | F                  | T       |
        //   | T               | F             | F                    | F                  | ?       |
        //   | T               | F             | ?                    | T                  | T       |
        //   | T               | F             | ?                    | F                  | T       |
        //   | F               | F             | T                    | T                  | ?       |
        //   | F               | F             | T                    | F                  | ?       |
        //   | F               | F             | F                    | F                  | F       |
        //   | F               | F             | ?                    | T                  | ?       |
        //   | F               | F             | ?                    | F                  | F       |
        //   | ?               | T             | T                    | T                  | T       |
        //   | ?               | T             | T                    | F                  | T       |
        //   | ?               | T             | F                    | F                  | ?       |
        //   | ?               | T             | ?                    | T                  | ?       |
        //   | ?               | T             | ?                    | F                  | ?       |
        //   | ?               | F             | T                    | T                  | T       |
        //   | ?               | F             | T                    | F                  | T       |
        //   | ?               | F             | F                    | F                  | F       |
        //   | ?               | F             | ?                    | T                  | ?       |
        //   | ?               | F             | ?                    | F                  | ?       |
        //   | N/A             | N/A           | F                    | T                  | Invalid |
        //   | F               | T             | N/A                  | N/A                | Invalid |
        //   +-----------------+---------------+----------------------+--------------------+---------+

        auto const conn_this = is_connectable_ && *is_connectable_;
        auto const conn_that = that.is_connectable_ && *that.is_connectable_;

        if ((!is_connectable_ && !that.is_connectable_) ||
            is_connectable_.value_or(conn_that || is_connected()) !=
                that.is_connectable_.value_or(conn_this || that.is_connected()))
        {
            is_connectable_.reset();
        }
        else
        {
            set_connectable(conn_this || conn_that);
        }
    }

    if (auto const& other = that.supports_utp(); !supports_utp().has_value() && other)
    {
        set_utp_supported(*other);
    }

    if (auto const& other = that.prefers_encryption(); !prefers_encryption().has_value() && other)
    {
        set_encryption_preferred(*other);
    }

    if (auto const& other = that.supports_holepunch(); !supports_holepunch().has_value() && other)
    {
        set_holepunch_supported(*other);
    }

    /* from_first_ should never be modified */
    found_at(that.from_best());

    /* num_consecutive_fruitless_ is already the latest */
    pex_flags_ |= that.pex_flags_;

    if (that.is_banned())
    {
        ban();
    }
    /* is_connected_ should already be set */
    /* keep is_seed_ as-is */
    /* keep upload_only_ as-is */

    if (that.outgoing_handshake_)
    {
        if (outgoing_handshake_)
        {
            that.destroy_handshake();
        }
        else
        {
            outgoing_handshake_ = std::move(that.outgoing_handshake_);
        }
    }
}

#define tr_logAddDebugSwarm(swarm, msg) tr_logAddDebugTor((swarm)->tor, msg)
#define tr_logAddTraceSwarm(swarm, msg) tr_logAddTraceTor((swarm)->tor, msg)

namespace
{

/* better goes first */
constexpr struct
{
    [[nodiscard]] constexpr static int compare(tr_peer_info const& a, tr_peer_info const& b) noexcept // <=>
    {
        if (auto const val = a.compare_by_piece_data_time(b); val != 0)
        {
            return -val;
        }

        if (auto const val = tr_compare_3way(a.from_best(), b.from_best()); val != 0)
        {
            return val;
        }

        return a.compare_by_fruitless_count(b);
    }

    [[nodiscard]] constexpr bool operator()(tr_peer_info const& a, tr_peer_info const& b) const noexcept
    {
        return compare(a, b) < 0;
    }

    template<typename T>
    [[nodiscard]] constexpr std::enable_if_t<std::is_same_v<std::decay_t<decltype(*std::declval<T>())>, tr_peer_info>, bool>
    operator()(T const& a, T const& b) const noexcept
    {
        return compare(*a, *b) < 0;
    }
} CompareAtomsByUsefulness{};

} // namespace

/** @brief Opaque, per-torrent data structure for peer connection information */
class tr_swarm
{
public:
    using Peers = std::vector<tr_peerMsgs*>;
    using Pool = small::map<tr_socket_address, std::shared_ptr<tr_peer_info>>;

    class WishlistMediator final : public Wishlist::Mediator
    {
    public:
        explicit WishlistMediator(tr_swarm& swarm)
            : tor_{ *swarm.tor }
            , swarm_{ swarm }
        {
        }

        [[nodiscard]] bool client_has_block(tr_block_index_t block) const override;
        [[nodiscard]] bool client_has_piece(tr_piece_index_t piece) const override;
        [[nodiscard]] bool client_wants_piece(tr_piece_index_t piece) const override;
        [[nodiscard]] bool is_sequential_download() const override;
        [[nodiscard]] uint8_t count_active_requests(tr_block_index_t block) const override;
        [[nodiscard]] size_t count_piece_replication(tr_piece_index_t piece) const override;
        [[nodiscard]] tr_block_span_t block_span(tr_piece_index_t piece) const override;
        [[nodiscard]] tr_piece_index_t piece_count() const override;
        [[nodiscard]] tr_priority_t priority(tr_piece_index_t piece) const override;

        [[nodiscard]] libtransmission::ObserverTag observe_peer_disconnect(
            libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const&, tr_bitfield const&>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_got_bad_piece(
            libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_got_bitfield(
            libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const&>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_got_block(
            libtransmission::SimpleObservable<tr_torrent*, tr_block_index_t>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_got_choke(
            libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const&>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_got_have(
            libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_got_have_all(
            libtransmission::SimpleObservable<tr_torrent*>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_got_reject(
            libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_index_t>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_piece_completed(
            libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_priority_changed(
            libtransmission::SimpleObservable<tr_torrent*, tr_file_index_t const*, tr_file_index_t, tr_priority_t>::Observer
                observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_sent_cancel(
            libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_index_t>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_sent_request(
            libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_span_t>::Observer observer) override;
        [[nodiscard]] libtransmission::ObserverTag observe_sequential_download_changed(
            libtransmission::SimpleObservable<tr_torrent*, bool>::Observer observer) override;

    private:
        tr_torrent& tor_;
        tr_swarm& swarm_;
    };

    [[nodiscard]] auto unique_lock() const
    {
        return tor->unique_lock();
    }

    tr_swarm(tr_peerMgr* manager_in, tr_torrent* tor_in) noexcept
        : manager{ manager_in }
        , tor{ tor_in }
        , tags_{ {
              tor_in->done_.observe([this](tr_torrent*, bool) { on_torrent_done(); }),
              tor_in->doomed_.observe([this](tr_torrent*) { on_torrent_doomed(); }),
              tor_in->got_bad_piece_.observe([this](tr_torrent*, tr_piece_index_t p) { on_got_bad_piece(p); }),
              tor_in->got_metainfo_.observe([this](tr_torrent*) { on_got_metainfo(); }),
              tor_in->piece_completed_.observe([this](tr_torrent*, tr_piece_index_t p) { on_piece_completed(p); }),
              tor_in->started_.observe([this](tr_torrent*) { on_torrent_started(); }),
              tor_in->stopped_.observe([this](tr_torrent*) { on_torrent_stopped(); }),
              tor_in->swarm_is_all_upload_only_.observe([this](tr_torrent* /*tor*/) { on_swarm_is_all_upload_only(); }),
          } }
    {
        rebuild_webseeds();
    }

    tr_swarm(tr_swarm&&) = delete;
    tr_swarm(tr_swarm const&) = delete;
    tr_swarm& operator=(tr_swarm&&) = delete;
    tr_swarm& operator=(tr_swarm const&) = delete;

    ~tr_swarm()
    {
        auto const lock = unique_lock();
        TR_ASSERT(!is_running);
        TR_ASSERT(std::empty(peers));
    }

    [[nodiscard]] uint16_t count_active_webseeds(uint64_t now) const noexcept
    {
        if (!tor->is_running() || tor->is_done())
        {
            return {};
        }

        return std::count_if(
            std::begin(webseeds),
            std::end(webseeds),
            [&now](auto const& webseed) { return webseed->get_piece_speed(now, TR_DOWN).base_quantity() != 0U; });
    }

    [[nodiscard]] TR_CONSTEXPR20 auto peerCount() const noexcept
    {
        return std::size(peers);
    }

    void remove_peer(tr_peerMsgs* peer)
    {
        auto const lock = unique_lock();

        peer_disconnect.emit(tor, peer->has(), peer->active_requests);

        auto const& peer_info = peer->peer_info;
        TR_ASSERT(peer_info);

        --stats.peer_count;
        --stats.peer_from_count[peer_info->from_first()];

        if (auto iter = std::find(std::begin(peers), std::end(peers), peer); iter != std::end(peers))
        {
            peers.erase(iter);
            TR_ASSERT(stats.peer_count == peerCount());
        }

        delete peer;
    }

    void remove_all_peers()
    {
        auto tmp = Peers{};
        std::swap(tmp, peers);
        for (auto* peer : tmp)
        {
            remove_peer(peer);
        }

        TR_ASSERT(stats.peer_count == 0);
    }

    [[nodiscard]] TR_CONSTEXPR20 auto is_all_upload_only() const noexcept
    {
        if (!pool_is_all_upload_only_)
        {
            pool_is_all_upload_only_ = std::all_of(
                std::begin(connectable_pool),
                std::end(connectable_pool),
                [](auto const& key_val) { return key_val.second->is_upload_only(); });
        }

        return *pool_is_all_upload_only_;
    }

    [[nodiscard]] std::shared_ptr<tr_peer_info> get_existing_peer_info(tr_socket_address const& socket_address) const noexcept
    {
        if (auto it = connectable_pool.find(socket_address); it != std::end(connectable_pool))
        {
            return it->second;
        }

        return {};
    }

    std::shared_ptr<tr_peer_info> ensure_info_exists(
        tr_socket_address const& socket_address,
        uint8_t const flags,
        tr_peer_from const from)
    {
        TR_ASSERT(socket_address.is_valid());
        TR_ASSERT(from < TR_PEER_FROM__MAX);

        auto peer_info = get_existing_peer_info(socket_address);
        if (peer_info)
        {
            peer_info->found_at(from);
            peer_info->set_pex_flags(flags);
        }
        else
        {
            peer_info = connectable_pool
                            .try_emplace(socket_address, std::make_shared<tr_peer_info>(socket_address, flags, from))
                            .first->second;
            ++stats.known_peer_from_count[from];
        }

        mark_all_upload_only_flag_dirty();

        return peer_info;
    }

    static void peer_callback_bt(tr_peerMsgs* const msgs, tr_peer_event const& event, void* const vs)
    {
        TR_ASSERT(msgs != nullptr);
        auto* s = static_cast<tr_swarm*>(vs);
        TR_ASSERT(msgs->swarm == s);
        auto const lock = s->unique_lock();

        switch (event.type)
        {
        case tr_peer_event::Type::ClientSentCancel:
            {
                auto* const tor = s->tor;
                auto const loc = tor->piece_loc(event.pieceIndex, event.offset);
                s->sent_cancel.emit(tor, msgs, loc.block);
            }
            break;

        case tr_peer_event::Type::ClientSentPieceData:
            {
                auto* const tor = s->tor;

                tor->bytes_uploaded_ += event.length;
                tr_announcerAddBytes(tor, TR_ANN_UP, event.length);
                tor->set_date_active(tr_time());
                tor->session->add_uploaded(event.length);
            }

            break;

        case tr_peer_event::Type::ClientGotPieceData:
            on_client_got_piece_data(s->tor, event.length, tr_time());
            break;

        case tr_peer_event::Type::ClientGotHave:
            s->got_have.emit(s->tor, event.pieceIndex);
            s->mark_all_upload_only_flag_dirty();
            break;

        case tr_peer_event::Type::ClientGotHaveAll:
            s->got_have_all.emit(s->tor);
            s->mark_all_upload_only_flag_dirty();
            break;

        case tr_peer_event::Type::ClientGotHaveNone:
            s->mark_all_upload_only_flag_dirty();
            break;

        case tr_peer_event::Type::ClientGotBitfield:
            s->got_bitfield.emit(s->tor, msgs->has());
            s->mark_all_upload_only_flag_dirty();
            break;

        case tr_peer_event::Type::ClientGotChoke:
            s->got_choke.emit(s->tor, msgs->active_requests);
            break;

        case tr_peer_event::Type::ClientGotPort:
            // We have 2 cases:
            // 1. We don't know the listening port of this peer (i.e. incoming connection and first time ClientGotPort)
            // 2. We got a new listening port from a known peer
            if (auto const& info = msgs->peer_info;
                !std::empty(event.port) && info && (std::empty(info->listen_port()) || info->listen_port() != event.port))
            {
                s->on_got_port(msgs, event);
            }

            break;

        case tr_peer_event::Type::ClientGotSuggest:
        case tr_peer_event::Type::ClientGotAllowedFast:
            // not currently supported
            break;

        case tr_peer_event::Type::Error:
            if (event.err == ERANGE || event.err == EMSGSIZE || event.err == ENOTCONN)
            {
                // some protocol error from the peer
                msgs->disconnect_soon();
                tr_logAddDebugSwarm(
                    s,
                    fmt::format(
                        "setting {} is_disconnecting_ flag because we got [({}) {}]",
                        msgs->display_name(),
                        event.err,
                        tr_strerror(event.err)));
            }
            else
            {
                tr_logAddDebugSwarm(s, fmt::format("unhandled error: ({}) {}", event.err, tr_strerror(event.err)));
            }

            break;

        default:
            peer_callback_common(msgs, event, s);
            break;
        }
    }

    libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const& /*bitfield*/, tr_bitfield const& /*active requests*/>
        peer_disconnect;
    libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const&> got_bitfield;
    libtransmission::SimpleObservable<tr_torrent*, tr_block_index_t> got_block;
    libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const&> got_choke;
    libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t> got_have;
    libtransmission::SimpleObservable<tr_torrent*> got_have_all;
    libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_index_t> got_reject;
    libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_index_t> sent_cancel;
    libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_span_t> sent_request;

    mutable tr_swarm_stats stats = {};

    uint8_t optimistic_unchoke_time_scaler = 0;

    bool is_running = false;

    tr_peerMgr* const manager;

    tr_torrent* const tor;

    std::vector<std::unique_ptr<tr_webseed>> webseeds;

    Peers peers;

    // depends-on: tor
    WishlistMediator wishlist_mediator{ *this };
    std::unique_ptr<Wishlist> wishlist;

    Pool connectable_pool;

    tr_peerMsgs* optimistic = nullptr; /* the optimistic peer, or nullptr if none */

private:
    void rebuild_webseeds()
    {
        auto const n = tor->webseed_count();

        webseeds.clear();
        webseeds.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            webseeds.emplace_back(tr_webseed::create(*tor, tor->webseed(i), &tr_swarm::peer_callback_webseed, this));
        }
        webseeds.shrink_to_fit();

        stats.active_webseed_count = 0;
    }

    void add_strike(tr_peer* peer) const
    {
        tr_logAddTraceSwarm(
            this,
            fmt::format("increasing peer {} strike count to {}", peer->display_name(), peer->strikes + 1));

        if (++peer->strikes >= MaxBadPiecesPerPeer)
        {
            peer->ban();
            tr_logAddTraceSwarm(this, fmt::format("banning peer {}", peer->display_name()));
        }
    }

    void stop()
    {
        auto const lock = unique_lock();

        is_running = false;
        remove_all_peers();
        wishlist.reset();
        for (auto& [sockaddr, peer_info] : connectable_pool)
        {
            peer_info->destroy_handshake();
        }
    }

    static void maybe_send_cancel_request(tr_peer* peer, tr_block_index_t block, tr_peer const* muted)
    {
        if (peer != nullptr && peer != muted)
        {
            peer->maybe_cancel_block_request(block);
        }
    }

    void cancel_all_requests_for_block(tr_block_index_t block, tr_peer const* no_notify)
    {
        for (auto* peer : peers)
        {
            maybe_send_cancel_request(peer, block, no_notify);
        }
    }

    void mark_all_upload_only_flag_dirty() noexcept
    {
        pool_is_all_upload_only_.reset();
    }

    void on_torrent_doomed()
    {
        auto const lock = unique_lock();
        stop();
        tor->swarm = nullptr;
        delete this;
    }

    void on_torrent_done()
    {
        std::for_each(std::begin(peers), std::end(peers), [](auto* const peer) { peer->set_interested(false); });
        wishlist.reset();
    }

    void on_swarm_is_all_upload_only()
    {
        auto const lock = unique_lock();

        for (auto const& [socket_address, peer_info] : connectable_pool)
        {
            peer_info->set_upload_only();
        }

        mark_all_upload_only_flag_dirty();
    }

    void on_piece_completed(tr_piece_index_t piece)
    {
        bool piece_came_from_peers = false;

        for (auto* const peer : peers)
        {
            // notify the peer that we now have this piece
            peer->on_piece_completed(piece);

            if (!piece_came_from_peers)
            {
                piece_came_from_peers = peer->blame.test(piece);
            }
        }

        if (piece_came_from_peers) /* webseed downloads don't belong in announce totals */
        {
            tr_announcerAddBytes(tor, TR_ANN_DOWN, tor->piece_size(piece));
        }
    }

    void on_got_bad_piece(tr_piece_index_t piece)
    {
        auto const maybe_add_strike = [this, piece](tr_peer* const peer)
        {
            if (peer->blame.test(piece))
            {
                tr_logAddTraceSwarm(
                    this,
                    fmt::format(
                        "peer {} contributed to corrupt piece ({}); now has {} strikes",
                        peer->display_name(),
                        piece,
                        peer->strikes + 1));
                add_strike(peer);
            }
        };

        auto const byte_count = tor->piece_size(piece);

        for (auto* const peer : peers)
        {
            maybe_add_strike(peer);
        }

        for (auto& webseed : webseeds)
        {
            maybe_add_strike(webseed.get());
        }

        tr_announcerAddBytes(tor, TR_ANN_CORRUPT, byte_count);
    }

    void on_got_metainfo()
    {
        // the webseed list may have changed...
        rebuild_webseeds();

        for (auto* peer : peers)
        {
            peer->on_torrent_got_metainfo();
        }
    }

    void on_torrent_started();
    void on_torrent_stopped();

    // ---

    static void peer_callback_webseed(tr_peer* const peer, tr_peer_event const& event, void* const vs)
    {
        TR_ASSERT(peer != nullptr);
        auto* s = static_cast<tr_swarm*>(vs);
        auto const lock = s->unique_lock();

        switch (event.type)
        {
        case tr_peer_event::Type::ClientGotPieceData:
            on_client_got_piece_data(s->tor, event.length, tr_time());
            break;

        default:
            peer_callback_common(peer, event, s);
            break;
        }
    }

    static void peer_callback_common(tr_peer* const peer, tr_peer_event const& event, tr_swarm* const s)
    {
        switch (event.type)
        {
        case tr_peer_event::Type::ClientSentRequest:
            {
                auto* const tor = s->tor;
                auto const loc_begin = tor->piece_loc(event.pieceIndex, event.offset);
                auto const loc_end = tor->piece_loc(event.pieceIndex, event.offset, event.length);
                s->sent_request.emit(tor, peer, { loc_begin.block, loc_end.block });
            }
            break;

        case tr_peer_event::Type::ClientGotRej:
            {
                auto* const tor = s->tor;
                auto const loc = tor->piece_loc(event.pieceIndex, event.offset);
                s->got_reject.emit(tor, peer, loc.block);
            }
            break;

        case tr_peer_event::Type::ClientGotBlock:
            {
                auto* const tor = s->tor;
                auto const loc = tor->piece_loc(event.pieceIndex, event.offset);
                s->cancel_all_requests_for_block(loc.block, peer);
                peer->blocks_sent_to_client.add(tr_time(), 1);
                peer->blame.set(loc.piece);
                tor->on_block_received(loc.block);
                s->got_block.emit(tor, loc.block);
            }

            break;

        default:
            TR_ASSERT_MSG(false, "This should be unreachable code");
            break;
        }
    }

    static void on_client_got_piece_data(tr_torrent* const tor, uint32_t const sent_length, time_t const now)
    {
        tor->bytes_downloaded_ += sent_length;
        tor->set_date_active(now);
        tor->session->add_downloaded(sent_length);
    }

    void on_got_port(tr_peerMsgs* const msgs, tr_peer_event const& event)
    {
        auto info_this = msgs->peer_info;
        TR_ASSERT(info_this->is_connected());
        TR_ASSERT(info_this->listen_port() != event.port);

        // we already know about this peer
        if (auto it_that = connectable_pool.find({ info_this->listen_address(), event.port });
            it_that != std::end(connectable_pool))
        {
            auto const info_that = it_that->second;
            TR_ASSERT(it_that->first == info_that->listen_socket_address());
            TR_ASSERT(it_that->first.address() == info_this->listen_address());
            TR_ASSERT(it_that->first.port() != info_this->listen_port());

            // if there is an existing connection to this peer, keep the better one
            if (info_that->is_connected() && on_got_port_duplicate_connection(msgs, info_that))
            {
                goto EXIT; // NOLINT cppcoreguidelines-avoid-goto
            }

            // merge the peer info objects
            info_this->merge(*info_that);

            // info_that will be replaced by info_this later, so decrement stat
            --stats.known_peer_from_count[info_that->from_first()];
        }

        // erase the old peer info entry
        stats.known_peer_from_count[info_this->from_first()] -= connectable_pool.erase(info_this->listen_socket_address());

        // set new listen port
        info_this->set_listen_port(event.port);

        // insert or replace the peer info ptr at the target location
        ++stats.known_peer_from_count[info_this->from_first()];
        connectable_pool.insert_or_assign(info_this->listen_socket_address(), std::move(info_this));

EXIT:
        mark_all_upload_only_flag_dirty();
    }

    bool on_got_port_duplicate_connection(tr_peerMsgs* const msgs, std::shared_ptr<tr_peer_info> info_that)
    {
        auto const info_this = msgs->peer_info;

        TR_ASSERT(info_that->is_connected());

        if (CompareAtomsByUsefulness(*info_this, *info_that))
        {
            auto const it = std::find_if(
                std::begin(peers),
                std::end(peers),
                [&info_that](tr_peerMsgs const* const peer) { return peer->peer_info == info_that; });
            TR_ASSERT(it != std::end(peers));
            (*it)->disconnect_soon();

            return false;
        }

        info_that->merge(*info_this);
        msgs->disconnect_soon();
        stats.known_peer_from_count[info_this->from_first()] -= connectable_pool.erase(info_this->listen_socket_address());

        return true;
    }

    // ---

    // number of bad pieces a peer is allowed to send before we ban them
    static auto constexpr MaxBadPiecesPerPeer = 5U;

    std::array<libtransmission::ObserverTag, 8> const tags_;

    mutable std::optional<bool> pool_is_all_upload_only_;
};

bool tr_swarm::WishlistMediator::client_has_block(tr_block_index_t block) const
{
    return tor_.has_block(block);
}

bool tr_swarm::WishlistMediator::client_has_piece(tr_piece_index_t piece) const
{
    return tor_.has_blocks(block_span(piece));
}

bool tr_swarm::WishlistMediator::client_wants_piece(tr_piece_index_t piece) const
{
    return tor_.piece_is_wanted(piece);
}

bool tr_swarm::WishlistMediator::is_sequential_download() const
{
    return tor_.is_sequential_download();
}

uint8_t tr_swarm::WishlistMediator::count_active_requests(tr_block_index_t block) const
{
    auto const op = [block](uint8_t acc, auto const& peer)
    {
        return acc + (peer->active_requests.test(block) ? 1U : 0U);
    };
    return std::accumulate(std::begin(swarm_.peers), std::end(swarm_.peers), uint8_t{}, op) +
        std::accumulate(std::begin(swarm_.webseeds), std::end(swarm_.webseeds), uint8_t{}, op);
}

size_t tr_swarm::WishlistMediator::count_piece_replication(tr_piece_index_t piece) const
{
    auto const op = [piece](size_t acc, auto const& peer)
    {
        return acc + (peer->has_piece(piece) ? 1U : 0U);
    };
    return std::accumulate(std::begin(swarm_.peers), std::end(swarm_.peers), size_t{}, op) +
        std::accumulate(std::begin(swarm_.webseeds), std::end(swarm_.webseeds), size_t{}, op);
}

tr_block_span_t tr_swarm::WishlistMediator::block_span(tr_piece_index_t piece) const
{
    auto span = tor_.block_span_for_piece(piece);

    // Overlapping block spans caused by blocks unaligned to piece boundaries
    // might cause redundant block requests to be sent out, so detect it and
    // ensure that block spans within the wishlist do not overlap.
    if (auto const is_unaligned_piece = tor_.block_loc(span.begin).piece != piece; is_unaligned_piece)
    {
        ++span.begin;
    }

    return span;
}

tr_piece_index_t tr_swarm::WishlistMediator::piece_count() const
{
    return tor_.piece_count();
}

tr_priority_t tr_swarm::WishlistMediator::priority(tr_piece_index_t piece) const
{
    return tor_.piece_priority(piece);
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_peer_disconnect(
    libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const&, tr_bitfield const&>::Observer observer)
{
    return swarm_.peer_disconnect.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_got_bad_piece(
    libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t>::Observer observer)
{
    return tor_.got_bad_piece_.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_got_bitfield(
    libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const&>::Observer observer)
{
    return swarm_.got_bitfield.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_got_block(
    libtransmission::SimpleObservable<tr_torrent*, tr_block_index_t>::Observer observer)
{
    return swarm_.got_block.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_got_choke(
    libtransmission::SimpleObservable<tr_torrent*, tr_bitfield const&>::Observer observer)
{
    return swarm_.got_choke.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_got_have(
    libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t>::Observer observer)
{
    return swarm_.got_have.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_got_have_all(
    libtransmission::SimpleObservable<tr_torrent*>::Observer observer)
{
    return swarm_.got_have_all.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_got_reject(
    libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_index_t>::Observer observer)
{
    return swarm_.got_reject.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_piece_completed(
    libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t>::Observer observer)
{
    return tor_.piece_completed_.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_priority_changed(
    libtransmission::SimpleObservable<tr_torrent*, tr_file_index_t const*, tr_file_index_t, tr_priority_t>::Observer observer)
{
    return tor_.priority_changed_.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_sent_cancel(
    libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_index_t>::Observer observer)
{
    return swarm_.sent_cancel.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_sent_request(
    libtransmission::SimpleObservable<tr_torrent*, tr_peer*, tr_block_span_t>::Observer observer)
{
    return swarm_.sent_request.observe(std::move(observer));
}

libtransmission::ObserverTag tr_swarm::WishlistMediator::observe_sequential_download_changed(
    libtransmission::SimpleObservable<tr_torrent*, bool>::Observer observer)
{
    return tor_.sequential_download_changed_.observe(std::move(observer));
}

// ---

struct tr_peerMgr
{
private:
    static auto constexpr BandwidthTimerPeriod = 500ms;
    static auto constexpr PeerInfoPeriod = 1min;
    static auto constexpr RechokePeriod = 10s;

    // Max number of outbound peer connections to initiate.
    // This throttle is an arbitrary number to avoid overloading routers.
    static auto constexpr MaxConnectionsPerSecond = size_t{ 18U };
    static auto constexpr MaxConnectionsPerPulse = size_t(MaxConnectionsPerSecond * BandwidthTimerPeriod / 1s);

    // Building a peer candidate list is expensive, so cache it across pulses.
    // We want to cache it long enough to avoid excess CPU cycles,
    // but short enough that the data isn't too stale.
    static auto constexpr OutboundCandidatesListTtl = BandwidthTimerPeriod * 4U;

    // How big the candidate list should be when we create it.
    static auto constexpr OutboundCandidateListCapacity = MaxConnectionsPerPulse * OutboundCandidatesListTtl /
        BandwidthTimerPeriod;

public:
    // The peers we might try connecting to in the next few seconds.
    // This list is cached between pulses so use resilient keys, e.g.
    // a `tr_torrent_id_t` instead of a `tr_torrent*` that can be freed.
    using OutboundCandidates = small::
        max_size_vector<std::pair<tr_torrent_id_t, tr_socket_address>, OutboundCandidateListCapacity>;

    explicit tr_peerMgr(
        tr_session* session_in,
        libtransmission::TimerMaker& timer_maker,
        tr_torrents& torrents,
        libtransmission::Blocklists& blocklist)
        : session{ session_in }
        , torrents_{ torrents }
        , blocklists_{ blocklist }
        , handshake_mediator_{ *session, timer_maker, torrents }
        , bandwidth_timer_{ timer_maker.create([this]() { bandwidth_pulse(); }) }
        , peer_info_timer_{ timer_maker.create([this]() { peer_info_pulse(); }) }
        , rechoke_timer_{ timer_maker.create([this]() { rechoke_pulse_marshall(); }) }
        , blocklists_tag_{ blocklist.observe_changes([this]() { on_blocklists_changed(); }) }
    {
        bandwidth_timer_->start_repeating(BandwidthTimerPeriod);
        peer_info_timer_->start_repeating(PeerInfoPeriod);
        rechoke_timer_->start_repeating(RechokePeriod);
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
        rechoke_timer_->set_interval(100ms);
    }

    [[nodiscard]] tr_swarm* get_existing_swarm(tr_sha1_digest_t const& hash) const
    {
        auto* const tor = torrents_.get(hash);
        return tor == nullptr ? nullptr : tor->swarm;
    }

    tr_session* const session;
    tr_torrents& torrents_;
    libtransmission::Blocklists const& blocklists_;
    Handshakes incoming_handshakes;

    HandshakeMediator handshake_mediator_;

private:
    void bandwidth_pulse();
    void make_new_peer_connections();
    void peer_info_pulse();
    void rechoke_pulse() const;
    void reconnect_pulse();

    void rechoke_pulse_marshall()
    {
        rechoke_pulse();
        rechoke_timer_->set_interval(RechokePeriod);
    }

    void on_blocklists_changed() const
    {
        /* we cache whether or not a peer is blocklisted...
           since the blocklist has changed, erase that cached value */
        for (auto* const tor : torrents_)
        {
            for (auto const& [socket_address, peer_info] : tor->swarm->connectable_pool)
            {
                peer_info->set_blocklisted_dirty();
            }

            for (auto* const peer : tor->swarm->peers)
            {
                peer->peer_info->set_blocklisted_dirty();
                if (peer->peer_info->is_blocklisted(blocklists_))
                {
                    peer->disconnect_soon();
                    tr_logAddDebugTor(tor, fmt::format("Peer {} blocked in blocklists update", peer->display_name()));
                }
            }
        }
    }

    OutboundCandidates outbound_candidates_;

    std::unique_ptr<libtransmission::Timer> const bandwidth_timer_;
    std::unique_ptr<libtransmission::Timer> const peer_info_timer_;
    std::unique_ptr<libtransmission::Timer> const rechoke_timer_;

    libtransmission::ObserverTag const blocklists_tag_;
};

// --- tr_peer virtual functions

tr_peer::tr_peer(tr_torrent const& tor)
    : session{ tor.session }
    , swarm{ tor.swarm }
    , active_requests{ tor.block_count() }
    , blame{ tor.piece_count() }
{
}

// ---

tr_peerMgr* tr_peerMgrNew(tr_session* session)
{
    return new tr_peerMgr{ session, session->timerMaker(), session->torrents(), session->blocklist() };
}

void tr_peerMgrFree(tr_peerMgr* manager)
{
    delete manager;
}

// ---

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
 * 2. tr_swarm::wishlist, a class that tracks the pieces that we want to
 *    request. It's used to decide which blocks to return next when
 *    tr_peerMgrGetNextRequests() is called.
 */

std::vector<tr_block_span_t> tr_peerMgrGetNextRequests(tr_torrent* torrent, tr_peer const* peer, size_t numwant)
{
    TR_ASSERT(!torrent->is_done());
    tr_swarm& swarm = *torrent->swarm;
    if (!swarm.wishlist)
    {
        swarm.wishlist = std::make_unique<Wishlist>(swarm.wishlist_mediator);
    }
    return swarm.wishlist->next(
        numwant,
        [peer](tr_piece_index_t p) { return peer->has_piece(p); },
        [peer](tr_block_index_t b) { return peer->active_requests.test(b); });
}

namespace
{
namespace handshake_helpers
{
void create_bit_torrent_peer(
    tr_torrent& tor,
    std::shared_ptr<tr_peerIo> io,
    std::shared_ptr<tr_peer_info> peer_info,
    tr_interned_string client)
{
    TR_ASSERT(peer_info);
    TR_ASSERT(tor.swarm != nullptr);

    tr_swarm* swarm = tor.swarm;

    auto* const
        msgs = tr_peerMsgs::create(tor, std::move(peer_info), std::move(io), client, &tr_swarm::peer_callback_bt, swarm);
    swarm->peers.push_back(msgs);

    ++swarm->stats.peer_count;
    ++swarm->stats.peer_from_count[msgs->peer_info->from_first()];

    TR_ASSERT(swarm->stats.peer_count == swarm->peerCount());
    TR_ASSERT(swarm->stats.peer_from_count[msgs->peer_info->from_first()] <= swarm->stats.peer_count);
}

/* FIXME: this is kind of a mess. */
[[nodiscard]] bool on_handshake_done(tr_peerMgr* const manager, tr_handshake::Result const& result)
{
    auto const lock = manager->unique_lock();

    TR_ASSERT(result.io != nullptr);
    auto const& socket_address = result.io->socket_address();
    auto* const swarm = manager->get_existing_swarm(result.io->torrent_hash());
    auto info = swarm != nullptr ? swarm->get_existing_peer_info(socket_address) : std::shared_ptr<tr_peer_info>{};

    if (result.io->is_incoming())
    {
        manager->incoming_handshakes.erase(socket_address);
    }
    else if (info)
    {
        info->destroy_handshake();
    }

    if (!result.is_connected || swarm == nullptr || !swarm->is_running)
    {
        if (info && !info->is_connected())
        {
            info->on_fruitless_connection();

            if (!result.read_anything_from_peer)
            {
                tr_logAddTraceSwarm(
                    swarm,
                    fmt::format(
                        "marking peer {} as unreachable... num_fruitless is {}",
                        info->display_name(),
                        info->fruitless_connection_count()));
                info->set_connectable(false);
            }
        }

        return false;
    }

    if (result.io->is_incoming())
    {
        info = std::make_shared<tr_peer_info>(socket_address.address(), 0U, TR_PEER_FROM_INCOMING);
    }

    if (!info)
    {
        return false;
    }

    if (!result.io->is_incoming())
    {
        info->set_connectable();
    }

    // If we're connected via µTP, then we know the peer supports µTP...
    if (result.io->is_utp())
    {
        info->set_utp_supported();
    }

    if (info->is_banned())
    {
        tr_logAddTraceSwarm(swarm, fmt::format("banned peer {} tried to reconnect", info->display_name()));
        return false;
    }

    if (swarm->peerCount() >= swarm->tor->peer_limit()) // too many peers already
    {
        return false;
    }

    if (info->is_connected()) // we're already connected to this peer; do nothing
    {
        return false;
    }

    auto client = tr_interned_string{};
    if (result.peer_id)
    {
        auto buf = std::array<char, 128>{};
        tr_clientForId(std::data(buf), sizeof(buf), *result.peer_id);
        client = tr_interned_string{ tr_quark_new(std::data(buf)) };
    }

    result.io->set_bandwidth(&swarm->tor->bandwidth());
    create_bit_torrent_peer(*swarm->tor, result.io, std::move(info), client);

    return true;
}
} // namespace handshake_helpers
} // namespace

void tr_peerMgrAddIncoming(tr_peerMgr* manager, tr_peer_socket&& socket)
{
    using namespace handshake_helpers;

    auto const lock = manager->unique_lock();

    if (manager->blocklists_.contains(socket.address()))
    {
        tr_logAddTrace(fmt::format("Banned IP address '{}' tried to connect to us", socket.display_name()));
        socket.close();
    }
    else if (manager->incoming_handshakes.count(socket.socket_address()) != 0U)
    {
        socket.close();
    }
    else // we don't have a connection to them yet...
    {
        auto const socket_address = socket.socket_address();
        auto* const session = manager->session;
        manager->incoming_handshakes.try_emplace(
            socket_address,
            &manager->handshake_mediator_,
            tr_peerIo::new_incoming(session, &session->top_bandwidth_, std::move(socket)),
            session->encryptionMode(),
            [manager](tr_handshake::Result const& result) { return on_handshake_done(manager, result); });
    }
}

size_t tr_peerMgrAddPex(tr_torrent* tor, tr_peer_from from, tr_pex const* pex, size_t n_pex)
{
    size_t n_used = 0;
    tr_swarm* s = tor->swarm;
    auto const lock = s->manager->unique_lock();

    for (tr_pex const* const end = pex + n_pex; pex != end; ++pex)
    {
        if (tr_isPex(pex) && /* safeguard against corrupt data */
            !s->manager->blocklists_.contains(pex->socket_address.address()) && pex->is_valid_for_peers(from) &&
            from != TR_PEER_FROM_INCOMING)
        {
            s->ensure_info_exists(pex->socket_address, pex->flags, from);
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
    size_t const n = compact_len / tr_socket_address::CompactSockAddrBytes[TR_AF_INET];
    auto const* walk = static_cast<std::byte const*>(compact);
    auto pex = std::vector<tr_pex>(n);

    for (size_t i = 0; i < n; ++i)
    {
        std::tie(pex[i].socket_address, walk) = tr_socket_address::from_compact_ipv4(walk);

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
    size_t const n = compact_len / tr_socket_address::CompactSockAddrBytes[TR_AF_INET6];
    auto const* walk = static_cast<std::byte const*>(compact);
    auto pex = std::vector<tr_pex>(n);

    for (size_t i = 0; i < n; ++i)
    {
        std::tie(pex[i].socket_address, walk) = tr_socket_address::from_compact_ipv6(walk);

        if (added_f != nullptr && n == added_f_len)
        {
            pex[i].flags = added_f[i];
        }
    }

    return pex;
}

// ---

namespace
{
namespace get_peers_helpers
{

[[nodiscard]] bool is_peer_interesting(tr_torrent const* tor, tr_peer_info const& info)
{
    TR_ASSERT(!std::empty(info.listen_port()));
    if (std::empty(info.listen_port()))
    {
        return false;
    }

    if (tor->is_done() && info.is_upload_only())
    {
        return false;
    }

    if (info.is_blocklisted(tor->session->blocklist()))
    {
        return false;
    }

    if (info.is_banned())
    {
        return false;
    }

    return true;
}

} // namespace get_peers_helpers
} // namespace

std::vector<tr_pex> tr_peerMgrGetPeers(tr_torrent const* tor, uint8_t address_type, uint8_t list_mode, size_t max_peer_count)
{
    using namespace get_peers_helpers;

    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    TR_ASSERT(address_type == TR_AF_INET || address_type == TR_AF_INET6);
    TR_ASSERT(list_mode == TR_PEERS_CONNECTED || list_mode == TR_PEERS_INTERESTING);

    tr_swarm const* s = tor->swarm;

    // build a list of peer info objects

    auto infos = std::vector<tr_peer_info const*>{};
    if (list_mode == TR_PEERS_CONNECTED) /* connected peers only */
    {
        auto const& peers = s->peers;
        infos.reserve(std::size(peers));
        for (auto const* peer : peers)
        {
            if (peer->socket_address().address().type == address_type)
            {
                infos.emplace_back(peer->peer_info.get());
            }
        }
    }
    else /* TR_PEERS_INTERESTING */
    {
        auto const& pool = s->connectable_pool;
        infos.reserve(std::size(pool));
        for (auto const& [socket_address, peer_info] : pool)
        {
            TR_ASSERT(socket_address == peer_info->listen_socket_address());
            if (socket_address.address().type == address_type && is_peer_interesting(tor, *peer_info))
            {
                infos.emplace_back(peer_info.get());
            }
        }
    }

    // add the N most useful peers into our return list

    auto const n = std::min(std::size(infos), max_peer_count);
    auto pex = std::vector<tr_pex>{};
    pex.reserve(n);

    std::partial_sort(std::begin(infos), std::begin(infos) + n, std::end(infos), CompareAtomsByUsefulness);
    infos.resize(n);

    for (auto const* const info : infos)
    {
        auto const& socket_address = info->listen_socket_address();
        [[maybe_unused]] auto const& addr = socket_address.address();

        TR_ASSERT(addr.is_valid());
        TR_ASSERT(addr.type == address_type);
        pex.emplace_back(socket_address, info->pex_flags());
    }

    std::sort(std::begin(pex), std::end(pex));
    return pex;
}

void tr_swarm::on_torrent_started()
{
    auto const lock = unique_lock();
    is_running = true;
    manager->rechokeSoon();
}

void tr_swarm::on_torrent_stopped()
{
    stop();
}

void tr_peerMgrAddTorrent(tr_peerMgr* manager, tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();
    TR_ASSERT(tor->swarm == nullptr);

    tor->swarm = new tr_swarm{ manager, tor };
}

int8_t tr_peerMgrPieceAvailability(tr_torrent const* tor, tr_piece_index_t piece)
{
    if (!tor->has_metainfo())
    {
        return 0;
    }

    if (tor->is_seed() || tor->has_piece(piece))
    {
        return -1;
    }

    auto const& peers = tor->swarm->peers;
    return std::count_if(std::begin(peers), std::end(peers), [piece](auto const* peer) { return peer->has_piece(piece); });
}

void tr_peerMgrTorrentAvailability(tr_torrent const* tor, int8_t* tab, unsigned int n_tabs)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tab != nullptr);
    TR_ASSERT(n_tabs > 0);

    std::fill_n(tab, n_tabs, int8_t{});

    auto const interval = tor->piece_count() / static_cast<float>(n_tabs);
    for (tr_piece_index_t i = 0; i < n_tabs; ++i)
    {
        auto const piece = static_cast<tr_piece_index_t>(i * interval);
        tab[i] = tr_peerMgrPieceAvailability(tor, piece);
    }
}

tr_swarm_stats tr_swarmGetStats(tr_swarm const* const swarm)
{
    TR_ASSERT(swarm != nullptr);

    auto count_active_peers = [&swarm](tr_direction dir)
    {
        return std::count_if(
            std::begin(swarm->peers),
            std::end(swarm->peers),
            [dir](auto const& peer) { return peer->is_active(dir); });
    };

    auto& stats = swarm->stats;
    stats.active_peer_count[TR_UP] = count_active_peers(TR_UP);
    stats.active_peer_count[TR_DOWN] = count_active_peers(TR_DOWN);
    stats.active_webseed_count = swarm->count_active_webseeds(tr_time_msec());
    return stats;
}

/* count how many bytes we want that connected peers have */
uint64_t tr_peerMgrGetDesiredAvailable(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    // common shortcuts...

    if (!tor->is_running() || tor->is_stopping() || tor->is_done() || !tor->has_metainfo())
    {
        return 0;
    }

    tr_swarm const* const swarm = tor->swarm;
    if (swarm == nullptr || std::empty(swarm->peers))
    {
        return 0;
    }

    auto available = tr_bitfield{ tor->piece_count() };
    for (auto const* const peer : swarm->peers)
    {
        available |= peer->has();
    }

    if (available.has_all())
    {
        return tor->left_until_done();
    }

    auto desired_available = uint64_t{};

    for (tr_piece_index_t i = 0, n = tor->piece_count(); i < n; ++i)
    {
        if (tor->piece_is_wanted(i) && available.test(i))
        {
            desired_available += tor->count_missing_bytes_in_piece(i);
        }
    }

    TR_ASSERT(desired_available <= tor->total_size());
    return desired_available;
}

tr_webseed_view tr_peerMgrWebseed(tr_torrent const* tor, size_t i)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm != nullptr);
    size_t const n = std::size(tor->swarm->webseeds);
    TR_ASSERT(i < n);

    return i >= n ? tr_webseed_view{} : tor->swarm->webseeds[i]->get_view();
}

namespace
{
namespace peer_stat_helpers
{

[[nodiscard]] auto get_peer_stats(tr_peerMsgs const* peer, time_t now, uint64_t now_msec)
{
    auto stats = tr_peer_stat{};

    auto const [addr, port] = peer->socket_address();

    addr.display_name(stats.addr, sizeof(stats.addr));
    stats.client = peer->user_agent().c_str();
    stats.port = port.host();
    stats.from = peer->peer_info->from_first();
    stats.progress = peer->percent_done();
    stats.isUTP = peer->is_utp_connection();
    stats.isEncrypted = peer->is_encrypted();
    stats.rateToPeer_KBps = peer->get_piece_speed(now_msec, TR_CLIENT_TO_PEER).count(Speed::Units::KByps);
    stats.rateToClient_KBps = peer->get_piece_speed(now_msec, TR_PEER_TO_CLIENT).count(Speed::Units::KByps);
    stats.peerIsChoked = peer->peer_is_choked();
    stats.peerIsInterested = peer->peer_is_interested();
    stats.clientIsChoked = peer->client_is_choked();
    stats.clientIsInterested = peer->client_is_interested();
    stats.isIncoming = peer->is_incoming_connection();
    stats.isDownloadingFrom = peer->is_active(TR_PEER_TO_CLIENT);
    stats.isUploadingTo = peer->is_active(TR_CLIENT_TO_PEER);
    stats.isSeed = peer->is_seed();

    stats.blocksToPeer = peer->blocks_sent_to_peer.count(now, CancelHistorySec);
    stats.blocksToClient = peer->blocks_sent_to_client.count(now, CancelHistorySec);
    stats.cancelsToPeer = peer->cancels_sent_to_peer.count(now, CancelHistorySec);
    stats.cancelsToClient = peer->cancels_sent_to_client.count(now, CancelHistorySec);

    stats.activeReqsToPeer = peer->active_req_count(TR_CLIENT_TO_PEER);
    stats.activeReqsToClient = peer->active_req_count(TR_PEER_TO_CLIENT);

    char* pch = stats.flagStr;

    if (stats.isUTP)
    {
        *pch++ = 'T';
    }

    if (peer->swarm->optimistic == peer)
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
} // namespace

tr_peer_stat* tr_peerMgrPeerStats(tr_torrent const* tor, size_t* setme_count)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm->manager != nullptr);

    auto const peers = tor->swarm->peers;
    auto const n = std::size(peers);
    auto* const ret = new tr_peer_stat[n];

    // TODO: re-implement as a callback solution (similar to tr_sessionSetCompletenessCallback) in case present call to run_in_session_thread is causing hangs when the peers info window is displayed.
    auto done_promise = std::promise<void>{};
    auto done_future = done_promise.get_future();
    tor->session->run_in_session_thread(
        [&peers, &ret, &done_promise]()
        {
            auto const now = tr_time();
            auto const now_msec = tr_time_msec();
            std::transform(
                std::begin(peers),
                std::end(peers),
                ret,
                [&now, &now_msec](auto const* peer) { return peer_stat_helpers::get_peer_stats(peer, now, now_msec); });
            done_promise.set_value();
        });
    done_future.wait();

    *setme_count = n;
    return ret;
}

namespace
{
namespace update_interest_helpers
{
/* does this peer have any pieces that we want? */
[[nodiscard]] bool isPeerInteresting(
    tr_torrent const* const tor,
    std::vector<bool> const& piece_is_interesting,
    tr_peerMsgs const* const peer)
{
    /* these cases should have already been handled by the calling code... */
    TR_ASSERT(!tor->is_done());
    TR_ASSERT(tor->client_can_download());

    if (peer->is_seed())
    {
        return true;
    }

    for (tr_piece_index_t i = 0; i < tor->piece_count(); ++i)
    {
        if (piece_is_interesting[i] && peer->has_piece(i))
        {
            return true;
        }
    }

    return false;
}

// determine which peers to show interest in
void updateInterest(tr_swarm* swarm)
{
    // sometimes this function isn't necessary
    auto const* const tor = swarm->tor;
    if (tor->is_done() || !tor->client_can_download())
    {
        return;
    }

    if (auto const& peers = swarm->peers; !std::empty(peers))
    {
        auto const n = tor->piece_count();

        // build a bitfield of interesting pieces...
        auto piece_is_interesting = std::vector<bool>(n);
        for (tr_piece_index_t i = 0U; i < n; ++i)
        {
            piece_is_interesting[i] = tor->piece_is_wanted(i) && !tor->has_piece(i);
        }

        for (auto* const peer : peers)
        {
            peer->set_interested(isPeerInteresting(tor, piece_is_interesting, peer));
        }
    }
}
} // namespace update_interest_helpers
} // namespace

// ---

namespace
{
namespace rechoke_uploads_helpers
{
struct ChokeData
{
    ChokeData(tr_peerMsgs* msgs, Speed rate, uint8_t salt, bool is_interested, bool was_choked, bool is_choked)
        : msgs_{ msgs }
        , rate_{ rate }
        , salt_{ salt }
        , is_interested_{ is_interested }
        , was_choked_{ was_choked }
        , is_choked_{ is_choked }
    {
    }

    tr_peerMsgs* msgs_;
    Speed rate_;
    uint8_t salt_;
    bool is_interested_;
    bool was_choked_;
    bool is_choked_;

    [[nodiscard]] constexpr auto compare(ChokeData const& that) const noexcept // <=>
    {
        // prefer higher overall speeds
        if (auto const val = tr_compare_3way(rate_, that.rate_); val != 0)
        {
            return -val;
        }

        if (was_choked_ != that.was_choked_) // prefer unchoked
        {
            return was_choked_ ? 1 : -1;
        }

        return tr_compare_3way(salt_, that.salt_);
    }

    [[nodiscard]] constexpr auto operator<(ChokeData const& that) const noexcept
    {
        return compare(that) < 0;
    }
};

/* get a rate for deciding which peers to choke and unchoke. */
[[nodiscard]] auto get_rate(tr_torrent const* tor, tr_peer const* peer, uint64_t now)
{
    if (tor->is_done())
    {
        return peer->get_piece_speed(now, TR_CLIENT_TO_PEER);
    }

    // downloading a private torrent... take upload speed into account
    // because there may only be a small window of opportunity to share
    if (tor->is_private())
    {
        return peer->get_piece_speed(now, TR_PEER_TO_CLIENT) + peer->get_piece_speed(now, TR_CLIENT_TO_PEER);
    }

    // downloading a public torrent
    return peer->get_piece_speed(now, TR_PEER_TO_CLIENT);
}

// an optimistically unchoked peer is immune from rechoking
// for this many calls to rechokeUploads().
auto constexpr OptimisticUnchokeMultiplier = uint8_t{ 4 };

void rechokeUploads(tr_swarm* s, uint64_t const now)
{
    auto const lock = s->unique_lock();

    auto& peers = s->peers;
    auto const peer_count = std::size(peers);
    auto choked = std::vector<ChokeData>{};
    choked.reserve(peer_count);
    auto const* const session = s->manager->session;
    bool const choke_all = !s->tor->client_can_upload();
    bool const is_maxed_out = s->tor->bandwidth().is_maxed_out(TR_UP, now);

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
        if (peer->is_seed())
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
                get_rate(s->tor, peer, now),
                salter(),
                peer->peer_is_interested(),
                peer->peer_is_choked(),
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

        item.is_choked_ = is_maxed_out ? item.was_choked_ : false;

        ++checked_choke_count;

        if (item.is_interested_)
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
            if (choked[i].is_interested_)
            {
                rand_pool.push_back(&choked[i]);
            }
        }

        if (auto const n = std::size(rand_pool); n != 0)
        {
            auto* c = rand_pool[tr_rand_int(n)];
            c->is_choked_ = false;
            s->optimistic = c->msgs_;
            s->optimistic_unchoke_time_scaler = OptimisticUnchokeMultiplier;
        }
    }

    for (auto& item : choked)
    {
        item.msgs_->set_choke(item.is_choked_);
    }
}
} // namespace rechoke_uploads_helpers
} // namespace

void tr_peerMgr::rechoke_pulse() const
{
    using namespace update_interest_helpers;
    using namespace rechoke_uploads_helpers;

    auto const lock = unique_lock();
    auto const now = tr_time_msec();

    for (auto* const tor : torrents_)
    {
        if (tor->is_running())
        {
            // possibly stop torrents that have seeded enough
            tor->stop_if_seed_limit_reached();
        }

        if (tor->is_running())
        {
            if (auto* const swarm = tor->swarm; swarm->stats.peer_count > 0)
            {
                rechokeUploads(swarm, now);
                updateInterest(swarm);
            }
        }
    }
}

// --- Life and Death

namespace
{
namespace disconnect_helpers
{
// when many peers are available, keep idle ones this long
auto constexpr MinUploadIdleSecs = time_t{ 60 };

// when few peers are available, keep idle ones this long
auto constexpr MaxUploadIdleSecs = time_t{ 60 * 5 };

[[nodiscard]] bool shouldPeerBeClosed(tr_swarm const* s, tr_peerMsgs const* peer, size_t peer_count, time_t const now)
{
    /* if it's marked for purging, close it */
    if (peer->is_disconnecting())
    {
        tr_logAddTraceSwarm(s, fmt::format("purging peer {} because its is_disconnecting_ flag is set", peer->display_name()));
        return true;
    }

    auto const* tor = s->tor;
    auto const& info = peer->peer_info;

    /* disconnect if we're both seeds and enough time has passed for PEX */
    if (tor->is_done() && peer->is_seed())
    {
        return !tor->allows_pex() || info->idle_secs(now).value_or(0U) >= 30U;
    }

    /* disconnect if it's been too long since piece data has been transferred.
     * this is on a sliding scale based on number of available peers... */
    {
        auto const relax_strictness_if_fewer_than_n = static_cast<size_t>(std::lround(tor->peer_limit() * 0.9));
        /* if we have >= relaxIfFewerThan, strictness is 100%.
         * if we have zero connections, strictness is 0% */
        float const strictness = peer_count >= relax_strictness_if_fewer_than_n ?
            1.0 :
            peer_count / (float)relax_strictness_if_fewer_than_n;
        auto const lo = MinUploadIdleSecs;
        auto const hi = MaxUploadIdleSecs;
        time_t const limit = hi - ((hi - lo) * strictness);

        if (auto const idle_secs = info->idle_secs(now); idle_secs && *idle_secs > limit)
        {
            tr_logAddTraceSwarm(
                s,
                fmt::format(
                    "purging peer {} because it's been {} secs since we shared anything",
                    peer->display_name(),
                    *idle_secs));
            return true;
        }
    }

    return false;
}

void close_peer(tr_peerMsgs* peer)
{
    TR_ASSERT(peer != nullptr);
    peer->swarm->remove_peer(peer);
}

constexpr struct
{
    [[nodiscard]] static int compare(tr_peerMsgs const* a, tr_peerMsgs const* b) // <=>
    {
        if (a->is_disconnecting() != b->is_disconnecting())
        {
            return a->is_disconnecting() ? 1 : -1;
        }

        return -a->peer_info->compare_by_piece_data_time(*b->peer_info);
    }

    [[nodiscard]] bool operator()(tr_peerMsgs const* a, tr_peerMsgs const* b) const // less than
    {
        return compare(a, b) < 0;
    }
} ComparePeerByMostActive{};

constexpr auto ComparePeerByLeastActive = [](tr_peerMsgs const* a, tr_peerMsgs const* b)
{
    return ComparePeerByMostActive(b, a);
};

using bad_peers_t = small::vector<tr_peerMsgs*, 512U>;

bad_peers_t& get_peers_to_close(tr_swarm const* const swarm, time_t const now_sec, bad_peers_t& bad_peers_buf)
{
    auto const& peers = swarm->peers;
    auto const peer_count = std::size(peers);

    bad_peers_buf.clear();
    bad_peers_buf.reserve(peer_count);
    for (auto* peer : swarm->peers)
    {
        if (shouldPeerBeClosed(swarm, peer, peer_count, now_sec))
        {
            bad_peers_buf.emplace_back(peer);
        }
    }

    return bad_peers_buf;
}

void close_bad_peers(tr_swarm* s, time_t const now_sec, bad_peers_t& bad_peers_buf)
{
    for (auto* peer : get_peers_to_close(s, now_sec, bad_peers_buf))
    {
        tr_logAddTraceSwarm(peer->swarm, fmt::format("removing bad peer {}", peer->display_name()));
        close_peer(peer);
    }
}

void enforceSwarmPeerLimit(tr_swarm* swarm, size_t max)
{
    // do we have too many peers?
    auto const n = swarm->peerCount();
    if (n <= max)
    {
        return;
    }

    // close all but the `max` most active
    auto peers = std::vector<tr_peerMsgs*>(n - max);
    std::partial_sort_copy(
        std::begin(swarm->peers),
        std::end(swarm->peers),
        std::begin(peers),
        std::end(peers),
        ComparePeerByLeastActive);
    std::for_each(std::begin(peers), std::end(peers), close_peer);
}

void enforceSessionPeerLimit(size_t global_peer_limit, tr_torrents& torrents)
{
    // if we're under the limit, then no action needed
    auto const current_size = tr_peerMsgs::size();
    if (current_size <= global_peer_limit)
    {
        return;
    }

    // make a list of all the peers
    auto peers = std::vector<tr_peerMsgs*>{};
    peers.reserve(current_size);
    for (auto const* const tor : torrents)
    {
        peers.insert(std::end(peers), std::begin(tor->swarm->peers), std::end(tor->swarm->peers));
    }

    TR_ASSERT(current_size == std::size(peers));
    if (std::size(peers) > global_peer_limit)
    {
        std::partial_sort(std::begin(peers), std::begin(peers) + global_peer_limit, std::end(peers), ComparePeerByMostActive);
        std::for_each(std::begin(peers) + global_peer_limit, std::end(peers), close_peer);
    }
}
} // namespace disconnect_helpers
} // namespace

void tr_peerMgr::reconnect_pulse()
{
    using namespace disconnect_helpers;

    auto const lock = unique_lock();
    auto const now_sec = tr_time();

    // remove crappy peers
    auto bad_peers_buf = bad_peers_t{};
    for (auto* const tor : torrents_)
    {
        auto* const swarm = tor->swarm;

        if (!swarm->is_running)
        {
            swarm->remove_all_peers();
        }
        else
        {
            close_bad_peers(swarm, now_sec, bad_peers_buf);
        }
    }

    // if we're over the per-torrent peer limits, cull some peers
    for (auto* const tor : torrents_)
    {
        if (tor->is_running())
        {
            enforceSwarmPeerLimit(tor->swarm, tor->peer_limit());
        }
    }

    // if we're over the per-session peer limits, cull some peers
    enforceSessionPeerLimit(session->peerLimit(), torrents_);

    // try to make new peer connections
    make_new_peer_connections();
}

// --- Peer Pool Size

namespace
{
namespace peer_info_pulse_helpers
{
auto get_max_peer_info_count(tr_torrent const& tor)
{
    return tor.is_done() ? tor.peer_limit() : tor.peer_limit() * 3U;
}

struct ComparePeerInfo
{
    [[nodiscard]] int compare(tr_peer_info const& a, tr_peer_info const& b) const noexcept
    {
        auto const is_a_inactive = a.is_inactive(now_);
        auto const is_b_inactive = b.is_inactive(now_);
        if (is_a_inactive != is_b_inactive)
        {
            return is_a_inactive ? 1 : -1;
        }

        return CompareAtomsByUsefulness.compare(a, b);
    }

    template<typename T>
    [[nodiscard]] std::enable_if_t<std::is_same_v<std::decay_t<decltype(*std::declval<T>())>, tr_peer_info>, bool> operator()(
        T const& a,
        T const& b) const noexcept
    {
        return compare(*a, *b) < 0;
    }

    time_t const now_ = tr_time();
};
} // namespace peer_info_pulse_helpers
} // namespace

void tr_peerMgr::peer_info_pulse()
{
    using namespace peer_info_pulse_helpers;

    auto const lock = unique_lock();
    for (auto const* tor : torrents_)
    {
        auto& pool = tor->swarm->connectable_pool;
        auto const max = get_max_peer_info_count(*tor);
        auto const pool_size = std::size(pool);
        if (pool_size <= max)
        {
            continue;
        }

        auto infos = std::vector<std::shared_ptr<tr_peer_info>>{};
        infos.reserve(pool_size);
        std::transform(
            std::begin(pool),
            std::end(pool),
            std::back_inserter(infos),
            [](auto const& keyval) { return keyval.second; });
        pool.clear();

        // Keep all peer info objects before test_begin unconditionally
        auto const test_begin = std::partition(
            std::begin(infos),
            std::end(infos),
            [](auto const& info) { return info->is_in_use(); });

        auto const iter_max = std::begin(infos) + max;
        if (iter_max > test_begin)
        {
            std::partial_sort(test_begin, iter_max, std::end(infos), ComparePeerInfo{});
        }
        infos.erase(std::max(test_begin, iter_max), std::end(infos));

        pool.reserve(std::size(infos));
        for (auto& info : infos)
        {
            pool.try_emplace(info->listen_socket_address(), std::move(info));
        }

        tr_logAddTraceSwarm(
            tor->swarm,
            fmt::format("max peer info count is {}... pruned from {} to {}", max, pool_size, std::size(pool)));
    }
}

// --- Bandwidth Allocation

namespace
{
namespace bandwidth_helpers
{
void pumpAllPeers(tr_peerMgr* mgr)
{
    for (auto* const tor : mgr->torrents_)
    {
        for (auto* const peer : tor->swarm->peers)
        {
            peer->pulse();
        }
    }
}
} // namespace bandwidth_helpers
} // namespace

void tr_peerMgr::bandwidth_pulse()
{
    using namespace bandwidth_helpers;

    auto const lock = unique_lock();

    pumpAllPeers(this);

    // allocate bandwidth to the peers
    static auto constexpr Msec = std::chrono::duration_cast<std::chrono::milliseconds>(BandwidthTimerPeriod).count();
    session->top_bandwidth_.allocate(Msec);

    // torrent upkeep
    for (auto* const tor : torrents_)
    {
        tor->do_idle_work();
    }

    reconnect_pulse();
}

// ---

namespace
{
namespace connect_helpers
{
/* is this atom someone that we'd want to initiate a connection to? */
[[nodiscard]] bool is_peer_candidate(tr_torrent const* tor, tr_peer_info const& peer_info, time_t const now)
{
    // not if we're both upload only and pex is disabled
    if (tor->is_done() && peer_info.is_upload_only() && !tor->allows_pex())
    {
        return false;
    }

    // not if we've already got a connection to them...
    if (peer_info.is_in_use())
    {
        return false;
    }

    // not if we just tried them already
    if (!peer_info.reconnect_interval_has_passed(now))
    {
        return false;
    }

    // not if they're blocklisted
    if (peer_info.is_blocklisted(tor->session->blocklist()))
    {
        return false;
    }

    // not if they're banned...
    if (peer_info.is_banned())
    {
        return false;
    }

    return true;
}

[[nodiscard]] constexpr uint64_t addValToKey(uint64_t value, unsigned int width, uint64_t addme)
{
    value <<= width;
    value |= addme;
    return value;
}

/* smaller value is better */
[[nodiscard]] uint64_t getPeerCandidateScore(tr_torrent const* tor, tr_peer_info const& peer_info, uint8_t salt)
{
    auto i = uint64_t{};
    auto score = uint64_t{};

    /* prefer peers we've exchanged piece data with, or never tried, over other peers. */
    i = peer_info.fruitless_connection_count() != 0U ? 1U : 0U;
    score = addValToKey(score, 1U, i);

    /* prefer the one we attempted least recently (to cycle through all peers) */
    i = peer_info.connection_attempt_time();
    score = addValToKey(score, 32U, i);

    /* prefer peers belonging to a torrent of a higher priority */
    switch (tor->get_priority())
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

    default:
        TR_ASSERT_MSG(false, "invalid priority");
        break;
    }

    score = addValToKey(score, 2U, i);

    // prefer recently-started torrents
    i = tor->started_recently(tr_time()) ? 0 : 1;
    score = addValToKey(score, 1U, i);

    /* prefer torrents we're downloading with */
    i = tor->is_done() ? 1 : 0;
    score = addValToKey(score, 1U, i);

    /* prefer peers that are known to be connectible */
    i = peer_info.is_connectable().value_or(false) ? 0 : 1;
    score = addValToKey(score, 1U, i);

    /* prefer peers that we might be able to upload to */
    i = peer_info.is_upload_only() ? 1 : 0;
    score = addValToKey(score, 1U, i);

    /* Prefer peers that we got from more trusted sources.
     * lower `fromBest` values indicate more trusted sources */
    score = addValToKey(score, 4U, peer_info.from_best()); // TODO(tearfur): use std::bit_width(TR_PEER_FROM__MAX - 1)

    /* salt */
    score = addValToKey(score, 8U, salt);

    return score;
}

void get_peer_candidates(size_t global_peer_limit, tr_torrents& torrents, tr_peerMgr::OutboundCandidates& setme)
{
    struct peer_candidate
    {
        peer_candidate() = default;

        peer_candidate(uint64_t score_in, tr_torrent const* const tor_in, tr_peer_info const* const peer_info_in)
            : score{ score_in }
            , tor{ tor_in }
            , peer_info{ peer_info_in }
        {
        }

        uint64_t score;
        tr_torrent const* tor;
        tr_peer_info const* peer_info;
    };

    setme.clear();

    auto const now = tr_time();
    auto const now_msec = tr_time_msec();

    // leave 5% of connection slots for incoming connections -- ticket #2609
    if (auto const max_candidates = static_cast<size_t>(global_peer_limit * 0.95); max_candidates <= tr_peerMsgs::size())
    {
        return;
    }

    auto candidates = std::vector<peer_candidate>{};
    candidates.reserve(tr_peer_info::known_connectable_count());

    /* populate the candidate array */
    auto salter = tr_salt_shaker{};
    for (auto* const tor : torrents)
    {
        auto* const swarm = tor->swarm;

        if (!swarm->is_running)
        {
            continue;
        }

        /* if everyone in the swarm is upload only and pex is disabled,
         * then don't initiate connections */
        bool const seeding = tor->is_done();
        if (seeding && swarm->is_all_upload_only() && !tor->allows_pex())
        {
            continue;
        }

        /* if we've already got enough peers in this torrent... */
        if (tor->peer_limit() <= swarm->peerCount())
        {
            continue;
        }

        /* if we've already got enough speed in this torrent... */
        if (seeding && tor->bandwidth().is_maxed_out(TR_UP, now_msec))
        {
            continue;
        }

        for (auto const& [socket_address, peer_info] : swarm->connectable_pool)
        {
            if (is_peer_candidate(tor, *peer_info, now))
            {
                candidates.emplace_back(getPeerCandidateScore(tor, *peer_info, salter()), tor, peer_info.get());
            }
        }
    }

    // only keep the best `max` candidates
    auto const n_keep = std::min(tr_peerMgr::OutboundCandidates::requested_inline_size, std::size(candidates));
    std::partial_sort(
        std::begin(candidates),
        std::begin(candidates) + n_keep,
        std::end(candidates),
        [](auto const& a, auto const& b) { return a.score < b.score; });
    candidates.resize(n_keep);

    // put the best candidates at the end of the list
    for (auto it = std::crbegin(candidates), end = std::crend(candidates); it != end; ++it)
    {
        setme.emplace_back(it->tor->id(), it->peer_info->listen_socket_address());
    }
}

void initiate_connection(tr_peerMgr* mgr, tr_swarm* s, tr_peer_info& peer_info)
{
    using namespace handshake_helpers;

    auto const now = tr_time();
    auto const utp = mgr->session->allowsUTP() && peer_info.supports_utp().value_or(true);
    auto* const session = mgr->session;

    if (tr_peer_socket::limit_reached(session) || (!utp && !session->allowsTCP()))
    {
        return;
    }

    tr_logAddTraceSwarm(
        s,
        fmt::format("Starting an OUTGOING {} connection with {}", utp ? " µTP" : "TCP", peer_info.display_name()));

    auto peer_io = tr_peerIo::new_outgoing(
        session,
        &session->top_bandwidth_,
        peer_info.listen_socket_address(),
        s->tor->info_hash(),
        s->tor->is_seed(),
        utp);

    if (!peer_io)
    {
        tr_logAddTraceSwarm(s, fmt::format("peerIo not created; marking peer {} as unreachable", peer_info.display_name()));
        peer_info.set_connectable(false);
        peer_info.on_fruitless_connection();
    }
    else
    {
        peer_info.start_handshake(
            &mgr->handshake_mediator_,
            peer_io,
            session->encryptionMode(),
            [mgr](tr_handshake::Result const& result) { return on_handshake_done(mgr, result); });
    }

    peer_info.set_connection_attempt_time(now);
}
} // namespace connect_helpers
} // namespace

void tr_peerMgr::make_new_peer_connections()
{
    using namespace connect_helpers;

    auto const lock = unique_lock();

    // get the candidates if we need to
    auto& candidates = outbound_candidates_;
    if (std::empty(candidates))
    {
        get_peer_candidates(session->peerLimit(), torrents_, candidates);
    }

    // initiate connections to the last N candidates
    auto const n_this_pass = std::min(std::size(candidates), MaxConnectionsPerPulse);
    for (auto it = std::crbegin(candidates), end = std::crbegin(candidates) + n_this_pass; it != end; ++it)
    {
        auto const& [tor_id, sock_addr] = *it;

        if (auto* const tor = torrents_.get(tor_id); tor != nullptr)
        {
            if (auto const& peer_info = tor->swarm->get_existing_peer_info(sock_addr))
            {
                initiate_connection(this, tor->swarm, *peer_info);
            }
        }
    }

    // remove the N candidates that we just consumed
    candidates.resize(std::size(candidates) - n_this_pass);
}

void HandshakeMediator::set_utp_failed(tr_sha1_digest_t const& info_hash, tr_socket_address const& socket_address)
{
    if (auto* const tor = torrents_.get(info_hash); tor != nullptr)
    {
        if (auto const& peer_info = tor->swarm->get_existing_peer_info(socket_address))
        {
            peer_info->set_utp_supported(false);
        }
    }
}
