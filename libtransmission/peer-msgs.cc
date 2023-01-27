// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <memory> // std::unique_ptr
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include <event2/bufferevent.h>

#include <fmt/format.h>

#include "transmission.h"

#include "cache.h"
#include "completion.h"
#include "crypto-utils.h"
#include "file.h"
#include "log.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-msgs.h"
#include "quark.h"
#include "session.h"
#include "timer.h"
#include "torrent-magnet.h"
#include "torrent.h"
#include "tr-assert.h"
#include "tr-buffer.h"
#include "tr-dht.h"
#include "utils.h"
#include "variant.h"
#include "version.h"

#ifndef EBADMSG
#define EBADMSG EINVAL
#endif

using namespace std::literals;

namespace
{

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

auto constexpr MinChokePeriodSec = int{ 10 };

// idle seconds before we send a keepalive
auto constexpr KeepaliveIntervalSecs = int{ 100 };

auto constexpr MetadataReqQ = int{ 64 };

auto constexpr ReqQ = int{ 512 };

// used in lowering the outMessages queue period
auto constexpr ImmediatePriorityIntervalSecs = int{ 0 };
auto constexpr HighPriorityIntervalSecs = int{ 2 };
auto constexpr LowPriorityIntervalSecs = int{ 10 };

// how many blocks to keep prefetched per peer
auto constexpr PrefetchMax = size_t{ 18 };

// when we're making requests from another peer,
// batch them together to send enough requests to
// meet our bandwidth goals for the next N seconds
auto constexpr RequestBufSecs = int{ 10 };

// ---

auto constexpr MaxPexPeerCount = size_t{ 50 };

// ---

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

// ---

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

peer_request blockToReq(tr_torrent const* tor, tr_block_index_t block)
{
    auto const loc = tor->blockLoc(block);
    return peer_request{ loc.piece, loc.piece_offset, tor->blockSize(block) };
}

// ---

/* this is raw, unchanged data from the peer regarding
 * the current message that it's sending us. */
struct tr_incoming
{
    uint8_t id = 0; // the protocol message, e.g. BtPeerMsgs::Piece
    uint32_t length = 0; // the full message payload length. Includes the +1 for id length
    std::optional<peer_request> block_req; // metadata for incoming blocks
    std::map<tr_block_index_t, std::unique_ptr<std::vector<uint8_t>>> block_buf; // piece data for incoming blocks
};

class tr_peerMsgsImpl;
// TODO: make these to be member functions
ReadState canRead(tr_peerIo* io, void* vmsgs, size_t* piece);
void cancelAllRequestsToClient(tr_peerMsgsImpl* msgs);
void didWrite(tr_peerIo* io, size_t bytes_written, bool was_piece_data, void* vmsgs);
void gotError(tr_peerIo* io, tr_error const& err, void* vmsgs);
void peerPulse(void* vmsgs);
void protocolSendCancel(tr_peerMsgsImpl* msgs, struct peer_request const& req);
void protocolSendChoke(tr_peerMsgsImpl* msgs, bool choke);
void protocolSendHave(tr_peerMsgsImpl* msgs, tr_piece_index_t index);
void protocolSendPort(tr_peerMsgsImpl* msgs, tr_port port);
void sendInterest(tr_peerMsgsImpl* msgs, bool b);
void sendLtepHandshake(tr_peerMsgsImpl* msgs);
void tellPeerWhatWeHave(tr_peerMsgsImpl* msgs);
void updateDesiredRequestCount(tr_peerMsgsImpl* msgs);

#define myLogMacro(msgs, level, text) \
    do \
    { \
        if (tr_logLevelIsActive(level)) \
        { \
            tr_logAddMessage( \
                __FILE__, \
                __LINE__, \
                (level), \
                fmt::format(FMT_STRING("{:s} [{:s}]: {:s}"), (msgs)->io->display_name(), (msgs)->client, text), \
                (msgs)->torrent->name()); \
        } \
    } while (0)

#define logdbg(msgs, text) myLogMacro(msgs, TR_LOG_DEBUG, text)
#define logtrace(msgs, text) myLogMacro(msgs, TR_LOG_TRACE, text)

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
    tr_peerMsgsImpl(tr_torrent* torrent_in, peer_atom* atom_in, std::shared_ptr<tr_peerIo> io_in, tr_peer_callback callback)
        : tr_peerMsgs{ torrent_in, atom_in }
        , outMessagesBatchPeriod{ LowPriorityIntervalSecs }
        , torrent{ torrent_in }
        , io{ std::move(io_in) }
        , have_{ torrent_in->pieceCount() }
        , callback_{ callback }
    {
        if (torrent->allowsPex())
        {
            pex_timer_ = getSession()->timerMaker().create([this]() { sendPex(); });
            pex_timer_->startRepeating(SendPexInterval);
        }

        if (io->supports_utp())
        {
            tr_peerMgrSetUtpSupported(torrent, io->address());
            tr_peerMgrSetUtpFailed(torrent, io->address(), false);
        }

        if (io->supports_ltep())
        {
            sendLtepHandshake(this);
        }

        tellPeerWhatWeHave(this);

        if (getSession()->allowsDHT() && io->supports_dht())
        {
            // only send PORT over IPv6 iff IPv6 DHT is running (BEP-32).
            if (auto const [addr, is_any] = getSession()->publicAddress(TR_AF_INET6); !is_any)
            {
                protocolSendPort(this, getSession()->udpPort());
            }
        }

        io->set_callbacks(canRead, didWrite, gotError, this);
        updateDesiredRequestCount(this);
    }

    tr_peerMsgsImpl(tr_peerMsgsImpl&&) = delete;
    tr_peerMsgsImpl(tr_peerMsgsImpl const&) = delete;
    tr_peerMsgsImpl& operator=(tr_peerMsgsImpl&&) = delete;
    tr_peerMsgsImpl& operator=(tr_peerMsgsImpl const&) = delete;

    ~tr_peerMsgsImpl() override
    {
        set_active(TR_UP, false);
        set_active(TR_DOWN, false);

        if (this->io)
        {
            this->io->clear();
        }
    }

    void dbgOutMessageLen() const
    {
        logtrace(this, fmt::format(FMT_STRING("outMessage size is now {:d}"), std::size(outMessages)));
    }

    void pokeBatchPeriod(int interval)
    {
        if (outMessagesBatchPeriod > interval)
        {
            outMessagesBatchPeriod = interval;
            logtrace(this, fmt::format(FMT_STRING("lowering batch interval to {:d} seconds"), interval));
        }
    }

    bool isTransferringPieces(uint64_t now, tr_direction dir, tr_bytes_per_second_t* setme_bytes_per_second) const override
    {
        auto const bytes_per_second = io->get_piece_speed_bytes_per_second(now, dir);

        if (setme_bytes_per_second != nullptr)
        {
            *setme_bytes_per_second = bytes_per_second;
        }

        return bytes_per_second > 0;
    }

    [[nodiscard]] size_t activeReqCount(tr_direction dir) const noexcept override
    {
        switch (dir)
        {
        case TR_CLIENT_TO_PEER: // requests we sent
            return tr_peerMgrCountActiveRequestsToPeer(torrent, this);

        case TR_PEER_TO_CLIENT: // requests they sent
            return std::size(peer_requested_);

        default:
            TR_ASSERT(0);
            return {};
        }
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
        return io->is_utp();
    }

    [[nodiscard]] bool is_encrypted() const override
    {
        return io->is_encrypted();
    }

    [[nodiscard]] bool is_incoming_connection() const override
    {
        return io->is_incoming();
    }

    [[nodiscard]] tr_bandwidth& bandwidth() noexcept override
    {
        return io->bandwidth();
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

    [[nodiscard]] std::pair<tr_address, tr_port> socketAddress() const override
    {
        return io->socket_address();
    }

    [[nodiscard]] std::string display_name() const override
    {
        auto const [addr, port] = socketAddress();
        return addr.display_name(port);
    }

    [[nodiscard]] tr_bitfield const& has() const noexcept override
    {
        return have_;
    }

    void onTorrentGotMetainfo() noexcept override
    {
        invalidatePercentDone();
    }

    void invalidatePercentDone()
    {
        updateInterest();
    }

    void cancel_block_request(tr_block_index_t block) override
    {
        protocolSendCancel(this, blockToReq(torrent, block));
    }

    void set_choke(bool peer_is_choked) override
    {
        time_t const now = tr_time();
        time_t const fibrillation_time = now - MinChokePeriodSec;

        if (chokeChangedAt > fibrillation_time)
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
        updateInterest();
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

    void updateInterest()
    {
        // TODO -- might need to poke the mgr on startup
    }

    //

    [[nodiscard]] bool isValidRequest(peer_request const& req) const
    {
        return tr_torrentReqIsValid(torrent, req.index, req.offset, req.length);
    }

    void requestBlocks(tr_block_span_t const* block_spans, size_t n_spans) override
    {
        TR_ASSERT(torrent->clientCanDownload());
        TR_ASSERT(is_client_interested());
        TR_ASSERT(!is_client_choked());

        for (auto const *span = block_spans, *span_end = span + n_spans; span != span_end; ++span)
        {
            for (auto [block, block_end] = *span; block < block_end; ++block)
            {
                // Note that requests can't cross over a piece boundary.
                // So if a piece isn't evenly divisible by the block size,
                // we need to split our block request info per-piece chunks.
                auto const byte_begin = torrent->blockLoc(block).byte;
                auto const block_size = torrent->blockSize(block);
                auto const byte_end = byte_begin + block_size;
                for (auto offset = byte_begin; offset < byte_end;)
                {
                    auto const loc = torrent->byteLoc(offset);
                    auto const left_in_block = block_size - loc.block_offset;
                    auto const left_in_piece = torrent->pieceSize(loc.piece) - loc.piece_offset;
                    auto const req_len = std::min(left_in_block, left_in_piece);
                    protocolSendRequest({ loc.piece, loc.piece_offset, req_len });
                    offset += req_len;
                }
            }

            tr_peerMgrClientSentRequests(torrent, this, *span);
        }
    }

    // how many blocks could we request from this peer right now?
    [[nodiscard]] RequestLimit canRequest() const noexcept override
    {
        auto const max_blocks = maxAvailableReqs();
        return RequestLimit{ max_blocks, max_blocks };
    }

    void sendPex();

    void publish(tr_peer_event const& peer_event)
    {
        if (callback_ != nullptr)
        {
            callback_(*this, peer_event);
        }
    }

private:
    [[nodiscard]] size_t maxAvailableReqs() const
    {
        if (torrent->isDone() || !torrent->hasMetainfo() || client_is_choked_ || !client_is_interested_)
        {
            return 0;
        }

        // Get the rate limit we should use.
        // TODO: this needs to consider all the other peers as well...
        uint64_t const now = tr_time_msec();
        auto rate_bytes_per_second = get_piece_speed_bytes_per_second(now, TR_PEER_TO_CLIENT);
        if (torrent->usesSpeedLimit(TR_PEER_TO_CLIENT))
        {
            rate_bytes_per_second = std::min(rate_bytes_per_second, torrent->speedLimitBps(TR_PEER_TO_CLIENT));
        }

        // honor the session limits, if enabled
        if (torrent->usesSessionLimits())
        {
            if (auto const irate_bytes_per_second = torrent->session->activeSpeedLimitBps(TR_PEER_TO_CLIENT);
                irate_bytes_per_second)
            {
                rate_bytes_per_second = std::min(rate_bytes_per_second, *irate_bytes_per_second);
            }
        }

        // use this desired rate to figure out how
        // many requests we should send to this peer
        size_t constexpr Floor = 32;
        size_t constexpr Seconds = RequestBufSecs;
        size_t const estimated_blocks_in_period = (rate_bytes_per_second * Seconds) / tr_block_info::BlockSize;
        size_t const ceil = reqq ? *reqq : 250;
        return std::clamp(estimated_blocks_in_period, Floor, ceil);
    }

    void protocolSendRequest(struct peer_request const& req)
    {
        TR_ASSERT(isValidRequest(req));

        auto& out = outMessages;
        out.addUint32(sizeof(uint8_t) + 3 * sizeof(uint32_t));
        out.addUint8(BtPeerMsgs::Request);
        out.addUint32(req.index);
        out.addUint32(req.offset);
        out.addUint32(req.length);

        logtrace(this, fmt::format(FMT_STRING("requesting {:d}:{:d}->{:d}..."), req.index, req.offset, req.length));
        dbgOutMessageLen();
        pokeBatchPeriod(ImmediatePriorityIntervalSecs);
    }

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

    tr_port dht_port;

    EncryptionPreference encryption_preference = EncryptionPreference::Unknown;

    size_t metadata_size_hint = 0;

    tr_torrent* const torrent;

    libtransmission::Buffer outMessages; /* all the non-piece messages */

    std::shared_ptr<tr_peerIo> const io;

    struct QueuedPeerRequest : public peer_request
    {
        explicit QueuedPeerRequest(peer_request in) noexcept
            : peer_request{ in }
        {
        }

        bool prefetched = false;
    };

    std::vector<QueuedPeerRequest> peer_requested_;

    std::vector<tr_pex> pex;
    std::vector<tr_pex> pex6;

    std::queue<int> peerAskedForMetadata;

    time_t clientSentAnythingAt = 0;

    time_t chokeChangedAt = 0;

    /* when we started batching the outMessages */
    time_t outMessagesBatchedAt = 0;

    struct tr_incoming incoming = {};

    /* if the peer supports the Extension Protocol in BEP 10 and
       supplied a reqq argument, it's stored here. */
    std::optional<size_t> reqq;

    std::unique_ptr<libtransmission::Timer> pex_timer_;

    tr_bitfield have_;

private:
    std::array<bool, 2> is_active_ = { false, false };

    tr_peer_callback const callback_;

    // seconds between periodic sendPex() calls
    static auto constexpr SendPexInterval = 90s;
};

// ---

void protocolSendReject(tr_peerMsgsImpl* msgs, struct peer_request const* req)
{
    TR_ASSERT(msgs->io->supports_fext());

    auto& out = msgs->outMessages;

    out.addUint32(sizeof(uint8_t) + 3 * sizeof(uint32_t));
    out.addUint8(BtPeerMsgs::FextReject);
    out.addUint32(req->index);
    out.addUint32(req->offset);
    out.addUint32(req->length);

    logtrace(msgs, fmt::format(FMT_STRING("rejecting {:d}:{:d}->{:d}..."), req->index, req->offset, req->length));
    msgs->dbgOutMessageLen();
}

void protocolSendCancel(tr_peerMsgsImpl* msgs, peer_request const& req)
{
    auto& out = msgs->outMessages;

    out.addUint32(sizeof(uint8_t) + 3 * sizeof(uint32_t));
    out.addUint8(BtPeerMsgs::Cancel);
    out.addUint32(req.index);
    out.addUint32(req.offset);
    out.addUint32(req.length);

    logtrace(msgs, fmt::format(FMT_STRING("cancelling {:d}:{:d}->{:d}..."), req.index, req.offset, req.length));
    msgs->dbgOutMessageLen();
    msgs->pokeBatchPeriod(ImmediatePriorityIntervalSecs);
}

void protocolSendPort(tr_peerMsgsImpl* msgs, tr_port port)
{
    auto& out = msgs->outMessages;

    logtrace(msgs, fmt::format(FMT_STRING("sending Port {:d}"), port.host()));
    out.addUint32(3);
    out.addUint8(BtPeerMsgs::Port);
    out.addPort(port);
}

void protocolSendHave(tr_peerMsgsImpl* msgs, tr_piece_index_t index)
{
    auto& out = msgs->outMessages;

    out.addUint32(sizeof(uint8_t) + sizeof(uint32_t));
    out.addUint8(BtPeerMsgs::Have);
    out.addUint32(index);

    logtrace(msgs, fmt::format(FMT_STRING("sending Have {:d}"), index));
    msgs->dbgOutMessageLen();
    msgs->pokeBatchPeriod(LowPriorityIntervalSecs);
}

void protocolSendChoke(tr_peerMsgsImpl* msgs, bool choke)
{
    auto& out = msgs->outMessages;

    out.addUint32(sizeof(uint8_t));
    out.addUint8(choke ? BtPeerMsgs::Choke : BtPeerMsgs::Unchoke);

    logtrace(msgs, choke ? "sending choke" : "sending unchoked");
    msgs->dbgOutMessageLen();
    msgs->pokeBatchPeriod(ImmediatePriorityIntervalSecs);
}

void protocolSendHaveAll(tr_peerMsgsImpl* msgs)
{
    TR_ASSERT(msgs->io->supports_fext());

    auto& out = msgs->outMessages;

    out.addUint32(sizeof(uint8_t));
    out.addUint8(BtPeerMsgs::FextHaveAll);

    logtrace(msgs, "sending HAVE_ALL...");
    msgs->dbgOutMessageLen();
    msgs->pokeBatchPeriod(ImmediatePriorityIntervalSecs);
}

void protocolSendHaveNone(tr_peerMsgsImpl* msgs)
{
    TR_ASSERT(msgs->io->supports_fext());

    auto& out = msgs->outMessages;

    out.addUint32(sizeof(uint8_t));
    out.addUint8(BtPeerMsgs::FextHaveNone);

    logtrace(msgs, "sending HAVE_NONE...");
    msgs->dbgOutMessageLen();
    msgs->pokeBatchPeriod(ImmediatePriorityIntervalSecs);
}

// --- INTEREST

void sendInterest(tr_peerMsgsImpl* msgs, bool b)
{
    TR_ASSERT(msgs != nullptr);

    auto& out = msgs->outMessages;

    logtrace(msgs, b ? "Sending Interested" : "Sending Not Interested");
    out.addUint32(sizeof(uint8_t));
    out.addUint8(b ? BtPeerMsgs::Interested : BtPeerMsgs::NotInterested);

    msgs->pokeBatchPeriod(HighPriorityIntervalSecs);
    msgs->dbgOutMessageLen();
}

bool popNextMetadataRequest(tr_peerMsgsImpl* msgs, int* setme)
{
    if (std::empty(msgs->peerAskedForMetadata))
    {
        return false;
    }

    auto& reqs = msgs->peerAskedForMetadata;
    *setme = reqs.front();
    reqs.pop();
    return true;
}

void cancelAllRequestsToClient(tr_peerMsgsImpl* msgs)
{
    if (auto const must_send_rej = msgs->io->supports_fext(); must_send_rej)
    {
        for (auto const& req : msgs->peer_requested_)
        {
            protocolSendReject(msgs, &req);
        }
    }

    msgs->peer_requested_.clear();
}

// ---

void sendLtepHandshake(tr_peerMsgsImpl* msgs)
{
    auto& out = msgs->outMessages;
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
    tr_variantDictAddBool(&val, TR_KEY_e, msgs->getSession()->encryptionMode() != TR_CLEAR_PREFERRED);

    if (auto const [addr, is_any] = msgs->getSession()->publicAddress(TR_AF_INET6); !is_any)
    {
        TR_ASSERT(addr.is_ipv6());
        tr_variantDictAddRaw(&val, TR_KEY_ipv6, &addr.addr.addr6, sizeof(addr.addr.addr6));
    }

    // http://bittorrent.org/beps/bep_0009.html
    // It also adds "metadata_size" to the handshake message (not the
    // "m" dictionary) specifying an integer value of the number of
    // bytes of the metadata.
    if (auto const info_dict_size = msgs->torrent->infoDictSize();
        allow_metadata_xfer && msgs->torrent->hasMetainfo() && info_dict_size > 0)
    {
        tr_variantDictAddInt(&val, TR_KEY_metadata_size, info_dict_size);
    }

    // http://bittorrent.org/beps/bep_0010.html
    // Local TCP listen port. Allows each side to learn about the TCP
    // port number of the other side. Note that there is no need for the
    // receiving side of the connection to send this extension message,
    // since its port number is already known.
    tr_variantDictAddInt(&val, TR_KEY_p, msgs->getSession()->advertisedPeerPort().host());

    // http://bittorrent.org/beps/bep_0010.html
    // An integer, the number of outstanding request messages this
    // client supports without dropping any. The default in in
    // libtorrent is 250.
    tr_variantDictAddInt(&val, TR_KEY_reqq, ReqQ);

    // https://www.bittorrent.org/beps/bep_0010.html
    // A string containing the compact representation of the ip address this peer sees
    // you as. i.e. this is the receiver's external ip address (no port is included).
    // This may be either an IPv4 (4 bytes) or an IPv6 (16 bytes) address.
    {
        auto buf = std::array<std::byte, TR_ADDRSTRLEN>{};
        auto const begin = std::data(buf);
        auto const end = msgs->io->address().to_compact(begin);
        auto const len = end - begin;
        TR_ASSERT(len == 4 || len == 16);
        tr_variantDictAddRaw(&val, TR_KEY_yourip, begin, len);
    }

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

    auto payload = tr_variantToStr(&val, TR_VARIANT_FMT_BENC);

    out.addUint32(2 * sizeof(uint8_t) + std::size(payload));
    out.addUint8(BtPeerMsgs::Ltep);
    out.addUint8(LtepMessages::Handshake);
    out.add(payload);
    msgs->pokeBatchPeriod(ImmediatePriorityIntervalSecs);
    msgs->dbgOutMessageLen();

    /* cleanup */
    tr_variantClear(&val);
}

void parseLtepHandshake(tr_peerMsgsImpl* msgs, uint32_t len)
{
    msgs->peerSentLtepHandshake = true;

    // LTEP messages are usually just a couple hundred bytes,
    // so try using a strbuf to handle it on the stack
    auto tmp = tr_strbuf<char, 512>{};
    tmp.resize(len);
    msgs->io->read_bytes(std::data(tmp), std::size(tmp));
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
            tr_peerMgrSetUtpFailed(msgs->torrent, msgs->io->address(), false);
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
        msgs->publish(tr_peer_event::GotPort(pex.port));
        logtrace(msgs, fmt::format(FMT_STRING("peer's port is now {:d}"), i));
    }

    uint8_t const* addr = nullptr;
    auto addr_len = size_t{};
    if (msgs->io->is_incoming() && tr_variantDictFindRaw(&val, TR_KEY_ipv4, &addr, &addr_len) && addr_len == 4)
    {
        pex.addr.type = TR_AF_INET;
        memcpy(&pex.addr.addr.addr4, addr, 4);
        tr_peerMgrAddPex(msgs->torrent, TR_PEER_FROM_LTEP, &pex, 1);
    }

    if (msgs->io->is_incoming() && tr_variantDictFindRaw(&val, TR_KEY_ipv6, &addr, &addr_len) && addr_len == 16)
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

    tr_variantClear(&val);
}

void parseUtMetadata(tr_peerMsgsImpl* msgs, uint32_t msglen)
{
    int64_t msg_type = -1;
    int64_t piece = -1;
    int64_t total_size = 0;

    auto tmp = std::vector<char>{};
    tmp.resize(msglen);
    msgs->io->read_bytes(std::data(tmp), std::size(tmp));
    char const* const msg_end = std::data(tmp) + std::size(tmp);

    auto dict = tr_variant{};
    char const* benc_end = nullptr;
    if (tr_variantFromBuf(&dict, TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE, tmp, &benc_end))
    {
        (void)tr_variantDictFindInt(&dict, TR_KEY_msg_type, &msg_type);
        (void)tr_variantDictFindInt(&dict, TR_KEY_piece, &piece);
        (void)tr_variantDictFindInt(&dict, TR_KEY_total_size, &total_size);
        tr_variantClear(&dict);
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
        size_t const piece_len = msg_end - benc_end;
        tr_torrentSetMetadataPiece(msgs->torrent, piece, benc_end, piece_len);
    }

    if (msg_type == MetadataMsgType::Request)
    {
        if (piece >= 0 && msgs->torrent->hasMetainfo() && msgs->torrent->isPublic() &&
            std::size(msgs->peerAskedForMetadata) < MetadataReqQ)
        {
            msgs->peerAskedForMetadata.push(piece);
        }
        else
        {
            auto& out = msgs->outMessages;

            /* build the rejection message */
            auto v = tr_variant{};
            tr_variantInitDict(&v, 2);
            tr_variantDictAddInt(&v, TR_KEY_msg_type, MetadataMsgType::Reject);
            tr_variantDictAddInt(&v, TR_KEY_piece, piece);
            auto const payload = tr_variantToStr(&v, TR_VARIANT_FMT_BENC);

            /* write it out as a LTEP message to our outMessages buffer */
            out.addUint32(2 * sizeof(uint8_t) + std::size(payload));
            out.addUint8(BtPeerMsgs::Ltep);
            out.addUint8(msgs->ut_metadata_id);
            out.add(payload);
            msgs->pokeBatchPeriod(HighPriorityIntervalSecs);
            msgs->dbgOutMessageLen();

            /* cleanup */
            tr_variantClear(&v);
        }
    }
}

void parseUtPex(tr_peerMsgsImpl* msgs, uint32_t msglen)
{
    auto* const tor = msgs->torrent;
    if (!tor->allowsPex())
    {
        return;
    }

    auto tmp = std::vector<char>{};
    tmp.resize(msglen);
    msgs->io->read_bytes(std::data(tmp), std::size(tmp));

    if (tr_variant val; tr_variantFromBuf(&val, TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE, tmp))
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

            auto pex = tr_pex::from_compact_ipv4(added, added_len, added_f, added_f_len);
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

            auto pex = tr_pex::from_compact_ipv6(added, added_len, added_f, added_f_len);
            pex.resize(std::min(MaxPexPeerCount, std::size(pex)));
            tr_peerMgrAddPex(tor, TR_PEER_FROM_PEX, std::data(pex), std::size(pex));
        }

        tr_variantClear(&val);
    }
}

void parseLtep(tr_peerMsgsImpl* msgs, uint32_t msglen)
{
    TR_ASSERT(msglen > 0);

    auto ltep_msgid = uint8_t{};
    msgs->io->read_uint8(&ltep_msgid);
    msglen--;

    if (ltep_msgid == LtepMessages::Handshake)
    {
        logtrace(msgs, "got ltep handshake");
        parseLtepHandshake(msgs, msglen);

        if (msgs->io->supports_ltep())
        {
            sendLtepHandshake(msgs);
            msgs->sendPex();
        }
    }
    else if (ltep_msgid == UT_PEX_ID)
    {
        logtrace(msgs, "got ut pex");
        msgs->peerSupportsPex = true;
        parseUtPex(msgs, msglen);
    }
    else if (ltep_msgid == UT_METADATA_ID)
    {
        logtrace(msgs, "got ut metadata");
        msgs->peerSupportsMetadataXfer = true;
        parseUtMetadata(msgs, msglen);
    }
    else
    {
        logtrace(msgs, fmt::format(FMT_STRING("skipping unknown ltep message ({:d})"), static_cast<int>(ltep_msgid)));
        msgs->io->read_buffer_drain(msglen);
    }
}

ReadState readBtLength(tr_peerMsgsImpl* msgs, size_t inlen)
{
    auto len = uint32_t{};
    if (inlen < sizeof(len))
    {
        return READ_LATER;
    }

    msgs->io->read_uint32(&len);
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

ReadState readBtMessage(tr_peerMsgsImpl* /*msgs*/, size_t /*inlen*/);

ReadState readBtId(tr_peerMsgsImpl* msgs, size_t inlen)
{
    if (inlen < sizeof(uint8_t))
    {
        return READ_LATER;
    }

    auto id = uint8_t{};
    msgs->io->read_uint8(&id);
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

    return readBtMessage(msgs, inlen - 1);
}

void prefetchPieces(tr_peerMsgsImpl* msgs)
{
    if (!msgs->getSession()->allowsPrefetch())
    {
        return;
    }

    // ensure that the first `PrefetchMax` items in `msgs->peer_requested_` are prefetched.
    auto& requests = msgs->peer_requested_;
    for (size_t i = 0, n = std::min(PrefetchMax, std::size(requests)); i < n; ++i)
    {
        if (auto& req = requests[i]; !req.prefetched)
        {
            msgs->getSession()->cache->prefetchBlock(msgs->torrent, msgs->torrent->pieceLoc(req.index, req.offset), req.length);
            req.prefetched = true;
        }
    }
}

[[nodiscard]] bool canAddRequestFromPeer(tr_peerMsgsImpl const* const msgs, struct peer_request const& req)
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

void peerMadeRequest(tr_peerMsgsImpl* msgs, struct peer_request const* req)
{
    if (canAddRequestFromPeer(msgs, *req))
    {
        msgs->peer_requested_.emplace_back(*req);
        prefetchPieces(msgs);
    }
    else if (msgs->io->supports_fext())
    {
        protocolSendReject(msgs, req);
    }
}

bool messageLengthIsCorrect(tr_peerMsgsImpl const* msg, uint8_t id, uint32_t len)
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

int clientGotBlock(tr_peerMsgsImpl* msgs, std::unique_ptr<std::vector<uint8_t>>& block_data, tr_block_index_t block);

ReadState readBtPiece(tr_peerMsgsImpl* msgs, size_t inlen, size_t* setme_piece_bytes_read)
{
    TR_ASSERT(msgs->io->read_buffer_size() >= inlen);

    logtrace(msgs, "In readBtPiece");

    // If this is the first we've seen of the piece data, parse out the header
    if (!msgs->incoming.block_req)
    {
        if (inlen < 8)
        {
            return READ_LATER;
        }

        auto req = peer_request{};
        msgs->io->read_uint32(&req.index);
        msgs->io->read_uint32(&req.offset);
        req.length = msgs->incoming.length - 9;
        logtrace(msgs, fmt::format(FMT_STRING("got incoming block header {:d}:{:d}->{:d}"), req.index, req.offset, req.length));
        msgs->incoming.block_req = req;
        return READ_NOW;
    }

    auto& req = msgs->incoming.block_req;
    auto const loc = msgs->torrent->pieceLoc(req->index, req->offset);
    auto const block = loc.block;
    auto const block_size = msgs->torrent->blockSize(block);
    auto& block_buf = msgs->incoming.block_buf[block];
    if (!block_buf)
    {
        block_buf = std::make_unique<std::vector<uint8_t>>();
        block_buf->reserve(block_size);
    }

    // read in another chunk of data
    auto const n_left_in_block = block_size - std::size(*block_buf);
    auto const n_left_in_req = size_t{ req->length };
    auto const n_to_read = std::min({ n_left_in_block, n_left_in_req, inlen });
    auto const old_length = std::size(*block_buf);
    block_buf->resize(old_length + n_to_read);
    msgs->io->read_bytes(&((*block_buf)[old_length]), n_to_read);

    msgs->publish(tr_peer_event::GotPieceData(n_to_read));
    *setme_piece_bytes_read += n_to_read;
    logtrace(
        msgs,
        fmt::format(
            FMT_STRING("got {:d} bytes for block {:d}:{:d}->{:d} ... {:d} remain in req, {:d} remain in block"),
            n_to_read,
            req->index,
            req->offset,
            req->length,
            req->length,
            block_size - std::size(*block_buf)));

    // if we didn't read enough to finish off the request,
    // update the table and wait for more
    if (n_to_read < n_left_in_req)
    {
        auto new_loc = msgs->torrent->byteLoc(loc.byte + n_to_read);
        req->index = new_loc.piece;
        req->offset = new_loc.piece_offset;
        req->length -= n_to_read;
        return READ_LATER;
    }

    // we've fully read this message
    req.reset();
    msgs->state = AwaitingBt::Length;

    // if we didn't read enough to finish off the block,
    // update the table and wait for more
    if (std::size(*block_buf) < block_size)
    {
        return READ_LATER;
    }

    return clientGotBlock(msgs, block_buf, block) != 0 ? READ_ERR : READ_NOW;
}

ReadState readBtMessage(tr_peerMsgsImpl* msgs, size_t inlen)
{
    uint8_t const id = msgs->incoming.id;
#ifdef TR_ENABLE_ASSERTS
    auto const start_buflen = msgs->io->read_buffer_size();
#endif
    bool const fext = msgs->io->supports_fext();

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
        msgs->publish(tr_peer_event::GotError(EMSGSIZE));
        return READ_ERR;
    }

    switch (id)
    {
    case BtPeerMsgs::Choke:
        logtrace(msgs, "got Choke");
        msgs->client_is_choked_ = true;

        if (!fext)
        {
            msgs->publish(tr_peer_event::GotChoke());
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
        msgs->io->read_uint32(&ui32);
        logtrace(msgs, fmt::format(FMT_STRING("got Have: {:d}"), ui32));

        if (msgs->torrent->hasMetainfo() && ui32 >= msgs->torrent->pieceCount())
        {
            msgs->publish(tr_peer_event::GotError(ERANGE));
            return READ_ERR;
        }

        /* a peer can send the same HAVE message twice... */
        if (!msgs->have_.test(ui32))
        {
            msgs->have_.set(ui32);
            msgs->publish(tr_peer_event::GotHave(ui32));
        }

        msgs->invalidatePercentDone();
        break;

    case BtPeerMsgs::Bitfield:
        {
            logtrace(msgs, "got a bitfield");
            auto tmp = std::vector<uint8_t>(msglen);
            msgs->io->read_bytes(std::data(tmp), std::size(tmp));
            msgs->have_ = tr_bitfield{ msgs->torrent->hasMetainfo() ? msgs->torrent->pieceCount() : std::size(tmp) * 8 };
            msgs->have_.setRaw(std::data(tmp), std::size(tmp));
            msgs->publish(tr_peer_event::GotBitfield(&msgs->have_));
            msgs->invalidatePercentDone();
            break;
        }

    case BtPeerMsgs::Request:
        {
            struct peer_request r;
            msgs->io->read_uint32(&r.index);
            msgs->io->read_uint32(&r.offset);
            msgs->io->read_uint32(&r.length);
            logtrace(msgs, fmt::format(FMT_STRING("got Request: {:d}:{:d}->{:d}"), r.index, r.offset, r.length));
            peerMadeRequest(msgs, &r);
            break;
        }

    case BtPeerMsgs::Cancel:
        {
            struct peer_request r;
            msgs->io->read_uint32(&r.index);
            msgs->io->read_uint32(&r.offset);
            msgs->io->read_uint32(&r.length);
            msgs->cancels_sent_to_client.add(tr_time(), 1);
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

            auto hport = uint16_t{};
            msgs->io->read_uint16(&hport); // read_uint16 performs ntoh
            if (auto const dht_port = tr_port::fromHost(hport); !std::empty(dht_port))
            {
                msgs->dht_port = dht_port;
                msgs->getSession()->addDhtNode(msgs->io->address(), msgs->dht_port);
            }
        }
        break;

    case BtPeerMsgs::FextSuggest:
        logtrace(msgs, "Got a BtPeerMsgs::FextSuggest");
        msgs->io->read_uint32(&ui32);

        if (fext)
        {
            msgs->publish(tr_peer_event::GotSuggest(ui32));
        }
        else
        {
            msgs->publish(tr_peer_event::GotError(EMSGSIZE));
            return READ_ERR;
        }

        break;

    case BtPeerMsgs::FextAllowedFast:
        logtrace(msgs, "Got a BtPeerMsgs::FextAllowedFast");
        msgs->io->read_uint32(&ui32);

        if (fext)
        {
            msgs->publish(tr_peer_event::GotAllowedFast(ui32));
        }
        else
        {
            msgs->publish(tr_peer_event::GotError(EMSGSIZE));
            return READ_ERR;
        }

        break;

    case BtPeerMsgs::FextHaveAll:
        logtrace(msgs, "Got a BtPeerMsgs::FextHaveAll");

        if (fext)
        {
            msgs->have_.setHasAll();
            msgs->publish(tr_peer_event::GotHaveAll());
            msgs->invalidatePercentDone();
        }
        else
        {
            msgs->publish(tr_peer_event::GotError(EMSGSIZE));
            return READ_ERR;
        }

        break;

    case BtPeerMsgs::FextHaveNone:
        logtrace(msgs, "Got a BtPeerMsgs::FextHaveNone");

        if (fext)
        {
            msgs->have_.setHasNone();
            msgs->publish(tr_peer_event::GotHaveNone());
            msgs->invalidatePercentDone();
        }
        else
        {
            msgs->publish(tr_peer_event::GotError(EMSGSIZE));
            return READ_ERR;
        }

        break;

    case BtPeerMsgs::FextReject:
        {
            struct peer_request r;
            logtrace(msgs, "Got a BtPeerMsgs::FextReject");
            msgs->io->read_uint32(&r.index);
            msgs->io->read_uint32(&r.offset);
            msgs->io->read_uint32(&r.length);

            if (fext)
            {
                msgs->publish(
                    tr_peer_event::GotRejected(msgs->torrent->blockInfo(), msgs->torrent->pieceLoc(r.index, r.offset).block));
            }
            else
            {
                msgs->publish(tr_peer_event::GotError(EMSGSIZE));
                return READ_ERR;
            }

            break;
        }

    case BtPeerMsgs::Ltep:
        logtrace(msgs, "Got a BtPeerMsgs::Ltep");
        parseLtep(msgs, msglen);
        break;

    default:
        logtrace(msgs, fmt::format(FMT_STRING("peer sent us an UNKNOWN: {:d}"), static_cast<int>(id)));
        msgs->io->read_buffer_drain(msglen);
        break;
    }

    TR_ASSERT(msglen + 1 == msgs->incoming.length);
    TR_ASSERT(msgs->io->read_buffer_size() == start_buflen - msglen);

    msgs->state = AwaitingBt::Length;
    return READ_NOW;
}

/* returns 0 on success, or an errno on failure */
int clientGotBlock(tr_peerMsgsImpl* msgs, std::unique_ptr<std::vector<uint8_t>>& block_data, tr_block_index_t const block)
{
    TR_ASSERT(msgs != nullptr);

    tr_torrent const* const tor = msgs->torrent;
    auto const n_expected = msgs->torrent->blockSize(block);

    if (!block_data)
    {
        logdbg(msgs, fmt::format("wrong block size: expected {:d}, got {:d}", n_expected, 0));
        return EMSGSIZE;
    }

    if (std::size(*block_data) != msgs->torrent->blockSize(block))
    {
        logdbg(msgs, fmt::format("wrong block size: expected {:d}, got {:d}", n_expected, std::size(*block_data)));
        block_data->clear();
        return EMSGSIZE;
    }

    logtrace(msgs, fmt::format(FMT_STRING("got block {:d}"), block));

    if (!tr_peerMgrDidPeerRequest(msgs->torrent, msgs, block))
    {
        logdbg(msgs, "we didn't ask for this message...");
        block_data->clear();
        return 0;
    }

    auto const loc = msgs->torrent->blockLoc(block);
    if (msgs->torrent->hasPiece(loc.piece))
    {
        logtrace(msgs, "we did ask for this message, but the piece is already complete...");
        block_data->clear();
        return 0;
    }

    // NB: if writeBlock() fails the torrent may be paused.
    // If this happens, `msgs` will be a dangling pointer and must no longer be used.
    if (auto const err = msgs->getSession()->cache->writeBlock(tor->id(), block, block_data); err != 0)
    {
        return err;
    }

    msgs->blame.set(loc.piece);
    msgs->incoming.block_buf.erase(block);
    msgs->publish(tr_peer_event::GotBlock(tor->blockInfo(), block));

    return 0;
}

void didWrite(tr_peerIo* /*io*/, size_t bytes_written, bool was_piece_data, void* vmsgs)
{
    auto* const msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);

    if (was_piece_data)
    {
        msgs->publish(tr_peer_event::SentPieceData(bytes_written));
    }

    peerPulse(msgs);
}

ReadState canRead(tr_peerIo* io, void* vmsgs, size_t* piece)
{
    auto* msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);
    size_t const inlen = io->read_buffer_size();

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
        ret = readBtPiece(msgs, inlen, piece);
    }
    else
    {
        switch (msgs->state)
        {
        case AwaitingBt::Length:
            ret = readBtLength(msgs, inlen);
            break;

        case AwaitingBt::Id:
            ret = readBtId(msgs, inlen);
            break;

        case AwaitingBt::Message:
            ret = readBtMessage(msgs, inlen);
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

    return ret;
}

// ---

void updateDesiredRequestCount(tr_peerMsgsImpl* msgs)
{
    msgs->desired_request_count = msgs->canRequest().max_blocks;
}

void updateMetadataRequests(tr_peerMsgsImpl* msgs, time_t now)
{
    if (!msgs->peerSupportsMetadataXfer)
    {
        return;
    }

    if (auto const piece = tr_torrentGetNextMetadataRequest(msgs->torrent, now); piece)
    {
        auto& out = msgs->outMessages;

        /* build the data message */
        auto tmp = tr_variant{};
        tr_variantInitDict(&tmp, 3);
        tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Request);
        tr_variantDictAddInt(&tmp, TR_KEY_piece, *piece);
        auto const payload = tr_variantToStr(&tmp, TR_VARIANT_FMT_BENC);

        logtrace(msgs, fmt::format(FMT_STRING("requesting metadata piece #{:d}"), *piece));

        /* write it out as a LTEP message to our outMessages buffer */
        out.addUint32(2 * sizeof(uint8_t) + std::size(payload));
        out.addUint8(BtPeerMsgs::Ltep);
        out.addUint8(msgs->ut_metadata_id);
        out.add(payload);
        msgs->pokeBatchPeriod(HighPriorityIntervalSecs);
        msgs->dbgOutMessageLen();

        /* cleanup */
        tr_variantClear(&tmp);
    }
}

void updateBlockRequests(tr_peerMsgsImpl* msgs)
{
    auto* const tor = msgs->torrent;

    if (!tor->clientCanDownload())
    {
        return;
    }

    auto const n_active = tr_peerMgrCountActiveRequestsToPeer(tor, msgs);
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

    if (auto const requests = tr_peerMgrGetNextRequests(tor, msgs, n_wanted); !std::empty(requests))
    {
        msgs->requestBlocks(std::data(requests), std::size(requests));
    }
}

size_t fillOutputBuffer(tr_peerMsgsImpl* msgs, time_t now)
{
    size_t bytes_written = 0;
    struct peer_request req;
    bool const have_messages = !std::empty(msgs->outMessages);
    bool const fext = msgs->io->supports_fext();

    // --- Protocol messages

    if (have_messages && msgs->outMessagesBatchedAt == 0) /* fresh batch */
    {
        logtrace(msgs, fmt::format(FMT_STRING("started an outMessages batch (length is {:d})"), std::size(msgs->outMessages)));
        msgs->outMessagesBatchedAt = now;
    }
    else if (have_messages && now - msgs->outMessagesBatchedAt >= msgs->outMessagesBatchPeriod)
    {
        auto const len = std::size(msgs->outMessages);
        /* flush the protocol messages */
        logtrace(msgs, fmt::format(FMT_STRING("flushing outMessages... to {:p} (length is {:d})"), fmt::ptr(msgs->io), len));
        msgs->io->write(msgs->outMessages, false);
        msgs->clientSentAnythingAt = now;
        msgs->outMessagesBatchedAt = 0;
        msgs->outMessagesBatchPeriod = LowPriorityIntervalSecs;
        bytes_written += len;
    }

    // --- Metadata Pieces

    if (auto piece = int{};
        msgs->io->get_write_buffer_space(now) >= METADATA_PIECE_SIZE && popNextMetadataRequest(msgs, &piece))
    {
        auto ok = bool{ false };

        if (auto const piece_data = tr_torrentGetMetadataPiece(msgs->torrent, piece); piece_data)
        {
            auto& out = msgs->outMessages;

            /* build the data message */
            auto tmp = tr_variant{};
            tr_variantInitDict(&tmp, 3);
            tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Data);
            tr_variantDictAddInt(&tmp, TR_KEY_piece, piece);
            tr_variantDictAddInt(&tmp, TR_KEY_total_size, msgs->torrent->infoDictSize());
            auto const payload = tr_variantToStr(&tmp, TR_VARIANT_FMT_BENC);

            /* write it out as a LTEP message to our outMessages buffer */
            out.addUint32(2 * sizeof(uint8_t) + std::size(payload) + std::size(*piece_data));
            out.addUint8(BtPeerMsgs::Ltep);
            out.addUint8(msgs->ut_metadata_id);
            out.add(payload);
            out.add(*piece_data);
            msgs->pokeBatchPeriod(HighPriorityIntervalSecs);
            msgs->dbgOutMessageLen();

            tr_variantClear(&tmp);

            ok = true;
        }

        if (!ok) /* send a rejection message */
        {
            auto& out = msgs->outMessages;

            /* build the rejection message */
            auto tmp = tr_variant{};
            tr_variantInitDict(&tmp, 2);
            tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Reject);
            tr_variantDictAddInt(&tmp, TR_KEY_piece, piece);
            auto payload = tr_variantToStr(&tmp, TR_VARIANT_FMT_BENC);

            /* write it out as a LTEP message to our outMessages buffer */
            out.addUint32(2 * sizeof(uint8_t) + std::size(payload));
            out.addUint8(BtPeerMsgs::Ltep);
            out.addUint8(msgs->ut_metadata_id);
            out.add(payload);
            msgs->pokeBatchPeriod(HighPriorityIntervalSecs);
            msgs->dbgOutMessageLen();

            tr_variantClear(&tmp);
        }
    }

    // --- Data Blocks

    if (msgs->io->get_write_buffer_space(now) >= tr_block_info::BlockSize && !std::empty(msgs->peer_requested_))
    {
        req = msgs->peer_requested_.front();
        msgs->peer_requested_.erase(std::begin(msgs->peer_requested_));

        if (msgs->isValidRequest(req) && msgs->torrent->hasPiece(req.index))
        {
            uint32_t const msglen = 4 + 1 + 4 + 4 + req.length;

            auto out = libtransmission::Buffer{};
            out.reserve(msglen);

            out.addUint32(sizeof(uint8_t) + 2 * sizeof(uint32_t) + req.length);
            out.addUint8(BtPeerMsgs::Piece);
            out.addUint32(req.index);
            out.addUint32(req.offset);
            auto buf = std::array<uint8_t, tr_block_info::BlockSize>{};
            bool err = msgs->getSession()->cache->readBlock(
                           msgs->torrent,
                           msgs->torrent->pieceLoc(req.index, req.offset),
                           req.length,
                           std::data(buf)) != 0;
            out.add(std::data(buf), req.length);

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
                logtrace(msgs, fmt::format(FMT_STRING("sending block {:d}:{:d}->{:d}"), req.index, req.offset, req.length));
                auto const n = std::size(out);
                TR_ASSERT(n == msglen);
                msgs->io->write(out, true);
                bytes_written += n;
                msgs->clientSentAnythingAt = now;
                msgs->blocks_sent_to_peer.add(tr_time(), 1);
            }

            if (err)
            {
                bytes_written = 0;
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

    // --- Keepalive

    if (msgs != nullptr && msgs->clientSentAnythingAt != 0 && now - msgs->clientSentAnythingAt > KeepaliveIntervalSecs)
    {
        logtrace(msgs, "sending a keepalive message");
        msgs->outMessages.addUint32(0);
        msgs->pokeBatchPeriod(ImmediatePriorityIntervalSecs);
    }

    return bytes_written;
}

void peerPulse(void* vmsgs)
{
    auto* msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);
    time_t const now = tr_time();

    updateDesiredRequestCount(msgs);
    updateBlockRequests(msgs);
    updateMetadataRequests(msgs, now);

    for (;;)
    {
        if (fillOutputBuffer(msgs, now) < 1)
        {
            break;
        }
    }
}

void gotError(tr_peerIo* /*io*/, tr_error const& /*error*/, void* vmsgs)
{
    static_cast<tr_peerMsgsImpl*>(vmsgs)->publish(tr_peer_event::GotError(ENOTCONN));
}

void sendBitfield(tr_peerMsgsImpl* msgs)
{
    TR_ASSERT(msgs->torrent->hasMetainfo());

    auto& out = msgs->outMessages;

    auto bytes = msgs->torrent->createPieceBitfield();
    out.addUint32(sizeof(uint8_t) + bytes.size());
    out.addUint8(BtPeerMsgs::Bitfield);
    out.add(bytes);
    logtrace(msgs, fmt::format(FMT_STRING("sending bitfield... outMessage size is now {:d}"), std::size(out)));
    msgs->pokeBatchPeriod(ImmediatePriorityIntervalSecs);
}

void tellPeerWhatWeHave(tr_peerMsgsImpl* msgs)
{
    bool const fext = msgs->io->supports_fext();

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

void tr_peerMsgsImpl::sendPex()
{
    // only send pex if both the torrent and peer support it
    if (!this->peerSupportsPex || !this->torrent->allowsPex())
    {
        return;
    }

    auto& old4 = this->pex;
    auto new4 = tr_peerMgrGetPeers(this->torrent, TR_AF_INET, TR_PEERS_CONNECTED, MaxPexPeerCount);
    auto added = std::vector<tr_pex>{};
    added.reserve(std::size(new4));
    std::set_difference(std::begin(new4), std::end(new4), std::begin(old4), std::end(old4), std::back_inserter(added));
    auto dropped = std::vector<tr_pex>{};
    dropped.reserve(std::size(old4));
    std::set_difference(std::begin(old4), std::end(old4), std::begin(new4), std::end(new4), std::back_inserter(dropped));

    auto& old6 = this->pex6;
    auto new6 = tr_peerMgrGetPeers(this->torrent, TR_AF_INET6, TR_PEERS_CONNECTED, MaxPexPeerCount);
    auto added6 = std::vector<tr_pex>{};
    added6.reserve(std::size(new6));
    std::set_difference(std::begin(new6), std::end(new6), std::begin(old6), std::end(old6), std::back_inserter(added6));
    auto dropped6 = std::vector<tr_pex>{};
    dropped6.reserve(std::size(old6));
    std::set_difference(std::begin(old6), std::end(old6), std::begin(new6), std::end(new6), std::back_inserter(dropped6));

    // Some peers give us error messages if we send
    // more than this many peers in a single pex message.
    // https://wiki.theory.org/BitTorrentPeerExchangeConventions
    static auto constexpr MaxPexAdded = size_t{ 50 };
    added.resize(std::min(std::size(added), MaxPexAdded));
    added6.resize(std::min(std::size(added6), MaxPexAdded));
    static auto constexpr MaxPexDropped = size_t{ 50 };
    dropped.resize(std::min(std::size(dropped), MaxPexDropped));
    dropped6.resize(std::min(std::size(dropped6), MaxPexDropped));

    logtrace(
        this,
        fmt::format(
            FMT_STRING("pex: old peer count {:d}+{:d}, new peer count {:d}+{:d}, added {:d}+{:d}, dropped {:d}+{:d}"),
            std::size(old4),
            std::size(old6),
            std::size(new4),
            std::size(new6),
            std::size(added),
            std::size(added6),
            std::size(dropped),
            std::size(dropped6)));

    // if there's nothing to send, then we're done
    if (std::empty(added) && std::empty(dropped) && std::empty(added6) && std::empty(dropped6))
    {
        return;
    }

    auto& out = this->outMessages;

    // update msgs
    std::swap(old4, new4);
    std::swap(old6, new6);

    // build the pex payload
    auto val = tr_variant{};
    tr_variantInitDict(&val, 3); /* ipv6 support: left as 3: speed vs. likelihood? */

    auto tmpbuf = std::vector<std::byte>{};
    tmpbuf.reserve(MaxPexAdded * 18);

    if (!std::empty(added))
    {
        // "added"
        tmpbuf.clear();
        tr_pex::to_compact_ipv4(std::back_inserter(tmpbuf), std::data(added), std::size(added));
        TR_ASSERT(std::size(tmpbuf) == std::size(added) * 6);
        tr_variantDictAddRaw(&val, TR_KEY_added, std::data(tmpbuf), std::size(tmpbuf));

        // "added.f"
        // unset each holepunch flag because we don't support it.
        tmpbuf.resize(std::size(added));
        auto* begin = std::data(tmpbuf);
        auto* walk = begin;
        for (auto const& p : added)
        {
            *walk++ = std::byte{ p.flags } & ~std::byte{ ADDED_F_HOLEPUNCH };
        }

        TR_ASSERT(static_cast<size_t>(walk - begin) == std::size(added));
        tr_variantDictAddRaw(&val, TR_KEY_added_f, begin, walk - begin);
    }

    if (!std::empty(dropped))
    {
        // "dropped"
        tmpbuf.clear();
        tr_pex::to_compact_ipv4(std::back_inserter(tmpbuf), std::data(dropped), std::size(dropped));
        TR_ASSERT(std::size(tmpbuf) == std::size(dropped) * 6);
        tr_variantDictAddRaw(&val, TR_KEY_dropped, std::data(tmpbuf), std::size(tmpbuf));
    }

    if (!std::empty(added6))
    {
        tmpbuf.clear();
        tr_pex::to_compact_ipv6(std::back_inserter(tmpbuf), std::data(added6), std::size(added6));
        TR_ASSERT(std::size(tmpbuf) == std::size(added6) * 18);
        tr_variantDictAddRaw(&val, TR_KEY_added6, std::data(tmpbuf), std::size(tmpbuf));

        // "added6.f"
        // unset each holepunch flag because we don't support it.
        tmpbuf.resize(std::size(added6));
        auto* begin = std::data(tmpbuf);
        auto* walk = begin;
        for (auto const& p : added6)
        {
            *walk++ = std::byte{ p.flags } & ~std::byte{ ADDED_F_HOLEPUNCH };
        }

        TR_ASSERT(static_cast<size_t>(walk - begin) == std::size(added6));
        tr_variantDictAddRaw(&val, TR_KEY_added6_f, begin, walk - begin);
    }

    if (!std::empty(dropped6))
    {
        // "dropped6"
        tmpbuf.clear();
        tr_pex::to_compact_ipv6(std::back_inserter(tmpbuf), std::data(dropped6), std::size(dropped6));
        TR_ASSERT(std::size(tmpbuf) == std::size(dropped6) * 18);
        tr_variantDictAddRaw(&val, TR_KEY_dropped6, std::data(tmpbuf), std::size(tmpbuf));
    }

    /* write the pex message */
    auto payload = tr_variantToStr(&val, TR_VARIANT_FMT_BENC);
    out.addUint32(2 * sizeof(uint8_t) + std::size(payload));
    out.addUint8(BtPeerMsgs::Ltep);
    out.addUint8(this->ut_pex_id);
    out.add(payload);
    this->pokeBatchPeriod(HighPriorityIntervalSecs);
    logtrace(this, fmt::format(FMT_STRING("sending a pex message; outMessage size is now {:d}"), std::size(out)));
    this->dbgOutMessageLen();

    tr_variantClear(&val);
}

} // namespace

tr_peerMsgs::~tr_peerMsgs()
{
    [[maybe_unused]] auto const n_prev = n_peers_--;
    TR_ASSERT(n_prev > 0U);
}

tr_peerMsgs* tr_peerMsgsNew(tr_torrent* torrent, peer_atom* atom, std::shared_ptr<tr_peerIo> io, tr_peer_callback callback)
{
    return new tr_peerMsgsImpl(torrent, atom, std::move(io), callback);
}
