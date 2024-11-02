// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <array>
#include <cstddef> // size_t
#include <cstdint> // uint8_t, uint32_t, uint64_t
#include <string>

#include "libtransmission/transmission.h"

#include "libtransmission/bitfield.h"
#include "libtransmission/block-info.h"
#include "libtransmission/history.h"
#include "libtransmission/net.h" // tr_port

/**
 * @addtogroup peers Peers
 * @{
 */

class tr_swarm;
struct tr_bandwidth;
struct tr_peer;

// --- Peer Publish / Subscribe

class tr_peer_event
{
public:
    enum class Type
    {
        // Unless otherwise specified, all events are for BT peers only
        ClientGotBlock, // applies to webseed too
        ClientGotChoke,
        ClientGotPieceData, // applies to webseed too
        ClientGotAllowedFast,
        ClientGotSuggest,
        ClientGotPort,
        ClientGotRej, // applies to webseed too
        ClientGotBitfield,
        ClientGotHave,
        ClientGotHaveAll,
        ClientGotHaveNone,
        ClientSentPieceData,
        Error // generic
    };

    Type type = Type::Error;

    tr_bitfield* bitfield = nullptr; // for GotBitfield
    uint32_t pieceIndex = 0; // for GotBlock, GotHave, Cancel, Allowed, Suggest
    uint32_t offset = 0; // for GotBlock
    uint32_t length = 0; // for GotBlock, GotPieceData
    int err = 0; // errno for GotError
    tr_port port = {}; // for GotPort

    [[nodiscard]] constexpr static auto GotBlock(tr_block_info const& block_info, tr_block_index_t block) noexcept
    {
        auto const loc = block_info.block_loc(block);
        auto event = tr_peer_event{};
        event.type = Type::ClientGotBlock;
        event.pieceIndex = loc.piece;
        event.offset = loc.piece_offset;
        event.length = block_info.block_size(block);
        return event;
    }

    [[nodiscard]] constexpr static auto GotAllowedFast(tr_piece_index_t piece) noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotAllowedFast;
        event.pieceIndex = piece;
        return event;
    }

    [[nodiscard]] constexpr static auto GotBitfield(tr_bitfield* bitfield) noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotBitfield;
        event.bitfield = bitfield;
        return event;
    }

    [[nodiscard]] constexpr static auto GotChoke() noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotChoke;
        return event;
    }

    [[nodiscard]] constexpr static auto GotError(int err) noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::Error;
        event.err = err;
        return event;
    }

    [[nodiscard]] constexpr static auto GotHave(tr_piece_index_t piece) noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotHave;
        event.pieceIndex = piece;
        return event;
    }

    [[nodiscard]] constexpr static auto GotHaveAll() noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotHaveAll;
        return event;
    }

    [[nodiscard]] constexpr static auto GotHaveNone() noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotHaveNone;
        return event;
    }

    [[nodiscard]] constexpr static auto GotPieceData(uint32_t length) noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotPieceData;
        event.length = length;
        return event;
    }

    [[nodiscard]] constexpr static auto GotPort(tr_port port) noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotPort;
        event.port = port;
        return event;
    }

    [[nodiscard]] constexpr static auto GotRejected(tr_block_info const& block_info, tr_block_index_t block) noexcept
    {
        auto const loc = block_info.block_loc(block);
        auto event = tr_peer_event{};
        event.type = Type::ClientGotRej;
        event.pieceIndex = loc.piece;
        event.offset = loc.piece_offset;
        event.length = block_info.block_size(block);
        return event;
    }

    [[nodiscard]] constexpr static auto GotSuggest(tr_piece_index_t piece) noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientGotSuggest;
        event.pieceIndex = piece;
        return event;
    }

    [[nodiscard]] constexpr static auto SentPieceData(uint32_t length) noexcept
    {
        auto event = tr_peer_event{};
        event.type = Type::ClientSentPieceData;
        event.length = length;
        return event;
    }
};

using tr_peer_callback_generic = void (*)(tr_peer* peer, tr_peer_event const& event, void* client_data);

/**
 * State information about a connected peer.
 *
 * @see tr_peer_info
 * @see tr_peerMsgs
 */
struct tr_peer
{
    using Speed = libtransmission::Values::Speed;

    explicit tr_peer(tr_torrent const& tor);
    virtual ~tr_peer();

    [[nodiscard]] virtual Speed get_piece_speed(uint64_t now, tr_direction direction) const = 0;

    [[nodiscard]] bool has_piece(tr_piece_index_t piece) const noexcept
    {
        return has().test(piece);
    }

    [[nodiscard]] float percent_done() const noexcept
    {
        return has().percent();
    }

    [[nodiscard]] bool is_seed() const noexcept
    {
        return has().has_all();
    }

    [[nodiscard]] virtual std::string display_name() const = 0;

    [[nodiscard]] virtual tr_bitfield const& has() const noexcept = 0;

    // requests that have been made but haven't been fulfilled yet
    [[nodiscard]] virtual size_t active_req_count(tr_direction) const noexcept = 0;

    virtual void request_blocks(tr_block_span_t const* block_spans, size_t n_spans) = 0;

    virtual void cancel_block_request(tr_block_index_t /*block*/)
    {
    }

    virtual void ban() = 0;

    tr_session* const session;

    tr_swarm* const swarm;

    tr_recentHistory<uint16_t> blocks_sent_to_peer;

    tr_recentHistory<uint16_t> cancels_sent_to_client;

    /// The following fields are only to be used in peer-mgr.cc.
    /// TODO(ckerr): refactor them out of `tr_peer`

    // whether or not this peer sent us any given block
    tr_bitfield blame;

    // how many bad pieces this piece has contributed to
    uint8_t strikes = 0;

    // how many blocks this peer has sent us
    tr_recentHistory<uint16_t> blocks_sent_to_client;

    // how many requests we made to this peer and then canceled
    tr_recentHistory<uint16_t> cancels_sent_to_peer;
};

// ---

struct tr_swarm_stats
{
    std::array<uint16_t, 2> active_peer_count;
    uint16_t active_webseed_count;
    // connected peers
    uint16_t peer_count;
    // connected peers by peer source
    std::array<uint16_t, TR_PEER_FROM__MAX> peer_from_count;
    // known peers by peer source
    std::array<uint16_t, TR_PEER_FROM__MAX> known_peer_from_count;
};

tr_swarm_stats tr_swarmGetStats(tr_swarm const* swarm);

// ---

#ifdef _WIN32
#undef EMSGSIZE
#define EMSGSIZE WSAEMSGSIZE
#endif

/** @} */
