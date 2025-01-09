// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <cstddef>
#include <cstdint> // uint8_t, uint32_t, int64_t
#include <ctime>
#include <deque>
#include <iterator>
#include <memory> // std::unique_ptr
#include <optional>
#include <queue>
#include <ratio>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/core.h>

#include <small/vector.hpp>

#include "libtransmission/transmission.h"

#include "libtransmission/bitfield.h"
#include "libtransmission/block-info.h"
#include "libtransmission/cache.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/interned-string.h"
#include "libtransmission/log.h"
#include "libtransmission/peer-common.h"
#include "libtransmission/peer-io.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/peer-msgs.h"
#include "libtransmission/quark.h"
#include "libtransmission/session.h"
#include "libtransmission/timer.h"
#include "libtransmission/torrent-magnet.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-buffer.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"
#include "libtransmission/version.h"

struct tr_error;

#ifndef EBADMSG
#define EBADMSG EINVAL
#endif

using namespace std::literals;

namespace
{
// initial capacity is big enough to hold a BtPeerMsgs::Piece message
using MessageBuffer = libtransmission::StackBuffer<tr_block_info::BlockSize + 16U, std::byte, std::ratio<5, 1>>;
using MessageReader = libtransmission::BufferReader<std::byte>;
using MessageWriter = libtransmission::BufferWriter<std::byte>;

// these values are hardcoded by various BEPs as noted
namespace BtPeerMsgs
{
// https://www.bittorrent.org/beps/bep_0003.html#peer-messages
auto constexpr Choke = uint8_t{ 0 };
auto constexpr Unchoke = uint8_t{ 1 };
auto constexpr Interested = uint8_t{ 2 };
auto constexpr NotInterested = uint8_t{ 3 };
auto constexpr Have = uint8_t{ 4 };
auto constexpr Bitfield = uint8_t{ 5 };
auto constexpr Request = uint8_t{ 6 };
auto constexpr Piece = uint8_t{ 7 };
auto constexpr Cancel = uint8_t{ 8 };

// https://www.bittorrent.org/beps/bep_0005.html#bittorrent-protocol-extension
auto constexpr DhtPort = uint8_t{ 9 };

// https://www.bittorrent.org/beps/bep_0006.html
auto constexpr FextSuggest = uint8_t{ 13 };
auto constexpr FextHaveAll = uint8_t{ 14 };
auto constexpr FextHaveNone = uint8_t{ 15 };
auto constexpr FextReject = uint8_t{ 16 };
auto constexpr FextAllowedFast = uint8_t{ 17 };

// https://www.bittorrent.org/beps/bep_0010.html
// see also LtepMessageIds below
auto constexpr Ltep = uint8_t{ 20 };

[[nodiscard]] constexpr std::string_view debug_name(uint8_t type) noexcept
{
    switch (type)
    {
    case Bitfield:
        return "bitfield"sv;
    case Cancel:
        return "cancel"sv;
    case Choke:
        return "choke"sv;
    case FextAllowedFast:
        return "fext-allow-fast"sv;
    case FextHaveAll:
        return "fext-have-all"sv;
    case FextHaveNone:
        return "fext-have-none"sv;
    case FextReject:
        return "fext-reject"sv;
    case FextSuggest:
        return "fext-suggest"sv;
    case Have:
        return "have"sv;
    case Interested:
        return "interested"sv;
    case Ltep:
        return "ltep"sv;
    case NotInterested:
        return "not-interested"sv;
    case Piece:
        return "piece"sv;
    case DhtPort:
        return "dht-port"sv;
    case Request:
        return "request"sv;
    case Unchoke:
        return "unchoke"sv;
    default:
        return "unknown"sv;
    }
}

} // namespace BtPeerMsgs

namespace LtepMessages
{

// https://www.bittorrent.org/beps/bep_0010.html
auto constexpr Handshake = uint8_t{ 0 };

} // namespace LtepMessages

// https://www.bittorrent.org/beps/bep_0010.html
// Client-defined extension message IDs that we tell peers about
// in the LTEP handshake and will respond to when sent in an LTEP
// message.
enum LtepMessageIds : uint8_t
{
    // we support peer exchange (bep 11)
    // https://www.bittorrent.org/beps/bep_0011.html
    UT_PEX_ID = 1,

    // we support sending metadata files (bep 9)
    // https://www.bittorrent.org/beps/bep_0009.html
    // see also MetadataMsgType below
    UT_METADATA_ID = 3,
};

// https://www.bittorrent.org/beps/bep_0009.html
namespace MetadataMsgType
{

auto constexpr Request = 0;
auto constexpr Data = 1;
auto constexpr Reject = 2;

} // namespace MetadataMsgType

auto constexpr MinChokePeriodSec = time_t{ 10 };

// idle seconds before we send a keepalive
auto constexpr KeepaliveIntervalSecs = time_t{ 100 };

auto constexpr MetadataReqQ = size_t{ 64U };

auto constexpr PeerReqQDefault = 500U;

// when we're making requests from another peer,
// batch them together to send enough requests to
// meet our bandwidth goals for the next N seconds
auto constexpr RequestBufSecs = time_t{ 10 };

// ---

auto constexpr MaxPexPeerCount = size_t{ 50U };

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

    [[nodiscard]] static auto from_block(tr_torrent const& tor, tr_block_index_t block) noexcept
    {
        auto const loc = tor.block_loc(block);
        return peer_request{ loc.piece, loc.piece_offset, tor.block_size(block) };
    }
};

// ---

/* this is raw, unchanged data from the peer regarding
 * the current message that it's sending us. */
struct tr_incoming
{
    std::optional<uint32_t> length; // the full message payload length. Includes the +1 for id length
    std::optional<uint8_t> id; // the protocol message, e.g. BtPeerMsgs::Piece
    MessageBuffer payload;

    struct incoming_piece_data
    {
        explicit incoming_piece_data(uint32_t block_size)
            : buf{ std::make_unique<Cache::BlockData>(block_size) }
            , block_size_{ block_size }
        {
        }

        [[nodiscard]] bool add_span(size_t begin, size_t end)
        {
            if (begin > end || end > block_size_)
            {
                return false;
            }

            for (; begin < end; ++begin)
            {
                have_.set(begin);
            }

            return true;
        }

        [[nodiscard]] auto has_all() const noexcept
        {
            return have_.count() >= block_size_;
        }

        std::unique_ptr<Cache::BlockData> buf;

    private:
        std::bitset<tr_block_info::BlockSize> have_;
        uint32_t const block_size_;
    };

    std::unordered_map<tr_block_index_t, incoming_piece_data> blocks;
};

#define myLogMacro(msgs, level, text) \
    do \
    { \
        if (tr_logLevelIsActive(level)) \
        { \
            tr_logAddMessage( \
                __FILE__, \
                __LINE__, \
                (level), \
                fmt::format("{:s} [{:s}]: {:s}", (msgs)->display_name(), (msgs)->user_agent().sv(), text), \
                (msgs)->tor_.name()); \
        } \
    } while (0)

#define logdbg(msgs, text) myLogMacro(msgs, TR_LOG_DEBUG, text)
#define logtrace(msgs, text) myLogMacro(msgs, TR_LOG_TRACE, text)
#define logwarn(msgs, text) myLogMacro(msgs, TR_LOG_WARN, text)

using ReadResult = std::pair<ReadState, size_t /*n_piece_data_bytes_read*/>;

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
 * @see tr_peer
 * @see tr_peer_info
 */
class tr_peerMsgsImpl final : public tr_peerMsgs
{
public:
    tr_peerMsgsImpl(
        tr_torrent& torrent_in,
        std::shared_ptr<tr_peer_info> peer_info_in,
        std::shared_ptr<tr_peerIo> io_in,
        tr_interned_string client,
        tr_peer_callback_bt callback,
        void* callback_data)
        : tr_peerMsgs{ torrent_in, std::move(peer_info_in), client, io_in->is_encrypted(), io_in->is_incoming(), io_in->is_utp() }
        , tor_{ torrent_in }
        , io_{ std::move(io_in) }
        , have_{ torrent_in.piece_count() }
        , callback_{ callback }
        , callback_data_{ callback_data }
    {
        if (tor_.allows_pex())
        {
            pex_timer_ = session->timerMaker().create([this]() { send_ut_pex(); });
            pex_timer_->start_repeating(SendPexInterval);
        }

        if (io_->supports_ltep())
        {
            send_ltep_handshake();
        }

        protocol_send_bitfield();

        if (session->allowsDHT() && io_->supports_dht())
        {
            protocol_send_dht_port(session->udpPort());
        }

        io_->set_callbacks(can_read, did_write, got_error, this);
        update_desired_request_count();

        update_active();
    }

    tr_peerMsgsImpl(tr_peerMsgsImpl&&) = delete;
    tr_peerMsgsImpl(tr_peerMsgsImpl const&) = delete;
    tr_peerMsgsImpl& operator=(tr_peerMsgsImpl&&) = delete;
    tr_peerMsgsImpl& operator=(tr_peerMsgsImpl const&) = delete;

    ~tr_peerMsgsImpl() override
    {
        set_active(TR_UP, false);
        set_active(TR_DOWN, false);

        if (io_)
        {
            io_->clear();
        }
    }

    // ---

    [[nodiscard]] Speed get_piece_speed(uint64_t now, tr_direction dir) const override
    {
        return io_->get_piece_speed(now, dir);
    }

    [[nodiscard]] size_t active_req_count(tr_direction dir) const noexcept override
    {
        switch (dir)
        {
        case TR_CLIENT_TO_PEER: // requests we sent
            return active_requests.count();

        case TR_PEER_TO_CLIENT: // requests they sent
            return std::size(peer_requested_);

        default:
            TR_ASSERT(0);
            return {};
        }
    }

    [[nodiscard]] tr_socket_address socket_address() const override
    {
        return io_->socket_address();
    }

    [[nodiscard]] std::string display_name() const override
    {
        return socket_address().display_name();
    }

    [[nodiscard]] tr_bitfield const& has() const noexcept override
    {
        return have_;
    }

    // ---

    void ban() override
    {
        peer_info->ban();
        disconnect_soon();
    }

    void on_torrent_got_metainfo() noexcept override
    {
        // A peer may not be interesting to us anymore after
        // sending us metadata, so do a status update
        update_active();
    }

    void maybe_cancel_block_request(tr_block_index_t block) override
    {
        if (active_requests.test(block))
        {
            cancels_sent_to_peer.add(tr_time(), 1);
            active_requests.unset(block);
            publish(tr_peer_event::SentCancel(tor_.block_info(), block));
            protocol_send_cancel(peer_request::from_block(tor_, block));
        }
    }

    void set_choke(bool peer_is_choked) override
    {
        auto const now = tr_time();
        auto const fibrillation_time = now - MinChokePeriodSec;

        if (choke_changed_at_ > fibrillation_time)
        {
            logtrace(this, fmt::format("Not changing choke to {} to avoid fibrillation", peer_is_choked));
        }
        else if (this->peer_is_choked() != peer_is_choked)
        {
            set_peer_choked(peer_is_choked);

            // https://www.bittorrent.org/beps/bep_0006.html#reject-request
            // A peer SHOULD choke first and then reject requests so that
            // the peer receiving the choke does not re-request the pieces.
            protocol_send_choke(peer_is_choked);
            if (peer_is_choked)
            {
                reject_all_requests();
            }

            choke_changed_at_ = now;
            update_active(TR_CLIENT_TO_PEER);
        }
    }

    void pulse() override;

    void on_piece_completed(tr_piece_index_t piece) override
    {
        protocol_send_have(piece);

        // since we have more pieces now, we might not be interested in this peer
        update_interest();
    }

    void set_interested(bool interested) override
    {
        if (client_is_interested() != interested)
        {
            set_client_interested(interested);
            protocol_send_interest(interested);
            update_active(TR_PEER_TO_CLIENT);
        }
    }

    // ---

    void request_blocks(tr_block_span_t const* block_spans, size_t n_spans) override
    {
        TR_ASSERT(tor_.client_can_download());
        TR_ASSERT(client_is_interested());
        TR_ASSERT(!client_is_choked());

        if (active_requests.has_none())
        {
            request_timeout_base_ = tr_time();
        }

        for (auto const *span = block_spans, *span_end = span + n_spans; span != span_end; ++span)
        {
            auto const [block_begin, block_end] = *span;
            for (auto block = block_begin; block < block_end; ++block)
            {
                // Note that requests can't cross over a piece boundary.
                // So if a piece isn't evenly divisible by the block size,
                // we need to split our block request info per-piece chunks.
                auto const byte_begin = tor_.block_loc(block).byte;
                auto const block_size = tor_.block_size(block);
                auto const byte_end = byte_begin + block_size;
                for (auto offset = byte_begin; offset < byte_end;)
                {
                    auto const loc = tor_.byte_loc(offset);
                    auto const left_in_block = block_size - loc.block_offset;
                    auto const left_in_piece = tor_.piece_size(loc.piece) - loc.piece_offset;
                    auto const req_len = std::min(left_in_block, left_in_piece);
                    protocol_send_request({ loc.piece, loc.piece_offset, req_len });
                    offset += req_len;
                }
            }

            active_requests.set_span(block_begin, block_end);
            publish(tr_peer_event::SentRequest(tor_.block_info(), *span));
        }
    }

    void update_active()
    {
        update_active(TR_CLIENT_TO_PEER);
        update_active(TR_PEER_TO_CLIENT);
    }

    void update_active(tr_direction direction)
    {
        TR_ASSERT(tr_isDirection(direction));
        set_active(direction, calculate_active(direction));
    }

    [[nodiscard]] bool calculate_active(tr_direction direction) const
    {
        if (direction == TR_CLIENT_TO_PEER)
        {
            return peer_is_interested() && !peer_is_choked();
        }

        // TR_PEER_TO_CLIENT

        if (!tor_.has_metainfo())
        {
            return true;
        }

        auto const active = client_is_interested() && !client_is_choked();
        TR_ASSERT(!active || !tor_.is_done());
        return active;
    }

private:
    // ---

    void update_interest()
    {
        // TODO(ckerr) -- might need to poke the mgr on startup

        // additional note (tearfur)
        // by "poke the mgr", Charles probably meant calling isPeerInteresting(),
        // then pass the result to set_interesting()
    }

    // ---

    [[nodiscard]] bool is_valid_request(peer_request const& req) const;

    void reject_all_requests()
    {
        auto& queue = peer_requested_;

        if (auto const must_send_rej = io_->supports_fext(); must_send_rej)
        {
            std::for_each(std::begin(queue), std::end(queue), [this](peer_request const& req) { protocol_send_reject(req); });
        }

        queue.clear();
    }

    [[nodiscard]] bool can_add_request_from_peer(peer_request const& req);

    void on_peer_made_request(peer_request const& req)
    {
        if (can_add_request_from_peer(req))
        {
            peer_requested_.emplace_back(req);

            fill_output_buffer(tr_time(), tr_time_msec());
        }
        else if (io_->supports_fext())
        {
            protocol_send_reject(req);
        }
    }

    // how many blocks could we request from this peer right now?
    [[nodiscard]] size_t max_available_reqs() const;

    void update_desired_request_count()
    {
        desired_request_count_ = max_available_reqs();
    }

    void maybe_send_block_requests();

    void check_request_timeout(time_t now);

    [[nodiscard]] constexpr auto client_reqq() const noexcept
    {
        return session->reqq();
    }

    // ---

    [[nodiscard]] std::optional<int64_t> pop_next_metadata_request()
    {
        auto& reqs = peer_requested_metadata_pieces_;

        if (std::empty(reqs))
        {
            return {};
        }

        auto next = reqs.front();
        reqs.pop();
        return next;
    }

    void maybe_send_metadata_requests(time_t now) const;
    [[nodiscard]] size_t add_next_metadata_piece();
    [[nodiscard]] size_t add_next_block(time_t now_sec, uint64_t now_msec);

    [[nodiscard]] size_t fill_output_buffer_impl(time_t now_sec, uint64_t now_msec);
    void fill_output_buffer(time_t now_sec, uint64_t now_msec)
    {
        while (fill_output_buffer_impl(now_sec, now_msec) != 0U)
        {
        }
    }

    // ---

    void send_ltep_handshake();
    void parse_ltep_handshake(MessageReader& payload);
    void parse_ut_metadata(MessageReader& payload_in);
    void parse_ut_pex(MessageReader& payload);
    void parse_ltep(MessageReader& payload);

    void send_ut_pex();

    int client_got_block(std::unique_ptr<Cache::BlockData> block_data, tr_block_index_t block);
    ReadResult read_piece_data(MessageReader& payload);
    ReadResult process_peer_message(uint8_t id, MessageReader& payload);

    // ---

    size_t protocol_send_keepalive() const; // NOLINT(modernize-use-nodiscard)

    template<typename... Args>
    size_t protocol_send_message(uint8_t type, Args const&... args) const;

    size_t protocol_send_reject(peer_request const& req) const // NOLINT(modernize-use-nodiscard)
    {
        TR_ASSERT(io_->supports_fext());
        return protocol_send_message(BtPeerMsgs::FextReject, req.index, req.offset, req.length);
    }

    size_t protocol_send_cancel(peer_request const& req) const // NOLINT(modernize-use-nodiscard)
    {
        return protocol_send_message(BtPeerMsgs::Cancel, req.index, req.offset, req.length);
    }

    size_t protocol_send_request(peer_request const& req) const // NOLINT(modernize-use-nodiscard)
    {
        TR_ASSERT(is_valid_request(req));
        return protocol_send_message(BtPeerMsgs::Request, req.index, req.offset, req.length);
    }

    size_t protocol_send_dht_port(tr_port const port) const // NOLINT(modernize-use-nodiscard)
    {
        return protocol_send_message(BtPeerMsgs::DhtPort, port.host());
    }

    size_t protocol_send_have(tr_piece_index_t const index) const // NOLINT(modernize-use-nodiscard)
    {
        static_assert(sizeof(tr_piece_index_t) == sizeof(uint32_t));
        return protocol_send_message(BtPeerMsgs::Have, index);
    }

    size_t protocol_send_choke(bool const choke) const // NOLINT(modernize-use-nodiscard)
    {
        return protocol_send_message(choke ? BtPeerMsgs::Choke : BtPeerMsgs::Unchoke);
    }

    void protocol_send_interest(bool const b) const
    {
        protocol_send_message(b ? BtPeerMsgs::Interested : BtPeerMsgs::NotInterested);
    }

    void protocol_send_bitfield();

    // ---

    void publish(tr_peer_event const& peer_event)
    {
        if (callback_ != nullptr)
        {
            (*callback_)(this, peer_event, callback_data_);
        }
    }

    // ---

    static void did_write(tr_peerIo* /*io*/, size_t bytes_written, bool was_piece_data, void* vmsgs);
    static ReadState can_read(tr_peerIo* io, void* vmsgs, size_t* piece);
    static void got_error(tr_peerIo* /*io*/, tr_error const& /*error*/, void* vmsgs);

    // ---

    bool peer_supports_pex_ = false;
    bool peer_supports_metadata_xfer_ = false;
    bool client_sent_ltep_handshake_ = false;

    size_t desired_request_count_ = 0;

    uint8_t ut_pex_id_ = 0;
    uint8_t ut_metadata_id_ = 0;

    tr_port dht_port_;

    tr_torrent& tor_;

    std::shared_ptr<tr_peerIo> const io_;

    std::deque<peer_request> peer_requested_;

    std::array<std::vector<tr_pex>, NUM_TR_AF_INET_TYPES> pex_;

    std::queue<int64_t> peer_requested_metadata_pieces_;

    time_t client_sent_at_ = 0;

    time_t choke_changed_at_ = 0;

    time_t request_timeout_base_ = {};

    tr_incoming incoming_ = {};

    // if the peer supports the Extension Protocol in BEP 10 and
    // supplied a reqq argument, it's stored here.
    std::optional<size_t> peer_reqq_;

    std::unique_ptr<libtransmission::Timer> pex_timer_;

    tr_bitfield have_;

    tr_peer_callback_bt const callback_;
    void* const callback_data_;

    // seconds between periodic send_ut_pex() calls
    static auto constexpr SendPexInterval = 90s;

    // how many seconds we expect the next piece block to arrive
    static auto constexpr RequestTimeoutSecs = time_t{ 90 };
};

// ---

[[nodiscard]] constexpr bool is_message_length_correct(tr_torrent const& tor, uint8_t id, uint32_t len)
{
    switch (id)
    {
    case BtPeerMsgs::Choke:
    case BtPeerMsgs::Unchoke:
    case BtPeerMsgs::Interested:
    case BtPeerMsgs::NotInterested:
    case BtPeerMsgs::FextHaveAll:
    case BtPeerMsgs::FextHaveNone:
        return len == 1U;

    case BtPeerMsgs::Have:
    case BtPeerMsgs::FextSuggest:
    case BtPeerMsgs::FextAllowedFast:
        return len == 5U;

    case BtPeerMsgs::Bitfield:
        return !tor.has_metainfo() || len == 1 + ((tor.piece_count() + 7U) / 8U);

    case BtPeerMsgs::Request:
    case BtPeerMsgs::Cancel:
    case BtPeerMsgs::FextReject:
        return len == 13U;

    case BtPeerMsgs::Piece:
        len -= sizeof(id) + sizeof(uint32_t /*piece*/) + sizeof(uint32_t /*offset*/);
        return len <= tr_block_info::BlockSize;

    case BtPeerMsgs::DhtPort:
        return len == 3U;

    case BtPeerMsgs::Ltep:
        return len >= 2U;

    default: // unrecognized message
        return false;
    }
}

namespace protocol_send_message_helpers
{
[[nodiscard]] constexpr auto get_param_length(uint8_t param) noexcept
{
    return sizeof(param);
}

[[nodiscard]] constexpr auto get_param_length(uint16_t param) noexcept
{
    return sizeof(param);
}

[[nodiscard]] constexpr auto get_param_length(uint32_t param) noexcept
{
    return sizeof(param);
}

template<typename T>
[[nodiscard]] TR_CONSTEXPR20 auto get_param_length(T const& param) noexcept
{
    return std::size(param);
}

// ---

void add_param(MessageWriter& buffer, uint8_t param) noexcept
{
    buffer.add_uint8(param);
}

void add_param(MessageWriter& buffer, uint16_t param) noexcept
{
    buffer.add_uint16(param);
}

void add_param(MessageWriter& buffer, uint32_t param) noexcept
{
    buffer.add_uint32(param);
}

template<typename T>
void add_param(MessageWriter& buffer, T const& param) noexcept
{
    buffer.add(param);
}

// ---

[[nodiscard]] std::string log_param(uint8_t param)
{
    return fmt::format(" {:d}", static_cast<int>(param));
}

[[nodiscard]] std::string log_param(uint16_t param)
{
    return fmt::format(" {:d}", static_cast<int>(param));
}

[[nodiscard]] std::string log_param(uint32_t param)
{
    return fmt::format(" {:d}", static_cast<int>(param));
}

template<typename T>
[[nodiscard]] std::string log_param(T const& /*unused*/)
{
    return " []";
}

template<typename... Args>
[[nodiscard]] std::string build_log_message(uint8_t type, Args const&... args)
{
    auto text = fmt::format("sending '{:s}'", BtPeerMsgs::debug_name(type));
    (text.append(log_param(args)), ...);
    return text;
}

template<typename... Args>
size_t build_peer_message(MessageWriter& out, uint8_t type, Args const&... args)
{
    auto msg_len = sizeof(type);
    ((msg_len += get_param_length(args)), ...);
    out.add_uint32(msg_len);
    out.add_uint8(type);
    (add_param(out, args), ...);

    return msg_len;
}
} // namespace protocol_send_message_helpers

template<typename... Args>
size_t tr_peerMsgsImpl::protocol_send_message(uint8_t type, Args const&... args) const
{
    using namespace protocol_send_message_helpers;

    logtrace(this, build_log_message(type, args...));

    auto out = MessageBuffer{};
    [[maybe_unused]] auto const msg_len = build_peer_message(out, type, args...);
    TR_ASSERT(is_message_length_correct(tor_, type, msg_len));
    auto const n_bytes_added = std::size(out);
    io_->write(out, type == BtPeerMsgs::Piece);
    return n_bytes_added;
}

void tr_peerMsgsImpl::protocol_send_bitfield()
{
    bool const fext = io_->supports_fext();

    if (fext && tor_.has_all())
    {
        protocol_send_message(BtPeerMsgs::FextHaveAll);
    }
    else if (fext && tor_.has_none())
    {
        protocol_send_message(BtPeerMsgs::FextHaveNone);
    }
    else if (!tor_.has_none())
    {
        // https://www.bittorrent.org/beps/bep_0003.html#peer-messages
        // Downloaders which don't have anything yet may skip the 'bitfield' message.
        protocol_send_message(BtPeerMsgs::Bitfield, tor_.create_piece_bitfield());
    }
}

size_t tr_peerMsgsImpl::protocol_send_keepalive() const
{
    logtrace(this, "sending 'keepalive'");

    auto out = MessageBuffer{};
    out.add_uint32(0);

    auto const n_bytes_added = std::size(out);
    io_->write(out, false);
    return n_bytes_added;
}

// ---

void tr_peerMsgsImpl::parse_ltep(MessageReader& payload)
{
    TR_ASSERT(!std::empty(payload));

    auto const ltep_msgid = payload.to_uint8();

    if (ltep_msgid == LtepMessages::Handshake)
    {
        parse_ltep_handshake(payload);

        // The peer most likely supports LTEP, so send our LTEP handshake in
        // case we haven't yet. Usually we would have sent our LTEP handshake
        // by this point, unless the peer didn't set the "extended" bit (20)
        // in the reserved bytes of the BT handshake.
        send_ltep_handshake();

        if (io_->supports_ltep())
        {
            send_ut_pex();
        }
    }
    else if (ltep_msgid == UT_PEX_ID)
    {
        peer_supports_pex_ = true;
        parse_ut_pex(payload);
    }
    else if (ltep_msgid == UT_METADATA_ID)
    {
        peer_supports_metadata_xfer_ = true;
        parse_ut_metadata(payload);
    }
    else
    {
        logtrace(this, fmt::format("skipping unknown ltep message ({:d})", static_cast<int>(ltep_msgid)));
    }
}

void tr_peerMsgsImpl::parse_ut_pex(MessageReader& payload)
{
    if (!tor_.allows_pex())
    {
        if (tor_.is_private())
        {
            logwarn(this, "got ut pex in private torrent, rejecting");
        }
        return;
    }

    if (auto var = tr_variant_serde::benc().inplace().parse(payload.to_string_view()); var)
    {
        logtrace(this, "got ut pex");

        uint8_t const* added = nullptr;
        auto added_len = size_t{};
        if (tr_variantDictFindRaw(&*var, TR_KEY_added, &added, &added_len))
        {
            uint8_t const* added_f = nullptr;
            auto added_f_len = size_t{};
            if (!tr_variantDictFindRaw(&*var, TR_KEY_added_f, &added_f, &added_f_len))
            {
                added_f_len = 0;
                added_f = nullptr;
            }

            auto pex = tr_pex::from_compact_ipv4(added, added_len, added_f, added_f_len);
            pex.resize(std::min(MaxPexPeerCount, std::size(pex)));
            tr_peerMgrAddPex(&tor_, TR_PEER_FROM_PEX, std::data(pex), std::size(pex));
        }

        if (tr_variantDictFindRaw(&*var, TR_KEY_added6, &added, &added_len))
        {
            uint8_t const* added_f = nullptr;
            auto added_f_len = size_t{};
            if (!tr_variantDictFindRaw(&*var, TR_KEY_added6_f, &added_f, &added_f_len))
            {
                added_f_len = 0;
                added_f = nullptr;
            }

            auto pex = tr_pex::from_compact_ipv6(added, added_len, added_f, added_f_len);
            pex.resize(std::min(MaxPexPeerCount, std::size(pex)));
            tr_peerMgrAddPex(&tor_, TR_PEER_FROM_PEX, std::data(pex), std::size(pex));
        }
    }
}

void tr_peerMsgsImpl::send_ut_pex()
{
    // only send pex if both the torrent and peer support it
    if (!peer_supports_pex_ || !tor_.allows_pex())
    {
        return;
    }

    static auto constexpr MaxPexAdded = size_t{ 50U };
    static auto constexpr MaxPexDropped = size_t{ 50U };

    auto map = tr_variant::Map{ 4U };
    auto tmpbuf = small::vector<std::byte, std::max(MaxPexAdded, MaxPexDropped) * tr_socket_address::CompactSockAddrMaxBytes>{};
    for (uint8_t i = 0; i < NUM_TR_AF_INET_TYPES; ++i)
    {
        static auto constexpr AddedMap = std::array{ TR_KEY_added, TR_KEY_added6 };
        static auto constexpr AddedFMap = std::array{ TR_KEY_added_f, TR_KEY_added6_f };
        static auto constexpr DroppedMap = std::array{ TR_KEY_dropped, TR_KEY_dropped6 };
        auto const ip_type = static_cast<tr_address_type>(i);

        auto& old_pex = pex_[i];
        auto new_pex = tr_peerMgrGetPeers(&tor_, ip_type, TR_PEERS_CONNECTED, MaxPexPeerCount);
        auto added = std::vector<tr_pex>{};
        added.reserve(std::size(new_pex));
        std::set_difference(
            std::begin(new_pex),
            std::end(new_pex),
            std::begin(old_pex),
            std::end(old_pex),
            std::back_inserter(added));
        auto dropped = std::vector<tr_pex>{};
        dropped.reserve(std::size(old_pex));
        std::set_difference(
            std::begin(old_pex),
            std::end(old_pex),
            std::begin(new_pex),
            std::end(new_pex),
            std::back_inserter(dropped));

        // Some peers give us error messages if we send
        // more than this many peers in a single pex message.
        // https://wiki.theory.org/BitTorrentPeerExchangeConventions
        added.resize(std::min(std::size(added), MaxPexAdded));
        dropped.resize(std::min(std::size(dropped), MaxPexDropped));

        logtrace(
            this,
            fmt::format(
                "pex: old {:s} peer count {:d}, new peer count {:d}, added {:d}, dropped {:d}",
                tr_ip_protocol_to_sv(ip_type),
                std::size(old_pex),
                std::size(new_pex),
                std::size(added),
                std::size(dropped)));

        // if there's nothing to send, then we're done
        if (std::empty(added) && std::empty(dropped))
        {
            continue;
        }

        // update msgs
        std::swap(old_pex, new_pex);

        // build the pex payload
        if (!std::empty(added))
        {
            // "added"
            tmpbuf.clear();
            tmpbuf.reserve(std::size(added) * tr_socket_address::CompactSockAddrBytes[i]);
            tr_pex::to_compact(std::back_inserter(tmpbuf), std::data(added), std::size(added));
            TR_ASSERT(std::size(tmpbuf) == std::size(added) * tr_socket_address::CompactSockAddrBytes[i]);
            map.try_emplace(AddedMap[i], std::string_view{ reinterpret_cast<char*>(std::data(tmpbuf)), std::size(tmpbuf) });

            // "added.f"
            tmpbuf.resize(std::size(added));
            auto* begin = std::data(tmpbuf);
            auto* walk = begin;
            for (auto const& p : added)
            {
                *walk++ = std::byte{ p.flags };
            }

            auto const f_len = static_cast<size_t>(walk - begin);
            TR_ASSERT(f_len == std::size(added));
            map.try_emplace(AddedFMap[i], std::string_view{ reinterpret_cast<char*>(begin), f_len });
        }

        if (!std::empty(dropped))
        {
            // "dropped"
            tmpbuf.clear();
            tmpbuf.reserve(std::size(dropped) * tr_socket_address::CompactSockAddrBytes[i]);
            tr_pex::to_compact(std::back_inserter(tmpbuf), std::data(dropped), std::size(dropped));
            TR_ASSERT(std::size(tmpbuf) == std::size(dropped) * tr_socket_address::CompactSockAddrBytes[i]);
            map.try_emplace(DroppedMap[i], std::string_view{ reinterpret_cast<char*>(std::data(tmpbuf)), std::size(tmpbuf) });
        }
    }

    protocol_send_message(BtPeerMsgs::Ltep, ut_pex_id_, tr_variant_serde::benc().to_string(tr_variant{ std::move(map) }));
}

void tr_peerMsgsImpl::send_ltep_handshake()
{
    if (client_sent_ltep_handshake_)
    {
        return;
    }

    logtrace(this, "sending an ltep handshake");
    client_sent_ltep_handshake_ = true;

    /* decide if we want to advertise metadata xfer support (BEP 9) */
    bool const allow_metadata_xfer = tor_.is_public();

    /* decide if we want to advertise pex support */
    bool const allow_pex = tor_.allows_pex();

    auto val = tr_variant{};
    tr_variantInitDict(&val, 8);
    tr_variantDictAddBool(&val, TR_KEY_e, session->encryptionMode() != TR_CLEAR_PREFERRED);

    // If connecting to global peer, then use global address
    // Otherwise we are connecting to local peer, use bind address directly
    if (auto const addr = io_->address().is_global_unicast_address() ? session->global_address(TR_AF_INET) :
                                                                       session->bind_address(TR_AF_INET);
        addr && !addr->is_any())
    {
        TR_ASSERT(addr->is_ipv4());
        tr_variantDictAddRaw(&val, TR_KEY_ipv4, &addr->addr.addr4, sizeof(addr->addr.addr4));
    }
    if (auto const addr = io_->address().is_global_unicast_address() ? session->global_address(TR_AF_INET6) :
                                                                       session->bind_address(TR_AF_INET6);
        addr && !addr->is_any())
    {
        TR_ASSERT(addr->is_ipv6());
        tr_variantDictAddRaw(&val, TR_KEY_ipv6, &addr->addr.addr6, sizeof(addr->addr.addr6));
    }

    // https://www.bittorrent.org/beps/bep_0009.html
    // It also adds "metadata_size" to the handshake message (not the
    // "m" dictionary) specifying an integer value of the number of
    // bytes of the metadata.
    if (auto const info_dict_size = tor_.info_dict_size(); allow_metadata_xfer && tor_.has_metainfo() && info_dict_size > 0)
    {
        tr_variantDictAddInt(&val, TR_KEY_metadata_size, info_dict_size);
    }

    // https://www.bittorrent.org/beps/bep_0010.html
    // Local TCP listen port. Allows each side to learn about the TCP
    // port number of the other side. Note that there is no need for the
    // receiving side of the connection to send this extension message,
    // since its port number is already known.
    tr_variantDictAddInt(&val, TR_KEY_p, session->advertisedPeerPort().host());

    // https://www.bittorrent.org/beps/bep_0010.html
    // An integer, the number of outstanding request messages this
    // client supports without dropping any.
    tr_variantDictAddInt(&val, TR_KEY_reqq, client_reqq());

    // https://www.bittorrent.org/beps/bep_0010.html
    // A string containing the compact representation of the ip address this peer sees
    // you as. i.e. this is the receiver's external ip address (no port is included).
    // This may be either an IPv4 (4 bytes) or an IPv6 (16 bytes) address.
    {
        auto buf = std::array<std::byte, TR_ADDRSTRLEN>{};
        auto const begin = std::data(buf);
        auto const end = io_->address().to_compact(begin);
        auto const len = end - begin;
        TR_ASSERT(len == tr_address::CompactAddrBytes[0] || len == tr_address::CompactAddrBytes[1]);
        tr_variantDictAddRaw(&val, TR_KEY_yourip, begin, len);
    }

    // https://www.bittorrent.org/beps/bep_0010.html
    // Client name and version (as a utf-8 string). This is a much more
    // reliable way of identifying the client than relying on the
    // peer id encoding.
    tr_variantDictAddStrView(&val, TR_KEY_v, TR_NAME " " USERAGENT_PREFIX);

    // https://www.bittorrent.org/beps/bep_0021.html
    // A peer that is a partial seed SHOULD include an extra header in
    // the extension handshake 'upload_only'. Setting the value of this
    // key to 1 indicates that this peer is not interested in downloading
    // anything.
    tr_variantDictAddBool(&val, TR_KEY_upload_only, tor_.is_done());

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

    protocol_send_message(BtPeerMsgs::Ltep, LtepMessages::Handshake, tr_variant_serde::benc().to_string(val));
}

void tr_peerMsgsImpl::parse_ltep_handshake(MessageReader& payload)
{
    auto const handshake_sv = payload.to_string_view();

    auto var = tr_variant_serde::benc().inplace().parse(handshake_sv);
    if (!var || !var->holds_alternative<tr_variant::Map>())
    {
        logtrace(this, "got ltep handshake, couldn't get dictionary");
        return;
    }

    logtrace(this, fmt::format("got ltep handshake, base64-encoded body: [{:s}]", tr_base64_encode(handshake_sv)));

    if (!io_->supports_ltep())
    {
        logwarn(this, "got ltep handshake, but peer did not advertise support in reserved bytes");
    }

    // does the peer prefer encrypted connections?
    if (auto e = int64_t{}; tr_variantDictFindInt(&*var, TR_KEY_e, &e))
    {
        peer_info->set_encryption_preferred(e != 0);
    }

    // check supported messages for utorrent pex
    peer_supports_pex_ = false;
    peer_supports_metadata_xfer_ = false;
    auto holepunch_supported = false;

    if (tr_variant* sub = nullptr; tr_variantDictFindDict(&*var, TR_KEY_m, &sub))
    {
        if (auto ut_pex = int64_t{}; tr_variantDictFindInt(sub, TR_KEY_ut_pex, &ut_pex))
        {
            peer_supports_pex_ = ut_pex != 0;
            ut_pex_id_ = static_cast<uint8_t>(ut_pex);
            logtrace(this, fmt::format("msgs->ut_pex is {:d}", ut_pex_id_));
        }

        if (auto ut_metadata = int64_t{}; tr_variantDictFindInt(sub, TR_KEY_ut_metadata, &ut_metadata))
        {
            peer_supports_metadata_xfer_ = ut_metadata != 0;
            ut_metadata_id_ = static_cast<uint8_t>(ut_metadata);
            logtrace(this, fmt::format("msgs->ut_metadata_id_ is {:d}", ut_metadata_id_));
        }

        if (auto ut_holepunch = int64_t{}; tr_variantDictFindInt(sub, TR_KEY_ut_holepunch, &ut_holepunch))
        {
            holepunch_supported = ut_holepunch != 0;
        }
    }

    // Transmission doesn't support this extension yet.
    // But its presence does indicate µTP support,
    // which we do care about...
    if (holepunch_supported)
    {
        peer_info->set_utp_supported();
    }
    // Even though we don't support it, no reason not to
    // help pass this flag to other peers who do.
    peer_info->set_holepunch_supported(holepunch_supported);

    // look for metainfo size (BEP 9)
    if (auto metadata_size = int64_t{};
        peer_supports_metadata_xfer_ && tr_variantDictFindInt(&*var, TR_KEY_metadata_size, &metadata_size))
    {
        if (!tr_metadata_download::is_valid_metadata_size(metadata_size))
        {
            peer_supports_metadata_xfer_ = false;
        }
        else
        {
            tor_.maybe_start_metadata_transfer(metadata_size);
        }
    }

    // look for upload_only (BEP 21)
    if (auto upload_only = int64_t{}; tr_variantDictFindInt(&*var, TR_KEY_upload_only, &upload_only))
    {
        peer_info->set_upload_only(upload_only != 0);
    }

    // https://www.bittorrent.org/beps/bep_0010.html
    // Client name and version (as a utf-8 string). This is a much more
    // reliable way of identifying the client than relying on the
    // peer id encoding.
    if (auto sv = std::string_view{}; tr_variantDictFindStrView(&*var, TR_KEY_v, &sv))
    {
        set_user_agent(tr_interned_string{ sv });
    }

    /* get peer's listening port */
    if (auto p = int64_t{}; tr_variantDictFindInt(&*var, TR_KEY_p, &p) && p > 0)
    {
        publish(tr_peer_event::GotPort(tr_port::from_host(p)));
        logtrace(this, fmt::format("peer's port is now {:d}", p));
    }

    std::byte const* addr_compact = nullptr;
    auto addr_len = size_t{};
    if (io_->is_incoming() && tr_variantDictFindRaw(&*var, TR_KEY_ipv4, &addr_compact, &addr_len) &&
        addr_len == tr_address::CompactAddrBytes[TR_AF_INET])
    {
        auto pex = tr_pex{ peer_info->listen_socket_address(), peer_info->pex_flags() };
        pex.socket_address.address_ = tr_address::from_compact_ipv4(addr_compact).first;
        tr_peerMgrAddPex(&tor_, TR_PEER_FROM_LTEP, &pex, 1);
    }

    if (io_->is_incoming() && tr_variantDictFindRaw(&*var, TR_KEY_ipv6, &addr_compact, &addr_len) &&
        addr_len == tr_address::CompactAddrBytes[TR_AF_INET6])
    {
        auto pex = tr_pex{ peer_info->listen_socket_address(), peer_info->pex_flags() };
        pex.socket_address.address_ = tr_address::from_compact_ipv6(addr_compact).first;
        tr_peerMgrAddPex(&tor_, TR_PEER_FROM_LTEP, &pex, 1);
    }

    /* get peer's maximum request queue size */
    if (auto reqq_in = int64_t{}; tr_variantDictFindInt(&*var, TR_KEY_reqq, &reqq_in) && reqq_in > 0)
    {
        peer_reqq_ = reqq_in;
    }
}

void tr_peerMsgsImpl::parse_ut_metadata(MessageReader& payload_in)
{
    int64_t msg_type = -1;
    int64_t piece = -1;
    int64_t total_size = 0;

    auto const tmp = payload_in.to_string_view();
    auto const* const msg_end = std::data(tmp) + std::size(tmp);

    auto serde = tr_variant_serde::benc();
    if (auto var = serde.inplace().parse(tmp); var)
    {
        (void)tr_variantDictFindInt(&*var, TR_KEY_msg_type, &msg_type);
        (void)tr_variantDictFindInt(&*var, TR_KEY_piece, &piece);
        (void)tr_variantDictFindInt(&*var, TR_KEY_total_size, &total_size);
    }

    logtrace(this, fmt::format("got ut_metadata msg: type {:d}, piece {:d}, total_size {:d}", msg_type, piece, total_size));
    if (tor_.is_private())
    {
        logwarn(this, "got ut metadata in private torrent, rejecting");
    }

    if (msg_type == MetadataMsgType::Reject)
    {
        // no-op
    }

    if (auto const piece_len = msg_end - serde.end();
        msg_type == MetadataMsgType::Data && piece * MetadataPieceSize + piece_len <= total_size)
    {
        tor_.set_metadata_piece(piece, serde.end(), piece_len);
    }

    if (msg_type == MetadataMsgType::Request)
    {
        if (piece >= 0 && tor_.has_metainfo() && tor_.is_public() && std::size(peer_requested_metadata_pieces_) < MetadataReqQ)
        {
            peer_requested_metadata_pieces_.push(piece);
        }
        else
        {
            /* send a rejection message */
            auto v = tr_variant{};
            tr_variantInitDict(&v, 2);
            tr_variantDictAddInt(&v, TR_KEY_msg_type, MetadataMsgType::Reject);
            tr_variantDictAddInt(&v, TR_KEY_piece, piece);
            protocol_send_message(BtPeerMsgs::Ltep, ut_metadata_id_, serde.to_string(v));
        }
    }
}

// ---

ReadResult tr_peerMsgsImpl::process_peer_message(uint8_t id, MessageReader& payload)
{
    bool const fext = io_->supports_fext();

    auto ui32 = uint32_t{};

    logtrace(
        this,
        fmt::format(
            "got peer msg '{:s}' ({:d}) with payload len {:d}",
            BtPeerMsgs::debug_name(id),
            static_cast<int>(id),
            std::size(payload)));

    if (!is_message_length_correct(tor_, id, sizeof(id) + std::size(payload)))
    {
        logdbg(
            this,
            fmt::format(
                "bad msg: '{:s}' ({:d}) with payload len {:d}",
                BtPeerMsgs::debug_name(id),
                static_cast<int>(id),
                std::size(payload)));
        publish(tr_peer_event::GotError(EMSGSIZE));
        return { ReadState::Err, {} };
    }

    switch (id)
    {
    case BtPeerMsgs::Choke:
        logtrace(this, "got Choke");
        set_client_choked(true);

        if (!fext)
        {
            publish(tr_peer_event::GotChoke());
            active_requests.set_has_none();
        }

        update_active(TR_PEER_TO_CLIENT);
        break;

    case BtPeerMsgs::Unchoke:
        logtrace(this, "got Unchoke");
        set_client_choked(false);
        update_active(TR_PEER_TO_CLIENT);
        update_desired_request_count();
        break;

    case BtPeerMsgs::Interested:
        logtrace(this, "got Interested");
        set_peer_interested(true);
        update_active(TR_CLIENT_TO_PEER);
        break;

    case BtPeerMsgs::NotInterested:
        logtrace(this, "got Not Interested");
        set_peer_interested(false);
        update_active(TR_CLIENT_TO_PEER);
        break;

    case BtPeerMsgs::Have:
        ui32 = payload.to_uint32();
        logtrace(this, fmt::format("got Have: {:d}", ui32));

        if (tor_.has_metainfo() && ui32 >= tor_.piece_count())
        {
            publish(tr_peer_event::GotError(ERANGE));
            return { ReadState::Err, {} };
        }

        /* a peer can send the same HAVE message twice... */
        if (!have_.test(ui32))
        {
            have_.set(ui32);
            peer_info->set_seed(is_seed());
            publish(tr_peer_event::GotHave(ui32));
        }

        break;

    case BtPeerMsgs::Bitfield:
        logtrace(this, "got a bitfield");
        have_ = tr_bitfield{ tor_.has_metainfo() ? tor_.piece_count() : std::size(payload) * 8 };
        have_.set_raw(reinterpret_cast<uint8_t const*>(std::data(payload)), std::size(payload));
        peer_info->set_seed(is_seed());
        publish(tr_peer_event::GotBitfield(&have_));
        break;

    case BtPeerMsgs::Request:
        {
            struct peer_request r;
            r.index = payload.to_uint32();
            r.offset = payload.to_uint32();
            r.length = payload.to_uint32();
            logtrace(this, fmt::format("got Request: {:d}:{:d}->{:d}", r.index, r.offset, r.length));
            on_peer_made_request(r);
            break;
        }

    case BtPeerMsgs::Cancel:
        {
            struct peer_request r;
            r.index = payload.to_uint32();
            r.offset = payload.to_uint32();
            r.length = payload.to_uint32();
            cancels_sent_to_client.add(tr_time(), 1);
            logtrace(this, fmt::format("got a Cancel {:d}:{:d}->{:d}", r.index, r.offset, r.length));

            auto& requests = peer_requested_;
            if (auto iter = std::find(std::begin(requests), std::end(requests), r); iter != std::end(requests))
            {
                requests.erase(iter);

                // bep6: "Even when a request is cancelled, the peer
                // receiving the cancel should respond with either the
                // corresponding reject or the corresponding piece"
                if (fext)
                {
                    protocol_send_reject(r);
                }
            }
            break;
        }

    case BtPeerMsgs::Piece:
        return read_piece_data(payload);

    case BtPeerMsgs::DhtPort:
        // https://www.bittorrent.org/beps/bep_0005.html
        // Peers supporting the DHT set the last bit of the 8-byte reserved flags
        // exchanged in the BitTorrent protocol handshake. Peer receiving a handshake
        // indicating the remote peer supports the DHT should send a PORT message.
        // It begins with byte 0x09 and has a two byte payload containing the UDP
        // port of the DHT node in network byte order.
        {
            logtrace(this, "Got a BtPeerMsgs::DhtPort");

            auto const hport = payload.to_uint16();
            if (auto const dht_port = tr_port::from_host(hport); !std::empty(dht_port))
            {
                dht_port_ = dht_port;
                session->maybe_add_dht_node(io_->address(), dht_port_);
            }
        }
        break;

    case BtPeerMsgs::FextSuggest:
        logtrace(this, "Got a BtPeerMsgs::FextSuggest");

        if (fext)
        {
            auto const piece = payload.to_uint32();
            publish(tr_peer_event::GotSuggest(piece));
        }
        else
        {
            publish(tr_peer_event::GotError(EMSGSIZE));
            return { ReadState::Err, {} };
        }

        break;

    case BtPeerMsgs::FextAllowedFast:
        logtrace(this, "Got a BtPeerMsgs::FextAllowedFast");

        if (fext)
        {
            auto const piece = payload.to_uint32();
            publish(tr_peer_event::GotAllowedFast(piece));
        }
        else
        {
            publish(tr_peer_event::GotError(EMSGSIZE));
            return { ReadState::Err, {} };
        }

        break;

    case BtPeerMsgs::FextHaveAll:
        logtrace(this, "Got a BtPeerMsgs::FextHaveAll");

        if (fext)
        {
            have_.set_has_all();
            peer_info->set_seed();
            publish(tr_peer_event::GotHaveAll());
        }
        else
        {
            publish(tr_peer_event::GotError(EMSGSIZE));
            return { ReadState::Err, {} };
        }

        break;

    case BtPeerMsgs::FextHaveNone:
        logtrace(this, "Got a BtPeerMsgs::FextHaveNone");

        if (fext)
        {
            have_.set_has_none();
            peer_info->set_seed(false);
            publish(tr_peer_event::GotHaveNone());
        }
        else
        {
            publish(tr_peer_event::GotError(EMSGSIZE));
            return { ReadState::Err, {} };
        }

        break;

    case BtPeerMsgs::FextReject:
        {
            struct peer_request r;
            r.index = payload.to_uint32();
            r.offset = payload.to_uint32();
            r.length = payload.to_uint32();

            if (fext)
            {
                if (auto const block = tor_.piece_loc(r.index, r.offset).block; active_requests.test(block))
                {
                    active_requests.unset(block);
                    publish(tr_peer_event::GotRejected(tor_.block_info(), block));
                }
            }
            else
            {
                publish(tr_peer_event::GotError(EMSGSIZE));
                return { ReadState::Err, {} };
            }

            break;
        }

    case BtPeerMsgs::Ltep:
        logtrace(this, "Got a BtPeerMsgs::Ltep");
        parse_ltep(payload);
        break;

    default:
        logtrace(this, fmt::format("peer sent us an UNKNOWN: {:d}", static_cast<int>(id)));
        break;
    }

    return { ReadState::Now, {} };
}

ReadResult tr_peerMsgsImpl::read_piece_data(MessageReader& payload)
{
    // <index><begin><block>
    auto const piece = payload.to_uint32();
    auto const offset = payload.to_uint32();
    auto const len = std::size(payload);

    auto const loc = tor_.piece_loc(piece, offset);
    auto const block = loc.block;
    auto const block_size = tor_.block_size(block);

    logtrace(this, fmt::format("got {:d} bytes for req {:d}:{:d}->{:d}", len, piece, offset, len));

    if (loc.block_offset + len > block_size)
    {
        logwarn(this, fmt::format("got unaligned block {:d} ({:d}:{:d}->{:d})", block, piece, offset, len));
        return { ReadState::Err, len };
    }

    if (!active_requests.test(block))
    {
        logwarn(this, fmt::format("got unrequested block {:d} ({:d}:{:d}->{:d})", block, piece, offset, len));
        return { ReadState::Err, len };
    }

    if (tor_.has_block(block))
    {
        logtrace(this, fmt::format("got completed block {:d} ({:d}:{:d}->{:d})", block, piece, offset, len));
        return { ReadState::Err, len };
    }

    peer_info->set_latest_piece_data_time(tr_time());
    publish(tr_peer_event::GotPieceData(len));

    if (loc.block_offset == 0U && len == block_size) // simple case: one message has entire block
    {
        auto buf = std::make_unique<Cache::BlockData>(block_size);
        payload.to_buf(std::data(*buf), len);
        auto const ok = client_got_block(std::move(buf), block) == 0;
        return { ok ? ReadState::Now : ReadState::Err, len };
    }

    auto& blocks = incoming_.blocks;
    auto& incoming_block = blocks.try_emplace(block, block_size).first->second;
    payload.to_buf(std::data(*incoming_block.buf) + loc.block_offset, len);

    if (!incoming_block.add_span(loc.block_offset, loc.block_offset + len))
    {
        return { ReadState::Err, len }; // invalid span
    }

    if (!incoming_block.has_all())
    {
        return { ReadState::Later, len }; // we don't have the full block yet
    }

    auto block_buf = std::move(incoming_block.buf);
    blocks.erase(block); // note: invalidates `incoming_block` local
    auto const ok = client_got_block(std::move(block_buf), block) == 0;
    return { ok ? ReadState::Now : ReadState::Err, len };
}

// returns 0 on success, or an errno on failure
int tr_peerMsgsImpl::client_got_block(std::unique_ptr<Cache::BlockData> block_data, tr_block_index_t const block)
{
    if (auto const n_bytes = block_data ? std::size(*block_data) : 0U; n_bytes != tor_.block_size(block))
    {
        auto const n_expected = tor_.block_size(block);
        logdbg(this, fmt::format("wrong block size: expected {:d}, got {:d}", n_expected, n_bytes));
        return EMSGSIZE;
    }

    logtrace(this, fmt::format("got block {:d}", block));

    // NB: if writeBlock() fails the torrent may be paused.
    // If this happens, this object will be destructed and must no longer be used.
    if (auto const err = session->cache->write_block(tor_.id(), block, std::move(block_data)); err != 0)
    {
        return err;
    }

    active_requests.unset(block);
    request_timeout_base_ = tr_time();
    publish(tr_peer_event::GotBlock(tor_.block_info(), block));

    return 0;
}

// ---

void tr_peerMsgsImpl::did_write(tr_peerIo* /*io*/, size_t bytes_written, bool was_piece_data, void* vmsgs)
{
    auto* const msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);

    if (was_piece_data)
    {
        msgs->peer_info->set_latest_piece_data_time(tr_time());
        msgs->publish(tr_peer_event::SentPieceData(bytes_written));
    }
}

ReadState tr_peerMsgsImpl::can_read(tr_peerIo* io, void* vmsgs, size_t* piece)
{
    auto* const msgs = static_cast<tr_peerMsgsImpl*>(vmsgs);

    // https://www.bittorrent.org/beps/bep_0003.html
    // Next comes an alternating stream of length prefixes and messages.
    // Messages of length zero are keepalives, and ignored.
    // All non-keepalive messages start with a single byte which gives their type.
    //
    // https://wiki.theory.org/BitTorrentSpecification
    // All of the remaining messages in the protocol take the form of
    // <length prefix><message ID><payload>. The length prefix is a four byte
    // big-endian value. The message ID is a single decimal byte.
    // The payload is message dependent.

    // read <length prefix>
    auto& current_message_len = msgs->incoming_.length; // the full message payload length. Includes the +1 for id length
    if (!current_message_len)
    {
        auto message_len = uint32_t{};
        if (io->read_buffer_size() < sizeof(message_len))
        {
            return ReadState::Later;
        }

        io->read_uint32(&message_len);

        // The keep-alive message is a message with zero bytes,
        // specified with the length prefix set to zero.
        // There is no message ID and no payload.
        if (message_len == 0U)
        {
            logtrace(msgs, "got KeepAlive");
            return ReadState::Now;
        }

        current_message_len = message_len;
    }

    // read <message ID>
    auto& current_message_type = msgs->incoming_.id;
    if (!current_message_type)
    {
        auto message_type = uint8_t{};
        if (io->read_buffer_size() < sizeof(message_type))
        {
            return ReadState::Later;
        }

        io->read_uint8(&message_type);
        current_message_type = message_type;
    }

    // read <payload>
    auto& current_payload = msgs->incoming_.payload;
    auto const full_payload_len = *current_message_len - sizeof(*current_message_type);
    auto n_left = full_payload_len - std::size(current_payload);
    auto const [buf, n_this_pass] = current_payload.reserve_space(std::min(n_left, io->read_buffer_size()));
    io->read_bytes(buf, n_this_pass);
    current_payload.commit_space(n_this_pass);
    n_left -= n_this_pass;
    logtrace(msgs, fmt::format("read {:d} payload bytes; {:d} left to go", n_this_pass, n_left));

    if (n_left > 0U)
    {
        return ReadState::Later;
    }

    // The incoming message is now complete. After processing the message
    // with `process_peer_message()`, reset the peerMsgs' incoming
    // field so it's ready to receive the next message.

    auto const [read_state, n_piece_bytes_read] = msgs->process_peer_message(*current_message_type, current_payload);
    *piece = n_piece_bytes_read;

    current_message_len.reset();
    current_message_type.reset();
    current_payload.clear();

    return read_state;
}

void tr_peerMsgsImpl::got_error(tr_peerIo* /*io*/, tr_error const& /*error*/, void* vmsgs)
{
    static_cast<tr_peerMsgsImpl*>(vmsgs)->publish(tr_peer_event::GotError(ENOTCONN));
}

// ---

void tr_peerMsgsImpl::pulse()
{
    auto const now_sec = tr_time();
    auto const now_msec = tr_time_msec();

    check_request_timeout(now_sec);
    update_desired_request_count();
    maybe_send_block_requests();
    maybe_send_metadata_requests(now_sec);
    fill_output_buffer(now_sec, now_msec);
}

void tr_peerMsgsImpl::maybe_send_metadata_requests(time_t now) const
{
    if (!peer_supports_metadata_xfer_)
    {
        return;
    }

    if (auto const piece = tor_.get_next_metadata_request(now); piece)
    {
        auto tmp = tr_variant{};
        tr_variantInitDict(&tmp, 3);
        tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Request);
        tr_variantDictAddInt(&tmp, TR_KEY_piece, *piece);
        protocol_send_message(BtPeerMsgs::Ltep, ut_metadata_id_, tr_variant_serde::benc().to_string(tmp));
    }
}

void tr_peerMsgsImpl::maybe_send_block_requests()
{
    if (!tor_.client_can_download())
    {
        return;
    }

    auto const n_active = active_req_count(TR_CLIENT_TO_PEER);
    if (n_active >= desired_request_count_)
    {
        return;
    }

    TR_ASSERT(client_is_interested());
    TR_ASSERT(!client_is_choked());

    auto const n_wanted = desired_request_count_ - n_active;
    if (auto const requests = tr_peerMgrGetNextRequests(&tor_, this, n_wanted); !std::empty(requests))
    {
        request_blocks(std::data(requests), std::size(requests));
    }
}

void tr_peerMsgsImpl::check_request_timeout(time_t now)
{
    if (active_requests.has_none() || now - request_timeout_base_ <= RequestTimeoutSecs)
    {
        return;
    }

    // If we didn't receive any piece data from this peer for a while,
    // cancel all active requests so that we will send a new batch.
    // If the peer still doesn't send anything to us, then it will
    // naturally get weeded out by the peer mgr.
    for (size_t block = 0; block < std::size(active_requests); ++block)
    {
        maybe_cancel_block_request(block);
    }
}

[[nodiscard]] size_t tr_peerMsgsImpl::fill_output_buffer_impl(time_t now_sec, uint64_t now_msec)
{
    auto n_bytes_written = size_t{};

    // fulfill metadata requests
    for (;;)
    {
        auto const old_len = n_bytes_written;
        n_bytes_written += add_next_metadata_piece();
        if (old_len == n_bytes_written)
        {
            break;
        }
    }

    // fulfill piece requests
    for (;;)
    {
        auto const old_len = n_bytes_written;
        n_bytes_written += add_next_block(now_sec, now_msec);
        if (old_len == n_bytes_written)
        {
            break;
        }
    }

    if (client_sent_at_ != 0 && now_sec - client_sent_at_ > KeepaliveIntervalSecs)
    {
        n_bytes_written += protocol_send_keepalive();
    }

    return n_bytes_written;
}

[[nodiscard]] size_t tr_peerMsgsImpl::add_next_metadata_piece()
{
    auto const piece = pop_next_metadata_request();

    if (!piece.has_value()) // no pending requests
    {
        return {};
    }

    auto data = tor_.get_metadata_piece(*piece);
    if (!data)
    {
        // send a reject
        auto tmp = tr_variant{};
        tr_variantInitDict(&tmp, 2);
        tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Reject);
        tr_variantDictAddInt(&tmp, TR_KEY_piece, *piece);
        return protocol_send_message(BtPeerMsgs::Ltep, ut_metadata_id_, tr_variant_serde::benc().to_string(tmp));
    }

    // send the metadata
    auto tmp = tr_variant{};
    tr_variantInitDict(&tmp, 3);
    tr_variantDictAddInt(&tmp, TR_KEY_msg_type, MetadataMsgType::Data);
    tr_variantDictAddInt(&tmp, TR_KEY_piece, *piece);
    tr_variantDictAddInt(&tmp, TR_KEY_total_size, tor_.info_dict_size());
    return protocol_send_message(BtPeerMsgs::Ltep, ut_metadata_id_, tr_variant_serde::benc().to_string(tmp), *data);
}

[[nodiscard]] size_t tr_peerMsgsImpl::add_next_block(time_t now_sec, uint64_t now_msec)
{
    if (std::empty(peer_requested_) || io_->get_write_buffer_space(now_msec) == 0U)
    {
        return {};
    }

    auto const req = peer_requested_.front();
    peer_requested_.pop_front();

    auto buf = std::array<uint8_t, tr_block_info::BlockSize>{};
    auto ok = is_valid_request(req) && tor_.has_piece(req.index);

    if (ok)
    {
        ok = tor_.ensure_piece_is_checked(req.index);

        if (!ok)
        {
            tor_.error().set_local_error(fmt::format("Please Verify Local Data! Piece #{:d} is corrupt.", req.index));
        }
    }

    if (ok)
    {
        ok = session->cache->read_block(tor_, tor_.piece_loc(req.index, req.offset), req.length, std::data(buf)) == 0;
    }

    if (ok)
    {
        blocks_sent_to_peer.add(now_sec, 1);
        auto const piece_data = std::string_view{ reinterpret_cast<char const*>(std::data(buf)), req.length };
        return protocol_send_message(BtPeerMsgs::Piece, req.index, req.offset, piece_data);
    }

    if (io_->supports_fext())
    {
        return protocol_send_reject(req);
    }

    return {};
}

// ---

bool tr_peerMsgsImpl::is_valid_request(peer_request const& req) const
{
    int err = 0;

    if (req.index >= tor_.piece_count())
    {
        err = 1;
    }
    else if (req.length < 1)
    {
        err = 2;
    }
    else if (req.offset + req.length > tor_.piece_size(req.index))
    {
        err = 3;
    }
    else if (req.length > tr_block_info::BlockSize)
    {
        err = 4;
    }
    else if (tor_.piece_loc(req.index, req.offset, req.length).byte > tor_.total_size())
    {
        err = 5;
    }

    if (err != 0)
    {
        tr_logAddTraceTor(&tor_, fmt::format("index {} offset {} length {} err {}", req.index, req.offset, req.length, err));
    }

    return err == 0;
}

[[nodiscard]] bool tr_peerMsgsImpl::can_add_request_from_peer(peer_request const& req)
{
    if (peer_is_choked())
    {
        logtrace(this, "rejecting request from choked peer");
        return false;
    }

    if (std::size(peer_requested_) >= client_reqq())
    {
        logtrace(this, "rejecting request ... reqq is full");
        return false;
    }

    if (!is_valid_request(req))
    {
        logtrace(this, "rejecting an invalid request.");
        return false;
    }

    if (!tor_.has_piece(req.index))
    {
        logtrace(this, "rejecting request for a piece we don't have.");
        return false;
    }

    return true;
}

size_t tr_peerMsgsImpl::max_available_reqs() const
{
    if (tor_.is_done() || !tor_.has_metainfo() || client_is_choked() || !client_is_interested())
    {
        return 0;
    }

    // Get the rate limit we should use.
    // TODO: this needs to consider all the other peers as well...
    uint64_t const now = tr_time_msec();
    auto rate = get_piece_speed(now, TR_PEER_TO_CLIENT);
    if (tor_.uses_speed_limit(TR_PEER_TO_CLIENT))
    {
        rate = std::min(rate, tor_.speed_limit(TR_PEER_TO_CLIENT));
    }

    // honor the session limits, if enabled
    if (tor_.uses_session_limits())
    {
        if (auto const limit = session->active_speed_limit(TR_PEER_TO_CLIENT))
        {
            rate = std::min(rate, *limit);
        }
    }

    // use this desired rate to figure out how
    // many requests we should send to this peer
    static auto constexpr Floor = size_t{ 32 };
    static size_t constexpr Seconds = RequestBufSecs;
    size_t const estimated_blocks_in_period = (rate.base_quantity() * Seconds) / tr_block_info::BlockSize;
    auto const ceil = peer_reqq_.value_or(PeerReqQDefault);

    return std::clamp(estimated_blocks_in_period, Floor, ceil);
}

} // namespace

tr_peerMsgs::tr_peerMsgs(
    tr_torrent const& tor,
    std::shared_ptr<tr_peer_info> peer_info_in,
    tr_interned_string user_agent,
    bool connection_is_encrypted,
    bool connection_is_incoming,
    bool connection_is_utp)
    : tr_peer{ tor }
    , peer_info{ std::move(peer_info_in) }
    , user_agent_{ user_agent }
    , connection_is_encrypted_{ connection_is_encrypted }
    , connection_is_incoming_{ connection_is_incoming }
    , connection_is_utp_{ connection_is_utp }
{
    peer_info->set_connected(tr_time());
    ++n_peers;
}

tr_peerMsgs::~tr_peerMsgs()
{
    peer_info->set_connected(tr_time(), false, is_disconnecting());
    TR_ASSERT(n_peers > 0U);
    --n_peers;
}

tr_peerMsgs* tr_peerMsgs::create(
    tr_torrent& torrent,
    std::shared_ptr<tr_peer_info> peer_info,
    std::shared_ptr<tr_peerIo> io,
    tr_interned_string user_agent,
    tr_peer_callback_bt callback,
    void* callback_data)
{
    return new tr_peerMsgsImpl{ torrent, std::move(peer_info), std::move(io), user_agent, callback, callback_data };
}
