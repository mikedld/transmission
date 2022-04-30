// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <memory> // std::unique_ptr
#include <optional>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>

#include <fmt/format.h>

#include "transmission.h"

#include "cache.h"
#include "completion.h"
#include "file.h"
#include "log.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-msgs.h"
#include "ptrarray.h"
#include "quark.h"
#include "session.h"
#include "torrent-magnet.h"
#include "torrent.h"
#include "tr-assert.h"
#include "tr-dht.h"
#include "utils.h"
#include "variant.h"
#include "version.h"

#ifndef EBADMSG
#define EBADMSG EINVAL
#endif

/**
***
**/

// these values are hardcoded by various BEPs as noted

namespace BtPeerMsgs
{

// http://bittorrent.org/beps/bep_0003.html#peer-messages
auto constexpr Choke = uint8_t{ 0 };
auto constexpr Unchoke = uint8_t{ 1 };
auto constexpr Interested = uint8_t{ 2 };
auto constexpr NotInterested = uint8_t{ 3 };
auto constexpr Have = uint8_t{ 4 };
auto constexpr Bitfield = uint8_t{ 5 };
auto constexpr Request = uint8_t{ 6 };
auto constexpr Piece = uint8_t{ 7 };
auto constexpr Cancel = uint8_t{ 8 };

// http://bittorrent.org/beps/bep_0005.html
auto constexpr Port = uint8_t{ 9 };

// https://www.bittorrent.org/beps/bep_0006.html
auto constexpr FextSuggest = uint8_t{ 13 };
auto constexpr FextHaveAll = uint8_t{ 14 };
auto constexpr FextHaveNone = uint8_t{ 15 };
auto constexpr FextReject = uint8_t{ 16 };
auto constexpr FextAllowedFast = uint8_t{ 17 };

// http://bittorrent.org/beps/bep_0010.html
// see also LtepMessageIds below
auto constexpr Ltep = uint8_t{ 20 };

} // namespace BtPeerMsgs

namespace LtepMessages
{

// http://bittorrent.org/beps/bep_0010.html
auto constexpr Handshake = uint8_t{ 0 };

} // namespace LtepMessages

// http://bittorrent.org/beps/bep_0010.html
// Client-defined extension message IDs that we tell peers about
// in the LTEP handshake and will respond to when sent in an LTEP
// message.
enum LtepMessageIds
{
    // we support peer exchange (bep 11)
    // https://www.bittorrent.org/beps/bep_0011.html
    UT_PEX_ID = 1,

    // we support sending metadata files (bep 9)
    // https://www.bittorrent.org/beps/bep_0009.html
    // see also MetadataMsgType below
    UT_METADATA_ID = 3,
};

// http://bittorrent.org/beps/bep_0009.html
namespace MetadataMsgType
{

auto constexpr Request = int{ 0 };
auto constexpr Data = int{ 1 };
auto constexpr Reject = int{ 2 };

} // namespace MetadataMsgType

// seconds between sendPex() calls
static auto constexpr PexIntervalSecs = int{ 90 };

static auto constexpr MinChokePeriodSec = int{ 10 };

// idle seconds before we send a keepalive
static auto constexpr KeepaliveIntervalSecs = int{ 100 };

static auto constexpr MetadataReqQ = int{ 64 };

static auto constexpr ReqQ = int{ 512 };

// used in lowering the outMessages queue period
static auto constexpr ImmediatePriorityIntervalSecs = int{ 0 };
static auto constexpr HighPriorityIntervalSecs = int{ 2 };
static auto constexpr LowPriorityIntervalSecs = int{ 10 };

// how many blocks to keep prefetched per peer
static auto constexpr PrefetchMax = size_t{ 18 };

// when we're making requests from another peer,
// batch them together to send enough requests to
// meet our bandwidth goals for the next N seconds
static auto constexpr RequestBufSecs = int{ 10 };

namespace
{

auto constexpr MaxPexPeerCount = size_t{ 50 };

} // unnamed namespace

enum class AwaitingBt
{
    Length,
    Id,
    Message,
    Piece
};

enum class EncryptionPreference
{
    Unknown,
    Yes,
    No
};

/**
***
**/

struct peer_request
{
    uint32_t index = 0;
    uint32_t offset = 0;
    uint32_t length = 0;

    [[nodiscard]] auto constexpr operator==(peer_request const& that) const noexcept
    {
        return this->index == that.index && this->offset == that.offset && this->length == that.length;
    }
};

static peer_request blockToReq(tr_torrent const* tor, tr_block_index_t block)
{
    auto const loc = tor->blockLoc(block);
    return peer_request{ loc.piece, loc.piece_offset, tor->blockSize(block) };
}

/**
***
**/

/* this is raw, unchanged data from the peer regarding
 * the current message that it's sending us. */
struct tr_incoming
{
    uint8_t id = 0;
    uint32_t length = 0; /* includes the +1 for id length */
    struct peer_request blockReq = {}; /* metadata for incoming blocks */
    struct evbuffer* block = nullptr; /* piece data for incoming blocks */
};

class tr_peerMsgsImpl;
// TODO: make these to be member functions
static ReadState canRead(tr_peerIo* io, void* vmsgs, size_t* piece);
static void cancelAllRequestsToClient(tr_peerMsgsImpl* msgs);
static void didWrite(tr_peerIo* io, size_t bytesWritten, bool wasPieceData, void* vmsgs);
static void gotError(tr_peerIo* io, short what, void* vmsgs);
static void peerPulse(void* vmsgs);
static void pexPulse(evutil_socket_t fd, short what, void* vmsgs);
static void protocolSendCancel(tr_peerMsgsImpl* msgs, struct peer_request const& req);
static void protocolSendChoke(tr_peerMsgsImpl* msgs, bool choke);
static void protocolSendHave(tr_peerMsgsImpl* msgs, tr_piece_index_t index);
static void protocolSendPort(tr_peerMsgsImpl* msgs, tr_port port);
static void sendInterest(tr_peerMsgsImpl* msgs, bool b);
static void sendLtepHandshake(tr_peerMsgsImpl* msgs);
static void tellPeerWhatWeHave(tr_peerMsgsImpl* msgs);
static void updateDesiredRequestCount(tr_peerMsgsImpl* msgs);
//zzz

struct EventDeleter
{
    void operator()(struct event* ev) const
    {
        event_free(ev);
    }
};

using UniqueTimer = std::unique_ptr<struct event, EventDeleter>;

/**
 * Low-level communication state information about a connected peer.
 *
 * This structure remembers the low-level protocol states that we're
 * in with this peer, such as active requests, pex messages, and so on.
 * Its fields are all private to peer-msgs.c.
 *
 * Data not directly involved with sending & receiving messages is
 * stored in tr_peer, where it can be accessed by both peermsgs and
 * the peer manager.
 *
 * @see struct peer_atom
 * @see tr_peer
 */
class tr_peerMsgsImpl final : public tr_peerMsgs
{
public:
    tr_peerMsgsImpl(tr_torrent* torrent_in, peer_atom* atom_in, tr_peerIo* io_in, tr_peer_callback callback, void* callbackData)
        : tr_peerMsgs{ torrent_in, atom_in }
        , outMessagesBatchPeriod{ LowPriorityIntervalSecs }
        , torrent{ torrent_in }
        , outMessages{ evbuffer_new() }
        , io{ io_in }
        , callback_{ callback }
        , callbackData_{ callbackData }
    {
        if (torrent->allowsPex())
        {
            pex_timer.reset(evtimer_new(torrent->session->event_base, pexPulse, this));
            tr_timerAdd(*pex_timer, PexIntervalSecs, 0);
        }

        if (tr_peerIoSupportsUTP(io))
        {
            tr_address const* addr = tr_peerIoGetAddress(io, nullptr);
            tr_peerMgrSetUtpSupported(torrent, addr);
            tr_peerMgrSetUtpFailed(torrent, addr, false);
        }

        if (tr_peerIoSupportsLTEP(io))
        {
            sendLtepHandshake(this);
        }

        tellPeerWhatWeHave(this);

        if (tr_dhtEnabled(torrent->session) && tr_peerIoSupportsDHT(io))
        {
            /* Only send PORT over IPv6 when the IPv6 DHT is running (BEP-32). */
            struct tr_address const* addr = tr_peerIoGetAddress(io, nullptr);

            if (addr->type == TR_AF_INET || tr_globalIPv6(nullptr) != nullptr)
            {
                protocolSendPort(this, tr_dhtPort(torrent->session));
            }
        }

        tr_peerIoSetIOFuncs(io, canRead, didWrite, gotError, this);
        updateDesiredRequestCount(this);
    }

    ~tr_peerMsgsImpl() override
    {
        set_active(TR_UP, false);
        set_active(TR_DOWN, false);

        if (this->incoming.block != nullptr)
        {
            evbuffer_free(this->incoming.block);
        }

        if (this->io != nullptr)
        {
            tr_peerIoClear(this->io);
            tr_peerIoUnref(this->io); /* balanced by the ref in handshakeDoneCB() */
        }

        evbuffer_free(this->outMessages);
        tr_free(this->pex6);
        tr_free(this->pex);
    }

    bool is_transferring_pieces(uint64_t now, tr_direction direction, unsigned int* setme_Bps) const override
    {
        auto const Bps = tr_peerIoGetPieceSpeed_Bps(io, now, direction);

        if (setme_Bps != nullptr)
        {
            *setme_Bps = Bps;
        }

        return Bps > 0;
    }

    [[nodiscard]] size_t pendingReqsToClient() const noexcept override
    {
        return std::size(peer_requested_);
    }

    [[nodiscard]] bool is_peer_choked() const noexcept override
    {
        return peer_is_choked_;
    }

    [[nodiscard]] bool is_peer_interested() const noexcept override
    {
        return peer_is_interested_;
    }

    [[nodiscard]] bool is_client_choked() const noexcept override
    {
        return client_is_choked_;
    }

    [[nodiscard]] bool is_client_interested() const noexcept override
    {
        return client_is_interested_;
    }

    [[nodiscard]] bool is_utp_connection() const noexcept override
    {
        return io->socket.type == TR_PEER_SOCKET_TYPE_UTP;
    }

    [[nodiscard]] bool is_encrypted() const override
    {
        return tr_peerIoIsEncrypted(io);
    }

    [[nodiscard]] bool is_incoming_connection() const override
    {
        return tr_peerIoIsIncoming(io);
    }

    [[nodiscard]] bool is_active(tr_direction direction) const override
    {
        TR_ASSERT(tr_isDirection(direction));
        auto const active = is_active_[direction];
        TR_ASSERT(active == calculate_active(direction));
        return active;
    }

    void update_active(tr_direction direction) override
    {
        TR_ASSERT(tr_isDirection(direction));

        set_active(direction, calculate_active(direction));
    }

    [[nodiscard]] bool is_connection_older_than(time_t timestamp) const noexcept override
    {
        return io->time_created < timestamp;
    }

    void cancel_block_request(tr_block_index_t block) override
    {
        protocolSendCancel(this, blockToReq(torrent, block));
    }

    void set_choke(bool peer_is_choked) override
    {
        time_t const now = tr_time();
        time_t const fibrillationTime = now - MinChokePeriodSec;

        if (chokeChangedAt > fibrillationTime)
        {
            // TODO logtrace(msgs, "Not changing choke to %d to avoid fibrillation", peer_is_choked);
        }
        else if (peer_is_choked_ != peer_is_choked)
        {
            peer_is_choked_ = peer_is_choked;

            if (peer_is_choked_)
            {
                cancelAllRequestsToClient(this);
            }

            protocolSendChoke(this, peer_is_choked_);
            chokeChangedAt = now;
            update_active(TR_CLIENT_TO_PEER);
        }
    }

    void pulse() override
    {
        peerPulse(this);
    }

    void on_piece_completed(tr_piece_index_t piece) override
    {
        protocolSendHave(this, piece);

        // since we have more pieces now, we might not be interested in this peer
        update_interest();
    }

    void set_interested(bool interested) override
    {
        if (client_is_interested_ != interested)
        {
            client_is_interested_ = interested;
            sendInterest(this, interested);
            update_active(TR_PEER_TO_CLIENT);
        }
    }

    void update_interest()
    {
        // TODO -- might need to poke the mgr on startup
    }

    // publishing events

    void publishError(int err)
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_ERROR;
        e.err = err;
        publish(e);
    }

    void publishGotBlock(struct peer_request const* req)
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_BLOCK;
        e.pieceIndex = req->index;
        e.offset = req->offset;
        e.length = req->length;
        publish(e);
    }

    void publishGotRej(struct peer_request const* req)
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_REJ;
        e.pieceIndex = req->index;
        e.offset = req->offset;
        e.length = req->length;
        publish(e);
    }

    void publishGotChoke()
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_CHOKE;
        publish(e);
    }

    void publishClientGotHaveAll()
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_HAVE_ALL;
        publish(e);
    }

    void publishClientGotHaveNone()
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_HAVE_NONE;
        publish(e);
    }

    void publishClientGotPieceData(uint32_t length)
    {
        auto e = tr_peer_event{};
        e.length = length;
        e.eventType = TR_PEER_CLIENT_GOT_PIECE_DATA;
        publish(e);
    }

    void publishPeerGotPieceData(uint32_t length)
    {
        auto e = tr_peer_event{};
        e.length = length;
        e.eventType = TR_PEER_PEER_GOT_PIECE_DATA;
        publish(e);
    }

    void publishClientGotSuggest(tr_piece_index_t pieceIndex)
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_SUGGEST;
        e.pieceIndex = pieceIndex;
        publish(e);
    }

    void publishClientGotPort(tr_port port)
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_PORT;
        e.port = port;
        publish(e);
    }

    void publishClientGotAllowedFast(tr_piece_index_t pieceIndex)
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_ALLOWED_FAST;
        e.pieceIndex = pieceIndex;
        publish(e);
    }

    void publishClientGotBitfield(tr_bitfield* bitfield)
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_BITFIELD;
        e.bitfield = bitfield;
        publish(e);
    }

    void publishClientGotHave(tr_piece_index_t index)
    {
        auto e = tr_peer_event{};
        e.eventType = TR_PEER_CLIENT_GOT_HAVE;
        e.pieceIndex = index;
        publish(e);
    }

private:
    [[nodiscard]] bool calculate_active(tr_direction direction) const
    {
        if (direction == TR_CLIENT_TO_PEER)
        {
            return is_peer_interested() && !is_peer_choked();
        }

        // TR_PEER_TO_CLIENT

        if (!torrent->hasMetainfo())
        {
            return true;
        }

        auto const active = is_client_interested() && !is_client_choked();
        TR_ASSERT(!active || !torrent->isDone());
        return active;
    }

    void set_active(tr_direction direction, bool active)
    {
        // TODO logtrace(msgs, "direction [%d] is_active [%d]", int(direction), int(is_active));
        auto& val = is_active_[direction];
        if (val != active)
        {
            val = active;

            tr_swarmIncrementActivePeers(torrent->swarm, direction, active);
        }
    }

    void publish(tr_peer_event const& e)
    {
        if (callback_ != nullptr)
        {
            (*callback_)(this, &e, callbackData_);
        }
    }

public:
    /* Whether or not we've choked this peer. */
    bool peer_is_choked_ = true;

    /* whether or not the peer has indicated it will download from us. */
    bool peer_is_interested_ = false;

    /* whether or not the peer is choking us. */
    bool client_is_choked_ = true;

    /* whether or not we've indicated to the peer that we would download from them if unchoked. */
    bool client_is_interested_ = false;

    bool peerSupportsPex = false;
    bool peerSupportsMetadataXfer = false;
    bool clientSentLtepHandshake = false;
    bool peerSentLtepHandshake = false;

    size_t desired_request_count = 0;

    /* how long the outMessages batch should be allowed to grow before
     * it's flushed -- some messages (like requests >:) should be sent
     * very quickly; others aren't as urgent. */
    int8_t outMessagesBatchPeriod;

    AwaitingBt state = AwaitingBt::Length;
    uint8_t ut_pex_id = 0;
    uint8_t ut_metadata_id = 0;
    uint16_t pexCount = 0;
    uint16_t pexCount6 = 0;

    tr_port dht_port;

    EncryptionPreference encryption_preference = EncryptionPreference::Unknown;

    size_t metadata_size_hint = 0;
#if 0
    /* number of pieces we'll allow in our fast set */
    static auto constexpr MAX_FAST_SET_SIZE = int{ 3 };
    size_t fastsetSize;
    tr_piece_index_t fastset[MAX_FAST_SET_SIZE];
#endif

    tr_torrent* const torrent;

    evbuffer* const outMessages; /* all the non-piece messages */

    struct QueuedPeerRequest : public peer_request
    {
        explicit QueuedPeerRequest(peer_request in) noexcept
            : peer_request{ in }
        {
        }

        bool prefetched = false;
    };

    std::vector<QueuedPeerRequest> peer_requested_;

    int peerAskedForMetadata[MetadataReqQ] = {};
    int peerAskedForMetadataCount = 0;

    tr_pex* pex = nullptr;
    tr_pex* pex6 = nullptr;

    time_t clientSentAnythingAt = 0;

    time_t chokeChangedAt = 0;

    /* when we started batching the outMessages */
    time_t outMessagesBatchedAt = 0;

    struct tr_incoming incoming = {};

    /* if the peer supports the Extension Protocol in BEP 10 and
       supplied a reqq argument, it's stored here. */
    std::optional<size_t> reqq;

    UniqueTimer pex_timer;

    tr_peerIo* io = nullptr;

private:
    bool is_active_[2] = { false, false };

    tr_peer_callback const callback_;
    void* const callbackData_;
};

tr_peerMsgs* tr_peerMsgsNew(tr_torrent* torrent, peer_atom* atom, tr_peerIo* io, tr_peer_callback callback, void* callbackData)
{
    return new tr_peerMsgsImpl(torrent, atom, io, callback, callbackData);
}

/**
***
**/

#define myLogMacro(msgs, level, text) \
    do \
    { \
        if (tr_logGetLevel() >= (level)) \
        { \
            tr_logAddMessage( \
                __FILE__, \
                __LINE__, \
                (level), \
                fmt::format(FMT_STRING("{:s} [{:s}]: {:s}"), (msgs)->io->addrStr(), (msgs)->client, text), \
                (msgs)->torrent->name()); \
        } \
    } while (0)

#define logdbg(msgs, text) myLogMacro(msgs, TR_LOG_DEBUG, text)
#define logtrace(msgs, text) myLogMacro(msgs, TR_LOG_TRACE, text)

/**
***j
**/

static void pokeBatchPeriod(tr_peerMsgsImpl* msgs, int interval)
{
    if (msgs->outMessagesBatchPeriod > interval)
    {
        msgs->outMessagesBatchPeriod = interval;
        logtrace(msgs, fmt::format(FMT_STRING("lowering batch interval to {:d} seconds"), interval));
    }
}

static void dbgOutMessageLen(tr_peerMsgsImpl* msgs)
{
    logtrace(msgs, fmt::format(FMT_STRING("outMessage size is now {:d}"), evbuffer_get_length(msgs->outMessages)));
}

static void protocolSendReject(tr_peerMsgsImpl* msgs, struct peer_request const* req)
{
    TR_ASSERT(tr_peerIoSupportsFEXT(msgs->io));

    struct evbuffer* out = msgs->outMessages;

    evbuffer_add_uint32(out, sizeof(uint8_t) + 3 * sizeof(uint32_t));
    evbuffer_add_uint8(out, BtPeerMsgs::FextReject);
    evbuffer_add_uint32(out, req->index);
    evbuffer_add_uint32(out, req->offset);
    evbuffer_add_uint32(out, req->length);

    logtrace(msgs, fmt::format(FMT_STRING("rejecting {:d}:{:d}->{:d}..."), req->index, req->offset, req->length));
    dbgOutMessageLen(msgs);
}

static void protocolSendRequest(tr_peerMsgsImpl* msgs, struct peer_request const& req)
{
    struct evbuffer* out = msgs->outMessages;

    evbuffer_add_uint32(out, sizeof(uint8_t) + 3 * sizeof(uint32_t));
    evbuffer_add_uint8(out, BtPeerMsgs::Request);
    evbuffer_add_uint32(out, req.index);
    evbuffer_add_uint32(out, req.offset);
    evbuffer_add_uint32(out, req.length);

    logtrace(msgs, fmt::format(FMT_STRING("requesting {:d}:{:d}->{:d}..."), req.index, req.offset, req.length));
    dbgOutMessageLen(msgs);
    pokeBatchPeriod(msgs, ImmediatePriorityIntervalSecs);
}

static void protocolSendCancel(tr_peerMsgsImpl* msgs, peer_request const& req)
{
    struct evbuffer* out = msgs->outMessages;

    evbuffer_add_uint32(out, sizeof(uint8_t) + 3 * sizeof(uint32_t));
    evbuffer_add_uint8(out, BtPeerMsgs::Cancel);
    evbuffer_add_uint32(out, req.index);
    evbuffer_add_uint32(out, req.offset);
    evbuffer_add_uint32(out, req.length);

    logtrace(msgs, fmt::format(FMT_STRING("cancelling {:d}:{:d}->{:d}..."), req.index, req.offset, req.length));
    dbgOutMessageLen(msgs);
    pokeBatchPeriod(msgs, ImmediatePriorityIntervalSecs);
}

static void protocolSendPort(tr_peerMsgsImpl* msgs, tr_port port)
{
    struct evbuffer* out = msgs->outMessages;

    logtrace(msgs, fmt::format(FMT_STRING("sending Port {:d}"), port.host()));
    evbuffer_add_uint32(out, 3);
    evbuffer_add_uint8(out, BtPeerMsgs::Port);
    evbuffer_add_uint16(out, port.network());
}

static void protocolSendHave(tr_peerMsgsImpl* msgs, tr_piece_index_t index)
{
    struct evbuffer* out = msgs->outMessages;

    evbuffer_add_uint32(out, sizeof(uint8_t) + sizeof(uint32_t));
    evbuffer_add_uint8(out, BtPeerMsgs::Have);
    evbuffer_add_uint32(out, index);

    logtrace(msgs, fmt::format(FMT_STRING("sending Have {:d}"), index));
    dbgOutMessageLen(msgs);
    pokeBatchPeriod(msgs, LowPriorityIntervalSecs);
}

#if 0

static void protocolSendAllowedFast(tr_peerMsgs* msgs, uint32_t pieceIndex)
{
    TR_ASSERT(tr_peerIoSupportsFEXT(msgs->io));

    tr_peerIo* io = msgs->io;
    struct evbuffer* out = msgs->outMessages;

    evbuffer_add_uint32(io, out, sizeof(uint8_t) + sizeof(uint32_t));
    evbuffer_add_uint8(io, out, BtPeerMsgs::FextAllowedFast);
    evbuffer_add_uint32(io, out, pieceIndex);

    logtrace(msgs, "sending Allowed Fast %u...", pieceIndex);
    dbgOutMessageLen(msgs);
}

#endif

static void protocolSendChoke(tr_peerMsgsImpl* msgs, bool choke)
{
    struct evbuffer* out = msgs->outMessages;

    evbuffer_add_uint32(out, sizeof(uint8_t));
    evbuffer_add_uint8(out, choke ? BtPeerMsgs::Choke : BtPeerMsgs::Unchoke);

    logtrace(msgs, choke ? "sending choke" : "sending unchoked");
    dbgOutMessageLen(msgs);
    pokeBatchPeriod(msgs, ImmediatePriorityIntervalSecs);
}

static void protocolSendHaveAll(tr_peerMsgsImpl* msgs)
{
    TR_ASSERT(tr_peerIoSupportsFEXT(msgs->io));

    struct evbuffer* out = msgs->outMessages;

    evbuffer_add_uint32(out, sizeof(uint8_t));
    evbuffer_add_uint8(out, BtPeerMsgs::FextHaveAll);

    logtrace(msgs, "sending HAVE_ALL...");
    dbgOutMessageLen(msgs);
    pokeBatchPeriod(msgs, ImmediatePriorityIntervalSecs);
}

static void protocolSendHaveNone(tr_peerMsgsImpl* msgs)
{
    TR_ASSERT(tr_peerIoSupportsFEXT(msgs->io));

    struct evbuffer* out = msgs->outMessages;

    evbuffer_add_uint32(out, sizeof(uint8_t));
    evbuffer_add_uint8(out, BtPeerMsgs::FextHaveNone);

    logtrace(msgs, "sending HAVE_NONE...");
    dbgOutMessageLen(msgs);
    pokeBatchPeriod(msgs, ImmediatePriorityIntervalSecs);
}

/**
***  ALLOWED FAST SET
***  For explanation, see http://www.bittorrent.org/beps/bep_0006.html
**/

#if 0

size_t tr_generateAllowedSet(tr_piece_index_t* setmePieces, size_t desiredSetSize, size_t pieceCount, uint8_t const* infohash,
    tr_address const* addr)
{
    TR_ASSERT(setmePieces != nullptr);
    TR_ASSERT(desiredSetSize <= pieceCount);
    TR_ASSERT(desiredSetSize != 0);
    TR_ASSERT(pieceCount != 0);
    TR_ASSERT(infohash != nullptr);
    TR_ASSERT(addr != nullptr);

    size_t setSize = 0;

    if (addr->type == TR_AF_INET)
    {
        uint8_t w[SHA_DIGEST_LENGTH + 4];
        uint8_t* walk = w;
        uint8_t x[SHA_DIGEST_LENGTH];

        uint32_t ui32 = ntohl(htonl(addr->addr.addr4.s_addr) & 0xffffff00); /* (1) */
        memcpy(w, &ui32, sizeof(uint32_t));
        walk += sizeof(uint32_t);
        memcpy(walk, infohash, SHA_DIGEST_LENGTH); /* (2) */
        walk += SHA_DIGEST_LENGTH;
        tr_sha1(x, w, walk - w, nullptr); /* (3) */
        TR_ASSERT(sizeof(w) == walk - w);

        while (setSize < desiredSetSize)
        {
            for (int i = 0; i < 5 && setSize < desiredSetSize; ++i) /* (4) */
            {
                uint32_t j = i * 4; /* (5) */
                uint32_t y = ntohl(*(uint32_t*)(x + j)); /* (6) */
                uint32_t index = y % pieceCount; /* (7) */
                bool found = false;

                for (size_t k = 0; !found && k < setSize; ++k) /* (8) */
                {
                    found = setmePieces[k] == index;
                }

                if (!found)
                {
                    setmePieces[setSize++] = index; /* (9) */
                }
            }

            tr_sha1(x, x, sizeof(x), nullptr); /* (3) */
        }
    }

    return setSize;
}

static void updateFastSet(tr_peerMsgs*)
{
    bool const fext = tr_peerIoSupportsFEXT(msgs->io);
    bool const peerIsNeedy = msgs->peer->progress < 0.10;

    if (fext && peerIsNeedy && !msgs->haveFastSet)
    {
        struct tr_address const* addr = tr_peerIoGetAddress(msgs->io, nullptr);
        tr_info const* inf = &msgs->torrent->info;
        size_t const numwant = std::min(MAX_FAST_SET_SIZE, inf->pieceCount);

        /* build the fast set */
        msgs->fastsetSize = tr_generateAllowedSet(msgs->fastset, numwant, inf->pieceCount, inf->hash, addr);
        msgs->haveFastSet = true;

        /* send it to the peer */
        for (size_t i = 0; i < msgs->fastsetSize; ++i)
        {
            protocolSendAllowedFast(msgs, msgs->fastset[i]);
        }
    }
}

#endif
/**
***  INTEREST
**/

static void sendInterest(tr_peerMsgsImpl* msgs, bool b)
{
    TR_ASSERT(msgs != nullptr);

    struct evbuffer* out = msgs->outMessages;

    logtrace(msgs, b ? "Sending Interested" : "Sending Not Interested");
    evbuffer_add_uint32(out, sizeof(uint8_t));
    evbuffer_add_uint8(out, b ? BtPeerMsgs::Interested : BtPeerMsgs::NotInterested);

    pokeBatchPeriod(msgs, HighPriorityIntervalSecs);
    dbgOutMessageLen(msgs);
}

static bool popNextMetadataRequest(tr_peerMsgsImpl* msgs, int* piece)
{
    if (msgs->peerAskedForMetadataCount == 0)
    {
        return false;
    }

    *piece = msgs->peerAskedForMetadata[0];

    tr_removeElementFromArray(msgs->peerAskedForMetadata, 0, sizeof(int), msgs->peerAskedForMetadataCount);
    --msgs->peerAskedForMetadataCount;

    return true;
}

static void cancelAllRequestsToClient(tr_peerMsgsImpl* msgs)
{
    if (auto const must_send_rej = tr_peerIoSupportsFEXT(msgs->io); must_send_rej)
    {
        for (auto& req : msgs->peer_requested_)
        {
            protocolSendReject(msgs, &req);
        }
    }

    msgs->peer_requested_.clear();
}

/**
***
**/

static bool reqIsValid(tr_peerMsgsImpl const* peer, uint32_t index, uint32_t offset, uint32_t length)
{
    return tr_torrentReqIsValid(peer->torrent, index, offset, length);
}

static bool requestIsValid(tr_peerMsgsImpl const* msgs, struct peer_request const* req)
{
    return reqIsValid(msgs, req->index, req->offset, req->length);
}

/**
***
**/

static void sendLtepHandshake(tr_peerMsgsImpl* msgs)
{
    evbuffer* const out = msgs->outMessages;
    unsigned char const* ipv6 = tr_globalIPv6(msgs->io->session);
    static tr_quark version_quark = 0;

    if (msgs->clientSentLtepHandshake)
    {
        return;
    }

    if (version_quark == 0)
    {
        version_quark = tr_quark_new(TR_NAME " " USERAGENT_PREFIX);
    }

    logtrace(msgs, "sending an ltep handshake");
    msgs->clientSentLtepHandshake = true;

    /* decide if we want to advertise metadata xfer support (BEP 9) */
    bool const allow_metadata_xfer = msgs->torrent->isPublic();

    /* decide if we want to advertise pex support */
    auto allow_pex = bool{};
    if (!msgs->torrent->allowsPex())
    {
        allow_pex = false;
    }
    else if (msgs->peerSentLtepHandshake)
    {
        allow_pex = msgs->peerSupportsPex;
    }
    else
    {
        allow_pex = true;
    }

    auto val = tr_variant{};
    tr_variantInitDict(&val, 8);
    tr_variantDictAddBool(&val, TR_KEY_e, msgs->session->encryptionMode != TR_CLEAR_PREFERRED);

    if (ipv6 != nullptr)
    {
        tr_variantDictAddRaw(&val, TR_KEY_ipv6, ipv6, 16);
    }

    // http://bittorrent.org/beps/bep_0009.html
    // It also adds "metadata_size" to the handshake message (not the
    // "m" dictionary) specifying an integer value of the number of
    // bytes of the metadata.
    auto const info_dict_size = msgs->torrent->infoDictSize();
    if (allow_metadata_xfer && msgs->torrent->hasMetainfo() && info_dict_size > 0)
    {
        tr_variantDictAddInt(&val, TR_KEY_metadata_size, info_dict_size);
    }

    // http://bittorrent.org/beps/bep_0010.html
    // Local TCP listen port. Allows each side to learn about the TCP
    // port number of the other side. Note that there is no need for the
    // receiving side of the connection to send this extension message,
    // since its port number is already known.
    tr_variantDictAddInt(&val, TR_KEY_p, msgs->session->peerPort().host());

    // http://bittorrent.org/beps/bep_0010.html
    // An integer, the number of outstanding request messages this
    // client supports without dropping any. The default in in
    // libtorrent is 250.
    tr_variantDictAddInt(&val, TR_KEY_reqq, ReqQ);

    // http://bittorrent.org/beps/bep_0010.html
    // Client name and version (as a utf-8 string). This is a much more
    // reliable way of identifying the client than relying on the
    // peer id encoding.
    tr_variantDictAddQuark(&val, TR_KEY_v, version_quark);

    // http://bittorrent.org/beps/bep_0021.html
    // A peer that is a partial seed SHOULD include an extra header in
    // the extension handshake 'upload_only'. Setting the value of this
    // key to 1 indicates that this peer is not interested in downloading
    // anything.
    tr_variantDictAddBool(&val, TR_KEY_upload_only, msgs->torrent->isDone());

    if (allow_metadata_xfer || allow_pex)
    {
        tr_variant* m = tr_variantDictAddDict(&val, TR_KEY_m, 2);

        if (allow_metadata_xfer)
        {
            tr_variantDictAddInt(m, TR_KEY_ut_metadata, UT_METADATA_ID);
        }

        if (allow_pex)
        {
            tr_variantDictAddInt(m, TR_KEY_ut_pex, UT_PEX_ID);
        }
    }

    auto* const payload = tr_variantToBuf(&val, TR_VARIANT_FMT_BENC);

    evbuffer_add_uint32(out, 2 * sizeof(uint8_t) + evbuffer_get_length(payload));
    evbuffer_add_uint8(out, BtPeerMsgs::Ltep);
    evbuffer_add_uint8(out, LtepMessages::Handshake);
    evbuffer_add_buffer(out, payload);
    pokeBatchPeriod(msgs, ImmediatePriorityIntervalSecs);
    dbgOutMessageLen(msgs);

    /* cleanup */
    evbuffer_free(payload);
    tr_variantFree(&val);
}

static void parseLtepHandshake(tr_peerMsgsImpl* msgs, uint32_t len, struct evbuffer* inbuf)
{
    msgs->peerSentLtepHandshake = true;

    // LTEP messages are usually just a couple hundred bytes,
    // so try using a strbuf to handle it on the stack
    auto tmp = tr_strbuf<char, 512>{};
    tmp.resize(len);
    tr_peerIoReadBytes(msgs->io, inbuf, std::data(tmp), std::size(tmp));
    auto const handshake_sv = tmp.sv();

    auto val = tr_variant{};
    if (!tr_variantFromBuf(&val, TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE, handshake_sv) || !tr_variantIsDict(&val))
    {
        logtrace(msgs, "GET  extended-handshake, couldn't get dictionary");
        return;
    }

    logtrace(msgs, fmt::format(FMT_STRING("here is the base64-encoded handshake: [{:s}]"), tr_base64_encode(handshake_sv)));

    /* does the peer prefer encrypted connections? */
    auto i = int64_t{};
    auto pex = tr_pex{};
    if (tr_variantDictFindInt(&val, TR_KEY_e, &i))
    {
        msgs->encryption_preference = i != 0 ? EncryptionPreference::Yes : EncryptionPreference::No;

        if (msgs->encryption_preference == EncryptionPreference::Yes)
        {
            pex.flags |= ADDED_F_ENCRYPTION_FLAG;
        }
    }

    /* check supported messages for utorrent pex */
    msgs->peerSupportsPex = false;
    msgs->peerSupportsMetadataXfer = false;

    if (tr_variant* sub = nullptr; tr_variantDictFindDict(&val, TR_KEY_m, &sub))
    {
        if (tr_variantDictFindInt(sub, TR_KEY_ut_pex, &i))
        {
            msgs->peerSupportsPex = i != 0;
            msgs->ut_pex_id = (uint8_t)i;
            logtrace(msgs, fmt::format(FMT_STRING("msgs->ut_pex is {:d}"), static_cast<int>(msgs->ut_pex_id)));
        }

        if (tr_variantDictFindInt(sub, TR_KEY_ut_metadata, &i))
        {
            msgs->peerSupportsMetadataXfer = i != 0;
            msgs->ut_metadata_id = (uint8_t)i;
            logtrace(msgs, fmt::format(FMT_STRING("msgs->ut_metadata_id is {:d}"), static_cast<int>(msgs->ut_metadata_id)));
        }

        if (tr_variantDictFindInt(sub, TR_KEY_ut_holepunch, &i))
        {
            /* Mysterious µTorrent extension that we don't grok.  However,
               it implies support for µTP, so use it to indicate that. */
            tr_peerMgrSetUtpFailed(msgs->torrent, tr_peerIoGetAddress(msgs->io, nullptr), false);
        }
    }

    /* look for metainfo size (BEP 9) */
    if (tr_variantDictFindInt(&val, TR_KEY_metadata_size, &i) && tr_torrentSetMetadataSizeHint(msgs->torrent, i))
    {
        msgs->metadata_size_hint = (size_t)i;
    }

    /* look for upload_only (BEP 21) */
    if (tr_variantDictFindInt(&val, TR_KEY_upload_only, &i))
    {
        pex.flags |= ADDED_F_SEED_FLAG;
    }

    /* get peer's listening port */
    if (tr_variantDictFindInt(&val, TR_KEY_p, &i))
    {
        pex.port.setHost(i);
        msgs->publishClientGotPort(pex.port);
        logtrace(msgs, fmt::format(FMT_STRING("peer's port is now {:d}"), i));
    }

    uint8_t const* addr = nullptr;
    auto addr_len = size_t{};
    if (tr_peerIoIsIncoming(msgs->io) && tr_variantDictFindRaw(&val, TR_KEY_ipv4, &addr, &addr_len) && addr_len == 4)
    {
        pex.addr.type = TR_AF_INET;
        memcpy(&pex.addr.addr.addr4, addr, 4);
        tr_peerMgrAddPex(msgs->torrent, TR_PEER_FROM_LTEP, &pex, 1);
    }

    if (tr_peerIoIsIncoming(msgs->io) && tr_variantDictFindRaw(&val, TR_KEY_ipv6, &addr, &addr_len) && addr_len == 16)
    {
        pex.addr.type = TR_AF_INET6;
        memcpy(&pex.addr.addr.addr6, addr, 16);
        tr_peerMgrAddPex(msgs->torrent, TR_PEER_FROM_LTEP, &pex, 1);
    }

    /* get peer's maximum request queue size */
    if (tr_variantDictFindInt(&val, TR_KEY_reqq, &i))
    {
        msgs->reqq = i;
    }

    tr_variantFree(&val);
}

static void parseUtMetadata(tr_peerMsgsImpl* msgs, uint32_t msglen, struct evbuffer* inbuf)
{
    int64_t msg_type = -1;
    int64_t piece = -1;
    int64_t total_size = 0;
    auto* const tmp = tr_new(char, msglen);

    tr_peerIoReadBytes(msgs->io, inbuf, tmp, msglen);
    char const* const msg_end = (char const*)tmp + msglen;

    auto dict = tr_variant{};
    char const* benc_end = nullptr;
    if (tr_variantFromBuf(&dict, TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE, { tmp, msglen }, &benc_end))
    {
        (void)tr_variantDictFindInt(&dict, TR_KEY_msg_type, &msg_type);
        (void)tr_variantDictFindInt(&dict, TR_KEY_piece, &piece);
        (void)tr_variantDictFindInt(&dict, TR_KEY_total_size, &total_size);
        tr_variantFree(&dict);
    }

    logtrace(
        msgs,
        fmt::format(FMT_STRING("got ut_metadata msg: type {:d}, piece {:d}, total_size {:d}"), msg_type, piece, total_size));

    if (msg_type == MetadataMsgType::Reject)
    {
        /* NOOP */
    }

    if (msg_type == MetadataMsgType::Data && !msgs->torrent->hasMetainfo() && msg_end - benc_end <= METADATA_PIECE_SIZE &&
        piece * METADATA_PIECE_SIZE + (msg_end - benc_end) <= total_size)
    {
        int const pieceLen = msg_end - benc_end;
        tr_torrentSetMetadataPiece(msgs->torrent, piece, benc_end, pieceLen);
    }

    if (msg_type == MetadataMsgType::Request)
    {
        if (piece >= 0 && msgs->torrent->hasMetainfo() && msgs->torrent->isPublic() &&
            msgs->peerAskedForMetadataCount < MetadataReqQ)
        {
            msgs->peerAskedForMetadata[msgs->peerAskedForMetadataCount++] = piece;
        }
        else
        {
            evbuffer* const out = msgs->outMessages;

            /* build the rejection message */
            auto v = tr_variant{};
            tr_variantInitDict(&v, 2);
            tr_variantDictAddInt(&v, TR_KEY_msg_type, MetadataMsgType::Reject);
            tr_variantDictAddInt(&v, TR_KEY_piece, piece);
            evbuffer* const payload = tr_variantToBuf(&v, TR_VARIANT_FMT_BENC);

            /* write it out as a LTEP message to our outMessages buffer */
            evbuffer_add_uint32(out, 2 * sizeof(uint8_t) + evbuffer_get_length(payload));
            evbuffer_add_uint8(out, BtPeerMsgs::Ltep);
            evbuffer_add_uint8(out, msgs->ut_metadata_id);
            evbuffer_add_buffer(out, payload);
            pokeBatchPeriod(msgs, HighPriorityIntervalSecs);
            dbgOutMessageLen(msgs);

            /* cleanup */
            evbuffer_free(payload);
            tr_variantFree(&v);
        }
    }

    tr_free(tmp);
}

static void parseUtPex(tr_peerMsgsImpl* msgs, uint32_t msglen, struct evbuffer* inbuf)
{
    tr_torrent* tor = msgs->torrent;
    if (!tor->allowsPex())
    {
        return;
    }

    auto* tmp = tr_new(char, msglen);
    tr_peerIoReadBytes(msgs->io, inbuf, tmp, msglen);

    if (tr_variant val; tr_variantFromBuf(&val, TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE, { tmp, msglen }))
    {
        uint8_t const* added = nullptr;
        auto added_len = size_t{};
        if (tr_variantDictFindRaw(&val, TR_KEY_added, &added, &added_len))
        {
            uint8_t const* added_f = nullptr;
            auto added_f_len = size_t{};
            if (!tr_variantDictFindRaw(&val, TR_KEY_added_f, &added_f, &added_f_len))
            {
                added_f_len = 0;
                added_f = nullptr;
            }

            auto pex = tr_peerMgrCompactToPex(added, added_len, added_f, added_f_len);
            pex.resize(std::min(MaxPexPeerCount, std::size(pex)));
            tr_peerMgrAddPex(tor, TR_PEER_FROM_PEX, std::data(pex), std::size(pex));
        }

        if (tr_variantDictFindRaw(&val, TR_KEY_added6, &added, &added_len))
        {
            uint8_t const* added_f = nullptr;
            auto added_f_len = size_t{};
            if (!tr_variantDictFindRaw(&val, TR_KEY_added6_f, &added_f, &added_f_len))
            {
                added_f_len = 0;
                added_f = nullptr;
            }

            auto pex = tr_peerMgrCompact6ToPex(added, added_len, added_f, added_f_len);
            pex.resize(std::min(MaxPexPeerCount, std::size(pex)));
            tr_peerMgrAddPex(tor, TR_PEER_FROM_PEX, std::data(pex), std::size(pex));
        }

        tr_variantFree(&val);
    }

    tr_free(tmp);
}

static void sendPex(tr_peerMsgsImpl* msgs);

static void parseLtep(tr_peerMsgsImpl* msgs, uint32_t msglen, struct evbuffer* inbuf)
{
    TR_ASSERT(msglen > 0);

    auto ltep_msgid = uint8_t{};
    tr_peerIoReadUint8(msgs->io, inbuf, &ltep_msgid);
    msglen--;

    if (ltep_msgid == LtepMessages::Handshake)
    {
        logtrace(msgs, "got ltep handshake");
        parseLtepHandshake(msgs, msglen, inbuf);

        if (tr_peerIoSupportsLTEP(msgs->io))
        {
            sendLtepHandshake(msgs);
            sendPex(msgs);
        }
    }
    else if (ltep_msgid == UT_PEX_ID)
    {
        logtrace(msgs, "got ut pex");
        msgs->peerSupportsPex = true;
        parseUtPex(msgs, msglen, inbuf);
    }
    else if (ltep_msgid == UT_METADATA_ID)
    {
        logtrace(msgs, "got ut metadata");
        msgs->peerSupportsMetadataXfer = true;
        parseUtMetadata(msgs, msglen, inbuf);
    }
    else
    {
        logtrace(msgs, fmt::format(FMT_STRING("skipping unknown ltep message ({:d})"), static_cast<int>(ltep_msgid)));
        evbuffer_drain(inbuf, msglen);
    }
}

static ReadState readBtLength(tr_peerMsgsImpl* msgs, struct evbuffer* inbuf, size_t inlen)
{
    auto len = uint32_t{};
    if (inlen < sizeof(len))
    {
        return READ_LATER;
    }

    tr_peerIoReadUint32(msgs->io, inbuf, &len);
    if (len == 0) /* peer sent us a keepalive message */
    {
        logtrace(msgs, "got KeepAlive");
    }
    else
    {
        msgs->incoming.length = len;
        msgs->state = AwaitingBt::Id;
    }

    return READ_NOW;
}

static ReadState readBtMessage(tr_peerMsgsImpl* /*msgs*/, struct evbuffer* /*inbuf*/, size_t /*inlen*/);

static ReadState readBtId(tr_peerMsgsImpl* msgs, struct evbuffer* inbuf, size_t inlen)
{
    if (inlen < sizeof(uint8_t))
    {
        return READ_LATER;
    }

    auto id = uint8_t{};
    tr_peerIoReadUint8(msgs->io, inbuf, &id);
    msgs->incoming.id = id;
    logtrace(
        msgs,
        fmt::format(FMT_STRING("msgs->incoming.id is now {:d}: msgs->incoming.length is {:d}"), id, msgs->incoming.length));

    if (id == BtPeerMsgs::Piece)
    {
        msgs->state = AwaitingBt::Piece;
        return READ_NOW;
    }

    if (msgs->incoming.length != 1)
    {
        msgs->state = AwaitingBt::Message;
        return READ_NOW;
    }

    return readBtMessage(msgs, inbuf, inlen - 1);
}

static void updatePeerProgress(tr_peerMsgsImpl* msgs)
{
    tr_peerUpdateProgress(msgs->torrent, msgs);

    msgs->update_interest();
}

static void prefetchPieces(tr_peerMsgsImpl* msgs)
{
    if (!msgs->session->isPrefetchEnabled)
    {
        return;
    }

    // ensure that the first `PrefetchMax` items in `msgs->peer_requested_` are prefetched.
    auto& requests = msgs->peer_requested_;
    for (size_t i = 0, n = std::min(PrefetchMax, std::size(requests)); i < n; ++i)
    {
        if (auto& req = requests[i]; !req.prefetched)
        {
            tr_cachePrefetchBlock(
                msgs->session->cache,
                msgs->torrent,
                msgs->torrent->pieceLoc(req.index, req.offset),
                req.length);
            req.prefetched = true;
        }
    }
}

[[nodiscard]] static bool canAddRequestFromPeer(tr_peerMsgsImpl const* const msgs, struct peer_request const& req)
{
    if (msgs->peer_is_choked_)
    {
        logtrace(msgs, "rejecting request from choked peer");
        return false;
    }

    if (std::size(msgs->peer_requested_) >= ReqQ)
    {
        logtrace(msgs, "rejecting request ... reqq is full");
        return false;
    }

    if (!tr_torrentReqIsValid(msgs->torrent, req.index, req.offset, req.length))
    {
        logtrace(msgs, "rejecting an invalid request.");
        return false;
    }

    if (!msgs->torrent->hasPiece(req.index))
    {
        logtrace(msgs, "rejecting request for a piece we don't have.");
        return false;
    }

    return true;
}

static void peerMadeRequest(tr_peerMsgsImpl* msgs, struct peer_request const* req)
{
    if (canAddRequestFromPeer(msgs, *req))
    {
        msgs->peer_requested_.emplace_back(*req);
        prefetchPieces(msgs);
    }
    else if (tr_peerIoSupportsFEXT(msgs->io))
    {
        protocolSendReject(msgs, req);
    }
}

static bool messageLengthIsCorrect(tr_peerMsgsImpl const* msg, uint8_t id, uint32_t len)
{
    switch (id)
    {
    case BtPeerMsgs::Choke:
    case BtPeerMsgs::Unchoke:
    case BtPeerMsgs::Interested:
    case BtPeerMsgs::NotInterested:
    case BtPeerMsgs::FextHaveAll:
    case BtPeerMsgs::FextHaveNone:
        return len == 1;

    case BtPeerMsgs::Have:
    case BtPeerMsgs::FextSuggest:
    case BtPeerMsgs::FextAllowedFast:
        return len == 5;

    case BtPeerMsgs::Bitfield:
        if (msg->torrent->hasMetainfo())
        {
            return len == (msg->torrent->pieceCount() >> 3) + ((msg->torrent->pieceCount() & 7) != 0 ? 1 : 0) + 1U;
        }

        /* we don't know the piece count yet,
           so we can only guess whether to send true or false */
        if (msg->metadata_size_hint > 0)
        {
            return len <= msg->metadata_size_hint;
        }

        return true;

    case BtPeerMsgs::Request:
    case BtPeerMsgs::Cancel:
    case BtPeerMsgs::FextReject:
        return len == 13;

    case BtPeerMsgs::Piece:
        return len > 9 && len <= 16393;

    case BtPeerMsgs::Port:
        return len == 3;

    case BtPeerMsgs::Ltep:
        return len >= 2;

    default:
        return false;
    }
}

static int clientGotBlock(tr_peerMsgsImpl* msgs, struct evbuffer* block, struct peer_request const* req);

static ReadState readBtPiece(tr_peerMsgsImpl* msgs, struct evbuffer* inbuf, size_t inlen, size_t* setme_piece_bytes_read)
{
    TR_ASSERT(evbuffer_get_length(inbuf) >= inlen);

    logtrace(msgs, "In readBtPiece");

    struct peer_request* req = &msgs->incoming.blockReq;

    if (req->length == 0)
    {
        if (inlen < 8)
        {
            return READ_LATER;
        }

        tr_peerIoReadUint32(msgs->io, inbuf, &req->index);
        tr_peerIoReadUint32(msgs->io, inbuf, &req->offset);
        req->length = msgs->incoming.length - 9;
        logtrace(
            msgs,
            fmt::format(FMT_STRING("got incoming block header {:d}:{:d}->{:d}"), req->index, req->offset, req->length));
        return READ_NOW;
    }

    if (msgs->incoming.block == nullptr)
    {
        msgs->incoming.block = evbuffer_new();
    }

    struct evbuffer* const block_buffer = msgs->incoming.block;

    /* read in another chunk of data */
    size_t const nLeft = req->length - evbuffer_get_length(block_buffer);
    size_t const n = std::min(nLeft, inlen);

    tr_peerIoReadBytesToBuf(msgs->io, inbuf, block_buffer, n);

    msgs->publishClientGotPieceData(n);
    *setme_piece_bytes_read += n;
    logtrace(
        msgs,
        fmt::format(
            FMT_STRING("got {:d} bytes for block {:d}:{:d}->{:d} ... {:d} remain"),
            n,
            req->index,
            req->offset,
            req->length,
            req->length - evbuffer_get_length(block_buffer)));

    if (evbuffer_get_length(block_buffer) < req->length)
    {
        return READ_LATER;
    }

    /* pass the block along... */
    int const err = clientGotBlock(msgs, block_buffer, req);
    evbuffer_drain(block_buffer, evbuffer_get_length(block_buffer));

    /* cleanup */
    req->length = 0;
    msgs->state = AwaitingBt::Length;
    return err != 0 ? READ_ERR : READ_NOW;
}

static ReadState readBtMessage(tr_peerMsgsImpl* msgs, struct evbuffer* inbuf, size_t inlen)
{
    uint8_t const id = msgs->incoming.id;
#ifdef TR_ENABLE_ASSERTS
    size_t const startBufLen = evbuffer_get_length(inbuf);
#endif
    bool const fext = tr_peerIoSupportsFEXT(msgs->io);

    auto ui32 = uint32_t{};
    auto msglen = uint32_t{ msgs->incoming.length };

    TR_ASSERT(msglen > 0);

    --msglen; /* id length */

    logtrace(
        msgs,
        fmt::format(FMT_STRING("got BT id {:d}, len {:d}, buffer size is {:d}"), static_cast<int>(id), msglen, inlen));

    if (inlen < msglen)
    {
        return READ_LATER;
    }

    if (!messageLengthIsCorrect(msgs, id, msglen + 1))
    {
        logdbg(
            msgs,
            fmt::format(FMT_STRING("bad packet - BT message #{:d} with a length of {:d}"), static_cast<int>(id), msglen));
        msgs->publishError(EMSGSIZE);
        return READ_ERR;
    }

    switch (id)
    {
    case BtPeerMsgs::Choke:
        logtrace(msgs, "got Choke");
        msgs->client_is_choked_ = true;

        if (!fext)
        {
            msgs->publishGotChoke();
        }

        msgs->update_active(TR_PEER_TO_CLIENT);
        break;

    case BtPeerMsgs::Unchoke:
        logtrace(msgs, "got Unchoke");
        msgs->client_is_choked_ = false;
        msgs->update_active(TR_PEER_TO_CLIENT);
        updateDesiredRequestCount(msgs);
        break;

    case BtPeerMsgs::Interested:
        logtrace(msgs, "got Interested");
        msgs->peer_is_interested_ = true;
        msgs->update_active(TR_CLIENT_TO_PEER);
        break;

    case BtPeerMsgs::NotInterested:
        logtrace(msgs, "got Not Interested");
        msgs->peer_is_interested_ = false;
        msgs->update_active(TR_CLIENT_TO_PEER);
        break;

    case BtPeerMsgs::Have:
        tr_peerIoReadUint32(msgs->io, inbuf, &ui32);
        logtrace(msgs, fmt::format(FMT_STRING("got Have: {:d}"), ui32));

        if (msgs->torrent->hasMetainfo() && ui32 >= msgs->torrent->pieceCount())
        {
            msgs->publishError(ERANGE);
            return READ_ERR;
        }

        /* a peer can send the same HAVE message twice... */
        if (!msgs->have.test(ui32))
        {
            msgs->have.set(ui32);
            msgs->publishClientGotHave(ui32);
        }

        updatePeerProgress(msgs);
        break;

    case BtPeerMsgs::Bitfield:
        {
            auto* const tmp = tr_new(uint8_t, msglen);
            logtrace(msgs, "got a bitfield");
            tr_peerIoReadBytes(msgs->io, inbuf, tmp, msglen);
            msgs->have.setRaw(tmp, msglen);
            msgs->publishClientGotBitfield(&msgs->have);
            updatePeerProgress(msgs);
            tr_free(tmp);
            break;
        }

    case BtPeerMsgs::Request:
        {
            struct peer_request r;
            tr_peerIoReadUint32(msgs->io, inbuf, &r.index);
            tr_peerIoReadUint32(msgs->io, inbuf, &r.offset);
            tr_peerIoReadUint32(msgs->io, inbuf, &r.length);
            logtrace(msgs, fmt::format(FMT_STRING("got Request: {:d}:{:d}->{:d}"), r.index, r.offset, r.length));
            peerMadeRequest(msgs, &r);
            break;
        }

    case BtPeerMsgs::Cancel:
        {
            struct peer_request r;
            tr_peerIoReadUint32(msgs->io, inbuf, &r.index);
            tr_peerIoReadUint32(msgs->io, inbuf, &r.offset);
            tr_peerIoReadUint32(msgs->io, inbuf, &r.length);
            msgs->cancelsSentToClient.add(tr_time(), 1);
            logtrace(msgs, fmt::format(FMT_STRING("got a Cancel {:d}:{:d}->{:d}"), r.index, r.offset, r.length));

            auto& requests = msgs->peer_requested_;
            if (auto iter = std::find(std::begin(requests), std::end(requests), r); iter != std::end(requests))
            {
                requests.erase(iter);

                // bep6: "Even when a request is cancelled, the peer
                // receiving the cancel should respond with either the
                // corresponding reject or the corresponding piece"
                if (fext)
                {
                    protocolSendReject(msgs, &r);
                }
            }
            break;
        }

    case BtPeerMsgs::Piece:
        TR_ASSERT(false); /* handled elsewhere! */
        break;

    case BtPeerMsgs::Port:
        // http://bittorrent.org/beps/bep_0005.html
        // Peers supporting the DHT set the last bit of the 8-byte reserved flags
        // exchanged in the BitTorrent protocol handshake. Peer receiving a handshake
        // indicating the remote peer supports the DHT should send a PORT message.
        // It begins with byte 0x09 and has a two byte payload containing the UDP
        // port of the DHT node in network byte order.
        {
            logtrace(msgs, "Got a BtPeerMsgs::Port");

            auto nport = uint16_t{};
            tr_peerIoReadUint16(msgs->io, inbuf, &nport);
            if (auto const dht_port = tr_port::fromNetwork(nport); !std::empty(dht_port))
            {
                msgs->dht_port = dht_port;
                tr_dhtAddNode(msgs->session, tr_peerAddress(msgs), msgs->dht_port, false);
            }
        }
        break;

    case BtPeerMsgs::FextSuggest:
        logtrace(msgs, "Got a BtPeerMsgs::FextSuggest");
        tr_peerIoReadUint32(msgs->io, inbuf, &ui32);

        if (fext)
        {
            msgs->publishClientGotSuggest(ui32);
        }
        else
        {
            msgs->publishError(EMSGSIZE);
            return READ_ERR;
        }

        break;

    case BtPeerMsgs::FextAllowedFast:
        logtrace(msgs, "Got a BtPeerMsgs::FextAllowedFast");
        tr_peerIoReadUint32(msgs->io, inbuf, &ui32);

        if (fext)
        {
            msgs->publishClientGotAllowedFast(ui32);
        }
        else
        {
            msgs->publishError(EMSGSIZE);
            return READ_ERR;
        }

        break;

    case BtPeerMsgs::FextHaveAll:
        logtrace(msgs, "Got a BtPeerMsgs::FextHaveAll");

        if (fext)
        {
            msgs->have.setHasAll();
            msgs->publishClientGotHaveAll();
            updatePeerProgress(msgs);
        }
        else
        {
            msgs->publishError(EMSGSIZE);
            return READ_ERR;
        }

        break;

    case BtPeerMsgs::FextHaveNone:
        logtrace(msgs, "Got a BtPeerMsgs::FextHaveNone");

        if (fext)
        {
            msgs->have.setHasNone();
            msgs->publishClientGotHaveNone();
            updatePeerProgress(msgs);
        }
        else
        {
            msgs->publishError(EMSGSIZE);
            return READ_ERR;
        }

        break;

    case BtPeerMsgs::FextReject:
        {
            struct peer_request r;
            logtrace(msgs, "Got a BtPeerMsgs::FextReject");
            tr_peerIoReadUint32(msgs->io, inbuf, &r.index);
            tr_peerIoReadUint32(msgs->io, inbuf, &r.offset);
            tr_peerIoReadUint32(msgs->io, inbuf, &r.length);

            if (fext)
            {
                msgs->publishGotRej(&r);
            }
            else
            {
                msgs->publishError(EMSGSIZE);
                return READ_ERR;
            }

            break;
        }

    case BtPeerMsgs::Ltep:
        logtrace(msgs, "Got a BtPeerMsgs::Ltep");
        parseLtep(msgs, msglen, inbuf);
        break;

    default:
        logtrace(msgs, fmt::format(FMT_STRING("peer sent us an UNKNOWN: {:d}"), static_cast<int>(id)));
        tr_peerIoDrain(msgs->io, inbuf, msglen);
        break;
    }

    TR_ASSERT(msglen + 1 == msgs->incoming.length);
    TR_ASSERT(evbuffer_get_length(inbuf) == startBufLen - msglen);

    msgs->state = AwaitingBt::Length;
    return READ_NOW;
}

/* returns 0 on success, or an errno on failure */
static int clientGotBlock(tr_peerMsgsImpl* msgs, struct evbuffer* data, struct peer_request const* req)
{
    TR_ASSERT(msgs != nullptr);
    TR_ASSERT(req != nullptr);

    tr_torrent* const tor = msgs->torrent;
    auto const block = tor->pieceLoc(req->index, req->offset).block;

    if (!requestIsValid(msgs, req))
    {
        logdbg(msgs, fmt::format(FMT_STRING("dropping invalid block {:d}:{:d}->{:d}"), req->index, req->offset, req->length));
        return EBADMSG;
    }

    if (req->length != msgs->torrent->blockSize(block))
    {
        logdbg(
            msgs,
            fmt::format(
                FMT_STRING("wrong block size -- expected {:d}, got {:d}"),
                msgs->torrent->blockSize(block),
                req->length));
        return EMSGSIZE;
    }

    logtrace(msgs, fmt::format(FMT_STRING("got block {:d}:{:d}->{:d}"), req->index, req->offset, req->length));

    if (!tr_peerMgrDidPeerRequest(msgs->torrent, msgs, block))
    {
        logdbg(msgs, "we didn't ask for this message...");
        return 0;
    }

    if (msgs->torrent->hasPiece(req->index))
    {
        logtrace(msgs, "we did ask for this message, but the piece is already complete...");
        return 0;
    }

    /**
    ***  Save the block
    **/

    if (int const
            err = tr_cacheWriteBlock(msgs->session->cache, tor, tor->pieceLoc(req->index, req->offset), req->length, data);
        err != 0)
    {
        return err;
    }

    msgs->blame.set(req->index);
    msgs->publishGotBlock(req);
    return 0;
}

static void didWrite(tr_peerIo* io, size_t bytesWritten, bool wasPieceData, void* vmsgs)
{
    auto* msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);

    if (wasPieceData)
    {
        msgs->publishPeerGotPieceData(bytesWritten);
    }

    if (tr_isPeerIo(io) && io->userData != nullptr)
    {
        peerPulse(msgs);
    }
}

static ReadState canRead(tr_peerIo* io, void* vmsgs, size_t* piece)
{
    auto* msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);
    evbuffer* const in = io->getReadBuffer();
    size_t const inlen = evbuffer_get_length(in);

    logtrace(
        msgs,
        fmt::format(FMT_STRING("canRead: inlen is {:d}, msgs->state is {:d}"), inlen, static_cast<int>(msgs->state)));

    auto ret = ReadState{};
    if (inlen == 0)
    {
        ret = READ_LATER;
    }
    else if (msgs->state == AwaitingBt::Piece)
    {
        ret = readBtPiece(msgs, in, inlen, piece);
    }
    else
    {
        switch (msgs->state)
        {
        case AwaitingBt::Length:
            ret = readBtLength(msgs, in, inlen);
            break;

        case AwaitingBt::Id:
            ret = readBtId(msgs, in, inlen);
            break;

        case AwaitingBt::Message:
            ret = readBtMessage(msgs, in, inlen);
            break;

        default:
#ifdef TR_ENABLE_ASSERTS
            TR_ASSERT_MSG(false, fmt::format(FMT_STRING("unhandled peer messages state {:d}"), static_cast<int>(msgs->state)));
#else
            ret = READ_ERR;
            break;
#endif
        }
    }

    logtrace(msgs, fmt::format(FMT_STRING("canRead: ret is {:d}"), static_cast<int>(ret)));

    return ret;
}

/**
***
**/

static void updateDesiredRequestCount(tr_peerMsgsImpl* msgs)
{
    tr_torrent const* const torrent = msgs->torrent;

    /* there are lots of reasons we might not want to request any blocks... */
    if (torrent->isDone() || !torrent->hasMetainfo() || msgs->client_is_choked_ || !msgs->client_is_interested_)
    {
        msgs->desired_request_count = 0;
    }
    else
    {
        /* Get the rate limit we should use.
         * TODO: this needs to consider all the other peers as well... */
        uint64_t const now = tr_time_msec();
        auto rate_Bps = tr_peerGetPieceSpeed_Bps(msgs, now, TR_PEER_TO_CLIENT);
        if (tr_torrentUsesSpeedLimit(torrent, TR_PEER_TO_CLIENT))
        {
            rate_Bps = std::min(rate_Bps, torrent->speedLimitBps(TR_PEER_TO_CLIENT));
        }

        /* honor the session limits, if enabled */
        auto irate_Bps = unsigned{};
        if (tr_torrentUsesSessionLimits(torrent) &&
            tr_sessionGetActiveSpeedLimit_Bps(torrent->session, TR_PEER_TO_CLIENT, &irate_Bps))
        {
            rate_Bps = std::min(rate_Bps, irate_Bps);
        }

        /* use this desired rate to figure out how
         * many requests we should send to this peer */
        size_t constexpr Floor = 32;
        size_t constexpr Seconds = RequestBufSecs;
        size_t const estimated_blocks_in_period = (rate_Bps * Seconds) / tr_block_info::BlockSize;
        size_t const ceil = msgs->reqq ? *msgs->reqq : 250;
        msgs->desired_request_count = std::clamp(estimated_blocks_in_period, Floor, ceil);
    }
}

static void updateMetadataRequests(tr_peerMsgsImpl* msgs, time_t now)
{
    auto piece = int{};
    if (msgs->peerSupportsMetadataXfer && tr_torrentGetNextMetadataRequest(msgs->torrent, now, &piece))
    {
        evbuffer* const out = msgs->outMessages;

        /* build the data message */
        auto tmp = tr_variant{};
        tr_variantInitDict(&tmp, 3);
        tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Request);
        tr_variantDictAddInt(&tmp, TR_KEY_piece, piece);
        auto* const payload = tr_variantToBuf(&tmp, TR_VARIANT_FMT_BENC);

        logtrace(msgs, fmt::format(FMT_STRING("requesting metadata piece #{:d}"), piece));

        /* write it out as a LTEP message to our outMessages buffer */
        evbuffer_add_uint32(out, 2 * sizeof(uint8_t) + evbuffer_get_length(payload));
        evbuffer_add_uint8(out, BtPeerMsgs::Ltep);
        evbuffer_add_uint8(out, msgs->ut_metadata_id);
        evbuffer_add_buffer(out, payload);
        pokeBatchPeriod(msgs, HighPriorityIntervalSecs);
        dbgOutMessageLen(msgs);

        /* cleanup */
        evbuffer_free(payload);
        tr_variantFree(&tmp);
    }
}

static void updateBlockRequests(tr_peerMsgsImpl* msgs)
{
    if (!msgs->torrent->clientCanDownload())
    {
        return;
    }

    auto const n_active = tr_peerMgrCountActiveRequestsToPeer(msgs->torrent, msgs);
    if (n_active >= msgs->desired_request_count)
    {
        return;
    }

    auto const n_wanted = msgs->desired_request_count - n_active;
    if (n_wanted == 0)
    {
        return;
    }

    TR_ASSERT(msgs->is_client_interested());
    TR_ASSERT(!msgs->is_client_choked());

    for (auto const span : tr_peerMgrGetNextRequests(msgs->torrent, msgs, n_wanted))
    {
        for (tr_block_index_t block = span.begin; block < span.end; ++block)
        {
            protocolSendRequest(msgs, blockToReq(msgs->torrent, block));
        }

        tr_peerMgrClientSentRequests(msgs->torrent, msgs, span);
    }
}

static size_t fillOutputBuffer(tr_peerMsgsImpl* msgs, time_t now)
{
    size_t bytesWritten = 0;
    struct peer_request req;
    bool const haveMessages = evbuffer_get_length(msgs->outMessages) != 0;
    bool const fext = tr_peerIoSupportsFEXT(msgs->io);

    /**
    ***  Protocol messages
    **/

    if (haveMessages && msgs->outMessagesBatchedAt == 0) /* fresh batch */
    {
        logtrace(
            msgs,
            fmt::format(FMT_STRING("started an outMessages batch (length is {:d})"), evbuffer_get_length(msgs->outMessages)));
        msgs->outMessagesBatchedAt = now;
    }
    else if (haveMessages && now - msgs->outMessagesBatchedAt >= msgs->outMessagesBatchPeriod)
    {
        size_t const len = evbuffer_get_length(msgs->outMessages);
        /* flush the protocol messages */
        logtrace(msgs, fmt::format(FMT_STRING("flushing outMessages... to {:p} (length is {:d})"), fmt::ptr(msgs->io), len));
        tr_peerIoWriteBuf(msgs->io, msgs->outMessages, false);
        msgs->clientSentAnythingAt = now;
        msgs->outMessagesBatchedAt = 0;
        msgs->outMessagesBatchPeriod = LowPriorityIntervalSecs;
        bytesWritten += len;
    }

    /**
    ***  Metadata Pieces
    **/

    auto piece = int{};
    if (tr_peerIoGetWriteBufferSpace(msgs->io, now) >= METADATA_PIECE_SIZE && popNextMetadataRequest(msgs, &piece))
    {
        auto ok = bool{ false };

        auto dataLen = size_t{};

        if (auto* data = static_cast<char*>(tr_torrentGetMetadataPiece(msgs->torrent, piece, &dataLen)); data != nullptr)
        {
            auto* const out = msgs->outMessages;

            /* build the data message */
            auto tmp = tr_variant{};
            tr_variantInitDict(&tmp, 3);
            tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Data);
            tr_variantDictAddInt(&tmp, TR_KEY_piece, piece);
            tr_variantDictAddInt(&tmp, TR_KEY_total_size, msgs->torrent->infoDictSize());
            evbuffer* const payload = tr_variantToBuf(&tmp, TR_VARIANT_FMT_BENC);

            /* write it out as a LTEP message to our outMessages buffer */
            evbuffer_add_uint32(out, 2 * sizeof(uint8_t) + evbuffer_get_length(payload) + dataLen);
            evbuffer_add_uint8(out, BtPeerMsgs::Ltep);
            evbuffer_add_uint8(out, msgs->ut_metadata_id);
            evbuffer_add_buffer(out, payload);
            evbuffer_add(out, data, dataLen);
            pokeBatchPeriod(msgs, HighPriorityIntervalSecs);
            dbgOutMessageLen(msgs);

            evbuffer_free(payload);
            tr_variantFree(&tmp);
            tr_free(data);

            ok = true;
        }

        if (!ok) /* send a rejection message */
        {
            evbuffer* const out = msgs->outMessages;

            /* build the rejection message */
            auto tmp = tr_variant{};
            tr_variantInitDict(&tmp, 2);
            tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Reject);
            tr_variantDictAddInt(&tmp, TR_KEY_piece, piece);
            evbuffer* const payload = tr_variantToBuf(&tmp, TR_VARIANT_FMT_BENC);

            /* write it out as a LTEP message to our outMessages buffer */
            evbuffer_add_uint32(out, 2 * sizeof(uint8_t) + evbuffer_get_length(payload));
            evbuffer_add_uint8(out, BtPeerMsgs::Ltep);
            evbuffer_add_uint8(out, msgs->ut_metadata_id);
            evbuffer_add_buffer(out, payload);
            pokeBatchPeriod(msgs, HighPriorityIntervalSecs);
            dbgOutMessageLen(msgs);

            evbuffer_free(payload);
            tr_variantFree(&tmp);
        }
    }

    /**
    ***  Data Blocks
    **/

    if (tr_peerIoGetWriteBufferSpace(msgs->io, now) >= tr_block_info::BlockSize && !std::empty(msgs->peer_requested_))
    {
        req = msgs->peer_requested_.front();
        msgs->peer_requested_.erase(std::begin(msgs->peer_requested_));

        if (requestIsValid(msgs, &req) && msgs->torrent->hasPiece(req.index))
        {
            uint32_t const msglen = 4 + 1 + 4 + 4 + req.length;
            struct evbuffer_iovec iovec[1];

            auto* const out = evbuffer_new();
            evbuffer_expand(out, msglen);

            evbuffer_add_uint32(out, sizeof(uint8_t) + 2 * sizeof(uint32_t) + req.length);
            evbuffer_add_uint8(out, BtPeerMsgs::Piece);
            evbuffer_add_uint32(out, req.index);
            evbuffer_add_uint32(out, req.offset);

            evbuffer_reserve_space(out, req.length, iovec, 1);
            bool err = tr_cacheReadBlock(
                           msgs->session->cache,
                           msgs->torrent,
                           msgs->torrent->pieceLoc(req.index, req.offset),
                           req.length,
                           static_cast<uint8_t*>(iovec[0].iov_base)) != 0;
            iovec[0].iov_len = req.length;
            evbuffer_commit_space(out, iovec, 1);

            /* check the piece if it needs checking... */
            if (!err)
            {
                err = !msgs->torrent->ensurePieceIsChecked(req.index);
                if (err)
                {
                    msgs->torrent->setLocalError(
                        fmt::format(FMT_STRING("Please Verify Local Data! Piece #{:d} is corrupt."), req.index));
                }
            }

            if (err)
            {
                if (fext)
                {
                    protocolSendReject(msgs, &req);
                }
            }
            else
            {
                size_t const n = evbuffer_get_length(out);
                logtrace(msgs, fmt::format(FMT_STRING("sending block {:d}:{:d}->{:d}"), req.index, req.offset, req.length));
                TR_ASSERT(n == msglen);
                tr_peerIoWriteBuf(msgs->io, out, true);
                bytesWritten += n;
                msgs->clientSentAnythingAt = now;
                msgs->blocksSentToPeer.add(tr_time(), 1);
            }

            evbuffer_free(out);

            if (err)
            {
                bytesWritten = 0;
                msgs = nullptr;
            }
        }
        else if (fext) /* peer needs a reject message */
        {
            protocolSendReject(msgs, &req);
        }

        if (msgs != nullptr)
        {
            prefetchPieces(msgs);
        }
    }

    /**
    ***  Keepalive
    **/

    if (msgs != nullptr && msgs->clientSentAnythingAt != 0 && now - msgs->clientSentAnythingAt > KeepaliveIntervalSecs)
    {
        logtrace(msgs, "sending a keepalive message");
        evbuffer_add_uint32(msgs->outMessages, 0);
        pokeBatchPeriod(msgs, ImmediatePriorityIntervalSecs);
    }

    return bytesWritten;
}

static void peerPulse(void* vmsgs)
{
    auto* msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);
    time_t const now = tr_time();

    if (tr_isPeerIo(msgs->io))
    {
        updateDesiredRequestCount(msgs);
        updateBlockRequests(msgs);
        updateMetadataRequests(msgs, now);
    }

    for (;;)
    {
        if (fillOutputBuffer(msgs, now) < 1)
        {
            break;
        }
    }
}

static void gotError(tr_peerIo* /*io*/, short what, void* vmsgs)
{
    auto* msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);

    if ((what & BEV_EVENT_TIMEOUT) != 0)
    {
        logdbg(msgs, fmt::format(FMT_STRING("libevent got a timeout, what={:d}"), what));
    }

    if ((what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) != 0)
    {
        logdbg(
            msgs,
            fmt::format(FMT_STRING("libevent got an error! what={:d}, errno={:d} ({:s})"), what, errno, tr_strerror(errno)));
    }

    msgs->publishError(ENOTCONN);
}

static void sendBitfield(tr_peerMsgsImpl* msgs)
{
    TR_ASSERT(msgs->torrent->hasMetainfo());

    struct evbuffer* out = msgs->outMessages;

    auto bytes = msgs->torrent->createPieceBitfield();
    evbuffer_add_uint32(out, sizeof(uint8_t) + bytes.size());
    evbuffer_add_uint8(out, BtPeerMsgs::Bitfield);
    evbuffer_add(out, bytes.data(), std::size(bytes));
    logtrace(msgs, fmt::format(FMT_STRING("sending bitfield... outMessage size is now {:d}"), evbuffer_get_length(out)));
    pokeBatchPeriod(msgs, ImmediatePriorityIntervalSecs);
}

static void tellPeerWhatWeHave(tr_peerMsgsImpl* msgs)
{
    bool const fext = tr_peerIoSupportsFEXT(msgs->io);

    if (fext && msgs->torrent->hasAll())
    {
        protocolSendHaveAll(msgs);
    }
    else if (fext && msgs->torrent->hasNone())
    {
        protocolSendHaveNone(msgs);
    }
    else if (!msgs->torrent->hasNone())
    {
        sendBitfield(msgs);
    }
}

/**
***
**/

/* some peers give us error messages if we send
   more than this many peers in a single pex message
   https://wiki.theory.org/BitTorrentPeerExchangeConventions */
static auto constexpr MaxPexAdded = int{ 50 };
static auto constexpr MaxPexDropped = int{ 50 };

struct PexDiffs
{
    tr_pex* added;
    tr_pex* dropped;
    tr_pex* elements;
    int addedCount;
    int droppedCount;
    int elementCount;
};

static void pexAddedCb(void const* vpex, void* userData)
{
    auto* diffs = static_cast<PexDiffs*>(userData);
    auto const* pex = static_cast<tr_pex const*>(vpex);

    if (diffs->addedCount < MaxPexAdded)
    {
        diffs->added[diffs->addedCount++] = *pex;
        diffs->elements[diffs->elementCount++] = *pex;
    }
}

static constexpr void pexDroppedCb(void const* vpex, void* userData)
{
    auto* diffs = static_cast<PexDiffs*>(userData);
    auto const* pex = static_cast<tr_pex const*>(vpex);

    if (diffs->droppedCount < MaxPexDropped)
    {
        diffs->dropped[diffs->droppedCount++] = *pex;
    }
}

static constexpr void pexElementCb(void const* vpex, void* userData)
{
    auto* diffs = static_cast<PexDiffs*>(userData);
    auto const* pex = static_cast<tr_pex const*>(vpex);

    diffs->elements[diffs->elementCount++] = *pex;
}

using tr_set_func = void (*)(void const* element, void* userData);

/**
 * @brief find the differences and commonalities in two sorted sets
 * @param a the first set
 * @param aCount the number of elements in the set 'a'
 * @param b the second set
 * @param bCount the number of elements in the set 'b'
 * @param compare the sorting method for both sets
 * @param elementSize the sizeof the element in the two sorted sets
 * @param in_a called for items in set 'a' but not set 'b'
 * @param in_b called for items in set 'b' but not set 'a'
 * @param in_both called for items that are in both sets
 * @param userData user data passed along to in_a, in_b, and in_both
 */
static void tr_set_compare(
    void const* va,
    size_t aCount,
    void const* vb,
    size_t bCount,
    tr_voidptr_compare_func compare,
    size_t elementSize,
    tr_set_func in_a_cb,
    tr_set_func in_b_cb,
    tr_set_func in_both_cb,
    void* userData)
{
    auto const* a = static_cast<uint8_t const*>(va);
    auto const* b = static_cast<uint8_t const*>(vb);
    uint8_t const* aend = a + elementSize * aCount;
    uint8_t const* bend = b + elementSize * bCount;

    while (a != aend || b != bend)
    {
        if (a == aend)
        {
            (*in_b_cb)(b, userData);
            b += elementSize;
        }
        else if (b == bend)
        {
            (*in_a_cb)(a, userData);
            a += elementSize;
        }
        else
        {
            int const val = (*compare)(a, b);

            if (val == 0)
            {
                (*in_both_cb)(a, userData);
                a += elementSize;
                b += elementSize;
            }
            else if (val < 0)
            {
                (*in_a_cb)(a, userData);
                a += elementSize;
            }
            else if (val > 0)
            {
                (*in_b_cb)(b, userData);
                b += elementSize;
            }
        }
    }
}

static void sendPex(tr_peerMsgsImpl* msgs)
{
    if (msgs->peerSupportsPex && msgs->torrent->allowsPex())
    {
        PexDiffs diffs;
        PexDiffs diffs6;
        tr_pex* newPex = nullptr;
        tr_pex* newPex6 = nullptr;
        int const newCount = tr_peerMgrGetPeers(msgs->torrent, &newPex, TR_AF_INET, TR_PEERS_CONNECTED, MaxPexPeerCount);
        int const newCount6 = tr_peerMgrGetPeers(msgs->torrent, &newPex6, TR_AF_INET6, TR_PEERS_CONNECTED, MaxPexPeerCount);

        /* build the diffs */
        diffs.added = tr_new(tr_pex, newCount);
        diffs.addedCount = 0;
        diffs.dropped = tr_new(tr_pex, msgs->pexCount);
        diffs.droppedCount = 0;
        diffs.elements = tr_new(tr_pex, newCount + msgs->pexCount);
        diffs.elementCount = 0;
        tr_set_compare(
            msgs->pex,
            msgs->pexCount,
            newPex,
            newCount,
            tr_pexCompare,
            sizeof(tr_pex),
            pexDroppedCb,
            pexAddedCb,
            pexElementCb,
            &diffs);
        diffs6.added = tr_new(tr_pex, newCount6);
        diffs6.addedCount = 0;
        diffs6.dropped = tr_new(tr_pex, msgs->pexCount6);
        diffs6.droppedCount = 0;
        diffs6.elements = tr_new(tr_pex, newCount6 + msgs->pexCount6);
        diffs6.elementCount = 0;
        tr_set_compare(
            msgs->pex6,
            msgs->pexCount6,
            newPex6,
            newCount6,
            tr_pexCompare,
            sizeof(tr_pex),
            pexDroppedCb,
            pexAddedCb,
            pexElementCb,
            &diffs6);
        logtrace(
            msgs,
            fmt::format(
                FMT_STRING("pex: old peer count {:d}+{:d}, new peer count {:d}+{:d}, added {:d}+{:d}, removed {:d}+{:d}"),
                msgs->pexCount,
                msgs->pexCount6,
                newCount,
                newCount6,
                diffs.addedCount,
                diffs6.addedCount,
                diffs.droppedCount,
                diffs6.droppedCount));

        if (diffs.addedCount == 0 && diffs.droppedCount == 0 && diffs6.addedCount == 0 && diffs6.droppedCount == 0)
        {
            tr_free(diffs.elements);
            tr_free(diffs6.elements);
        }
        else
        {
            uint8_t* tmp = nullptr;
            uint8_t* walk = nullptr;
            evbuffer* const out = msgs->outMessages;

            /* update peer */
            tr_free(msgs->pex);
            msgs->pex = diffs.elements;
            msgs->pexCount = diffs.elementCount;
            tr_free(msgs->pex6);
            msgs->pex6 = diffs6.elements;
            msgs->pexCount6 = diffs6.elementCount;

            /* build the pex payload */
            auto val = tr_variant{};
            tr_variantInitDict(&val, 3); /* ipv6 support: left as 3: speed vs. likelihood? */

            if (diffs.addedCount > 0)
            {
                /* "added" */
                tmp = walk = tr_new(uint8_t, diffs.addedCount * 6);

                for (int i = 0; i < diffs.addedCount; ++i)
                {
                    memcpy(walk, &diffs.added[i].addr.addr, 4);
                    walk += 4;
                    memcpy(walk, &diffs.added[i].port, 2);
                    walk += 2;
                }

                TR_ASSERT(walk - tmp == diffs.addedCount * 6);
                tr_variantDictAddRaw(&val, TR_KEY_added, tmp, walk - tmp);
                tr_free(tmp);

                /* "added.f"
                 * unset each holepunch flag because we don't support it. */
                tmp = walk = tr_new(uint8_t, diffs.addedCount);

                for (int i = 0; i < diffs.addedCount; ++i)
                {
                    *walk++ = diffs.added[i].flags & ~ADDED_F_HOLEPUNCH;
                }

                TR_ASSERT(walk - tmp == diffs.addedCount);
                tr_variantDictAddRaw(&val, TR_KEY_added_f, tmp, walk - tmp);
                tr_free(tmp);
            }

            if (diffs.droppedCount > 0)
            {
                /* "dropped" */
                tmp = walk = tr_new(uint8_t, diffs.droppedCount * 6);

                for (int i = 0; i < diffs.droppedCount; ++i)
                {
                    memcpy(walk, &diffs.dropped[i].addr.addr, 4);
                    walk += 4;
                    memcpy(walk, &diffs.dropped[i].port, 2);
                    walk += 2;
                }

                TR_ASSERT(walk - tmp == diffs.droppedCount * 6);
                tr_variantDictAddRaw(&val, TR_KEY_dropped, tmp, walk - tmp);
                tr_free(tmp);
            }

            if (diffs6.addedCount > 0)
            {
                /* "added6" */
                tmp = walk = tr_new(uint8_t, diffs6.addedCount * 18);

                for (int i = 0; i < diffs6.addedCount; ++i)
                {
                    memcpy(walk, &diffs6.added[i].addr.addr.addr6.s6_addr, 16);
                    walk += 16;
                    memcpy(walk, &diffs6.added[i].port, 2);
                    walk += 2;
                }

                TR_ASSERT(walk - tmp == diffs6.addedCount * 18);
                tr_variantDictAddRaw(&val, TR_KEY_added6, tmp, walk - tmp);
                tr_free(tmp);

                /* "added6.f"
                 * unset each holepunch flag because we don't support it. */
                tmp = walk = tr_new(uint8_t, diffs6.addedCount);

                for (int i = 0; i < diffs6.addedCount; ++i)
                {
                    *walk++ = diffs6.added[i].flags & ~ADDED_F_HOLEPUNCH;
                }

                TR_ASSERT(walk - tmp == diffs6.addedCount);
                tr_variantDictAddRaw(&val, TR_KEY_added6_f, tmp, walk - tmp);
                tr_free(tmp);
            }

            if (diffs6.droppedCount > 0)
            {
                /* "dropped6" */
                tmp = walk = tr_new(uint8_t, diffs6.droppedCount * 18);

                for (int i = 0; i < diffs6.droppedCount; ++i)
                {
                    memcpy(walk, &diffs6.dropped[i].addr.addr.addr6.s6_addr, 16);
                    walk += 16;
                    memcpy(walk, &diffs6.dropped[i].port, 2);
                    walk += 2;
                }

                TR_ASSERT(walk - tmp == diffs6.droppedCount * 18);
                tr_variantDictAddRaw(&val, TR_KEY_dropped6, tmp, walk - tmp);
                tr_free(tmp);
            }

            /* write the pex message */
            auto* const payload = tr_variantToBuf(&val, TR_VARIANT_FMT_BENC);
            evbuffer_add_uint32(out, 2 * sizeof(uint8_t) + evbuffer_get_length(payload));
            evbuffer_add_uint8(out, BtPeerMsgs::Ltep);
            evbuffer_add_uint8(out, msgs->ut_pex_id);
            evbuffer_add_buffer(out, payload);
            pokeBatchPeriod(msgs, HighPriorityIntervalSecs);
            logtrace(
                msgs,
                fmt::format(FMT_STRING("sending a pex message; outMessage size is now {:d}"), evbuffer_get_length(out)));
            dbgOutMessageLen(msgs);

            evbuffer_free(payload);
            tr_variantFree(&val);
        }

        /* cleanup */
        tr_free(diffs.added);
        tr_free(diffs.dropped);
        tr_free(newPex);
        tr_free(diffs6.added);
        tr_free(diffs6.dropped);
        tr_free(newPex6);
    }
}

static void pexPulse(evutil_socket_t /*fd*/, short /*what*/, void* vmsgs)
{
    auto* msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);

    sendPex(msgs);

    TR_ASSERT(msgs->pex_timer);
    tr_timerAdd(*msgs->pex_timer, PexIntervalSecs, 0);
}
