// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <array>
#include <cstdint> // uint8_t, uint32_t, uint64_t
#include <functional>

#include "transmission.h"

#include "torrent.h"
#include "tr-assert.h"
#include "bitfield.h"
#include "block-info.h"
#include "history.h"
#include "interned-string.h"
#include "net.h" // tr_port

/**
 * @addtogroup peers Peers
 * @{
 */

class tr_peer;
class tr_swarm;
struct peer_atom;
struct tr_bandwidth;

// --- Peer Publish / Subscribe

class tr_peer_event
{
public:
    enum class Type
    {
        ClientGotBlock,
        ClientGotChoke,
        ClientGotPieceData,
        ClientGotAllowedFast,
        ClientGotSuggest,
        ClientGotPort,
        ClientGotRej,
        ClientGotBitfield,
        ClientGotHave,
        ClientGotHaveAll,
        ClientGotHaveNone,
        ClientSentPieceData,
        Error
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
        auto const loc = block_info.blockLoc(block);
        auto event = tr_peer_event{};
        event.type = Type::ClientGotBlock;
        event.pieceIndex = loc.piece;
        event.offset = loc.piece_offset;
        event.length = block_info.blockSize(block);
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
        auto const loc = block_info.blockLoc(block);
        auto event = tr_peer_event{};
        event.type = Type::ClientGotRej;
        event.pieceIndex = loc.piece;
        event.offset = loc.piece_offset;
        event.length = block_info.blockSize(block);
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

using tr_peer_callback = std::function<void(tr_peer&, tr_peer_event const&)>;

/**
 * State information about a connected peer.
 *
 * @see struct peer_atom
 * @see tr_peerMsgs
 */
class tr_peer
{
public:
    tr_peer(tr_torrent* tor, peer_atom* atom = nullptr);
    virtual ~tr_peer();

    virtual bool isTransferringPieces(uint64_t now, tr_direction dir, tr_bytes_per_second_t* setme_bytes_per_second) const = 0;

    [[nodiscard]] bool hasPiece(tr_piece_index_t piece) const noexcept
    {
        return has().test(piece);
    }

    [[nodiscard]] float percentDone() const noexcept
    {
        return has().percent();
    }

    [[nodiscard]] bool isSeed() const noexcept
    {
        return has().hasAll();
    }

    [[nodiscard]] virtual std::string display_name() const = 0;

    [[nodiscard]] virtual tr_bitfield const& has() const noexcept = 0;

    [[nodiscard]] virtual tr_bandwidth& bandwidth() noexcept = 0;

    // requests that have been made but haven't been fulfilled yet
    [[nodiscard]] virtual size_t activeReqCount(tr_direction) const noexcept = 0;

    [[nodiscard]] tr_bytes_per_second_t get_piece_speed_bytes_per_second(uint64_t now, tr_direction direction) const
    {
        auto bytes_per_second = tr_bytes_per_second_t{};
        isTransferringPieces(now, direction, &bytes_per_second);
        return bytes_per_second;
    }

    [[nodiscard]] tr_session* getSession() const noexcept
    {
        TR_ASSERT(torrent != nullptr);
        return torrent->session;
    }

    [[nodiscard]] tr_swarm* getSwarm() const noexcept
    {
        TR_ASSERT(torrent != nullptr);
        return torrent->swarm;
    }

    void clientCallback(tr_peer_event const& event);

    virtual void requestBlocks(tr_block_span_t const* block_spans, size_t n_spans) = 0;

    struct RequestLimit
    {
        // How many blocks we could request.
        size_t max_spans = 0;

        // How many spans those blocks could be in.
        // This is for webseeds, which make parallel requests.
        size_t max_blocks = 0;
    };

    // how many blocks could we request from this peer right now?
    [[nodiscard]] virtual RequestLimit canRequest() const noexcept = 0;

    tr_torrent* const torrent;

    tr_recentHistory<uint16_t> blocks_sent_to_peer;

    tr_recentHistory<uint16_t> cancels_sent_to_client;

    /// The following fields are only to be used in peer-mgr.cc.
    /// TODO(ckerr): refactor them out of `tr_peer`

    // hook to private peer-mgr information
    peer_atom* const atom;

    // whether or not this peer sent us any given block
    tr_bitfield blame;

    // whether or not we should free this peer soon.
    bool do_purge = false;

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
    uint16_t peer_count;
    std::array<uint16_t, TR_PEER_FROM__MAX> peer_from_count;
};

tr_swarm_stats tr_swarmGetStats(tr_swarm const* swarm);

void tr_swarmIncrementActivePeers(tr_swarm* swarm, tr_direction direction, bool is_active);

// ---

#ifdef _WIN32
#undef EMSGSIZE
#define EMSGSIZE WSAEMSGSIZE
#endif

/** @} */
