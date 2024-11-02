// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno>
#include <cstdint>
#include <functional>
#include <type_traits> // std::underlying_type_t

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h> // ntohl, ntohs
#endif

#include <event2/event.h>

#include <libutp/utp.h>

#include <fmt/core.h>

#include <small/map.hpp>

#include "libtransmission/transmission.h"

#include "libtransmission/bandwidth.h"
#include "libtransmission/block-info.h" // tr_block_info
#include "libtransmission/error.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/peer-io.h"
#include "libtransmission/peer-socket.h" // tr_peer_socket, tr_netOpen...
#include "libtransmission/session.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h" // for _()

struct sockaddr;

#define tr_logAddErrorIo(io, msg) tr_logAddError(msg, (io)->display_name())
#define tr_logAddWarnIo(io, msg) tr_logAddWarn(msg, (io)->display_name())
#define tr_logAddDebugIo(io, msg) tr_logAddDebug(msg, (io)->display_name())
#define tr_logAddTraceIo(io, msg) tr_logAddTrace(msg, (io)->display_name())

namespace
{
// Helps us to ignore errors that say "try again later"
// since that's what peer-io does by default anyway.
[[nodiscard]] constexpr auto can_retry_from_error(int error_code) noexcept
{
#ifdef _WIN32
    return error_code == 0 || error_code == WSAEWOULDBLOCK || error_code == WSAEINTR || error_code == WSAEINPROGRESS;
#else
    return error_code == 0 || error_code == EAGAIN || error_code == EWOULDBLOCK || error_code == EINTR ||
        error_code == EINPROGRESS;
#endif
}

size_t get_desired_output_buffer_size(tr_peerIo const* io, uint64_t now)
{
    // this is all kind of arbitrary, but what seems to work well is
    // being large enough to hold the next 20 seconds' worth of input,
    // or a few blocks, whichever is bigger. OK to tweak this as needed.
    static auto constexpr PeriodSecs = uint64_t{ 15U };

    // the 3 is an arbitrary number of blocks;
    // the .5 is to leave room for protocol messages
    static auto constexpr Floor = static_cast<uint64_t>(tr_block_info::BlockSize * 3.5);

    auto const current_speed = io->get_piece_speed(now, TR_UP);
    return std::max(Floor, current_speed.base_quantity() * PeriodSecs);
}

void log_peer_io_bandwidth(tr_peerIo const& peer_io, tr_bandwidth* const parent)
{
    tr_logAddTraceIo(
        &peer_io,
        fmt::format("bandwidth is {}; its parent is {}", fmt::ptr(&peer_io.bandwidth()), fmt::ptr(parent)));
}
} // namespace

// ---

tr_peerIo::tr_peerIo(
    tr_session* session,
    tr_sha1_digest_t const* info_hash,
    bool is_incoming,
    bool client_is_seed,
    tr_bandwidth* parent_bandwidth)
    : bandwidth_{ parent_bandwidth }
    , info_hash_{ info_hash != nullptr ? *info_hash : tr_sha1_digest_t{} }
    , session_{ session }
    , client_is_seed_{ client_is_seed }
    , is_incoming_{ is_incoming }
{
}

std::shared_ptr<tr_peerIo> tr_peerIo::create(
    tr_session* session,
    tr_bandwidth* parent,
    tr_sha1_digest_t const* info_hash,
    bool is_incoming,
    bool is_seed)
{
    TR_ASSERT(session != nullptr);
    auto lock = session->unique_lock();

    auto io = std::make_shared<tr_peerIo>(session, info_hash, is_incoming, is_seed, parent);
    io->bandwidth().set_peer(io);
    return io;
}

std::shared_ptr<tr_peerIo> tr_peerIo::new_incoming(tr_session* session, tr_bandwidth* parent, tr_peer_socket socket)
{
    TR_ASSERT(session != nullptr);

    auto peer_io = tr_peerIo::create(session, parent, nullptr, true, false);
    peer_io->set_socket(std::move(socket));
    log_peer_io_bandwidth(*peer_io, parent);
    return peer_io;
}

std::shared_ptr<tr_peerIo> tr_peerIo::new_outgoing(
    tr_session* session,
    tr_bandwidth* parent,
    tr_socket_address const& socket_address,
    tr_sha1_digest_t const& info_hash,
    bool client_is_seed,
    bool utp)
{
    using preferred_key_t = std::underlying_type_t<tr_preferred_transport>;
    auto const preferred = session->preferred_transport();

    TR_ASSERT(!tr_peer_socket::limit_reached(session));
    TR_ASSERT(session != nullptr);
    TR_ASSERT(socket_address.is_valid());
    TR_ASSERT(utp || session->allowsTCP());

    auto peer_io = tr_peerIo::create(session, parent, &info_hash, false, client_is_seed);
    auto const func = small::max_size_map<preferred_key_t, std::function<bool()>, TR_NUM_PREFERRED_TRANSPORT>{
        { TR_PREFER_UTP,
          [&]()
          {
#ifdef WITH_UTP
              if (utp)
              {
                  auto* const sock = utp_create_socket(session->utp_context);
                  utp_set_userdata(sock, peer_io.get());
                  peer_io->set_socket(tr_peer_socket{ socket_address, sock });

                  auto const [ss, sslen] = socket_address.to_sockaddr();
                  if (utp_connect(sock, reinterpret_cast<sockaddr const*>(&ss), sslen) == 0)
                  {
                      return true;
                  }
              }
#endif
              return false;
          } },
        { TR_PREFER_TCP,
          [&]()
          {
              if (!peer_io->socket_.is_valid())
              {
                  if (auto sock = tr_netOpenPeerSocket(session, socket_address, client_is_seed); sock.is_valid())
                  {
                      peer_io->set_socket(std::move(sock));
                      return true;
                  }
              }
              return false;
          } }
    };

    if (func.at(preferred)())
    {
        log_peer_io_bandwidth(*peer_io, parent);
        return peer_io;
    }
    for (preferred_key_t i = 0U; i < TR_NUM_PREFERRED_TRANSPORT; ++i)
    {
        if (i != preferred && func.at(i)())
        {
            log_peer_io_bandwidth(*peer_io, parent);
            return peer_io;
        }
    }

    return {};
}

tr_peerIo::~tr_peerIo()
{
    auto const lock = session_->unique_lock();

    clear_callbacks();
    tr_logAddTraceIo(this, "in tr_peerIo destructor");
    event_disable(EV_READ | EV_WRITE);
    close();
}

// ---

void tr_peerIo::set_socket(tr_peer_socket socket_in)
{
    close(); // tear down the previous socket, if any

    socket_ = std::move(socket_in);

    if (socket_.is_tcp())
    {
        event_read_.reset(event_new(session_->event_base(), socket_.handle.tcp, EV_READ, &tr_peerIo::event_read_cb, this));
        event_write_.reset(event_new(session_->event_base(), socket_.handle.tcp, EV_WRITE, &tr_peerIo::event_write_cb, this));
    }
#ifdef WITH_UTP
    else if (socket_.is_utp())
    {
        utp_set_userdata(socket_.handle.utp, this);
    }
#endif
    else
    {
        TR_ASSERT_MSG(false, "unsupported peer socket type");
    }
}

void tr_peerIo::close()
{
    socket_.close();
    event_write_.reset();
    event_read_.reset();
    inbuf_.clear();
    outbuf_.clear();
    outbuf_info_.clear();
    encrypt_disable();
    decrypt_disable();
}

void tr_peerIo::clear()
{
    clear_callbacks();
    set_enabled(TR_UP, false);
    set_enabled(TR_DOWN, false);
    close();
}

bool tr_peerIo::reconnect()
{
    TR_ASSERT(!is_incoming());
    TR_ASSERT(session_->allowsTCP());

    auto const pending_events = pending_events_;
    event_disable(EV_READ | EV_WRITE);

    close();

    auto sock = tr_netOpenPeerSocket(session_, socket_address(), client_is_seed());
    if (!sock.is_tcp())
    {
        return false;
    }
    set_socket(std::move(sock));

    event_enable(pending_events);

    return true;
}

// ---

void tr_peerIo::did_write_wrapper(size_t bytes_transferred)
{
    auto const keep_alive = shared_from_this();

    while (bytes_transferred != 0U && !std::empty(outbuf_info_))
    {
        auto& [n_bytes_left, is_piece_data] = outbuf_info_.front();

        size_t const payload = std::min(uint64_t{ n_bytes_left }, uint64_t{ bytes_transferred });
        /* For µTP sockets, the overhead is computed in utp_on_overhead. */
        size_t const overhead = socket_.guess_packet_overhead(payload);
        uint64_t const now = tr_time_msec();

        bandwidth().notify_bandwidth_consumed(TR_UP, payload, is_piece_data, now);

        if (overhead > 0U)
        {
            bandwidth().notify_bandwidth_consumed(TR_UP, overhead, false, now);
        }

        if (did_write_ != nullptr)
        {
            did_write_(this, payload, is_piece_data, user_data_);
        }

        bytes_transferred -= payload;
        n_bytes_left -= payload;
        if (n_bytes_left == 0U)
        {
            outbuf_info_.pop_front();
        }
    }
}

size_t tr_peerIo::try_write(size_t max)
{
    static auto constexpr Dir = TR_UP;

    if (max == 0U)
    {
        return {};
    }

    auto& buf = outbuf_;
    max = std::min(max, std::size(buf));
    max = bandwidth().clamp(Dir, max);
    if (max == 0U)
    {
        set_enabled(Dir, false);
        return {};
    }

    auto error = tr_error{};
    auto const n_written = socket_.try_write(buf, max, &error);
    // enable further writes if there's more data to write
    set_enabled(Dir, !std::empty(buf) && (!error || can_retry_from_error(error.code())));

    if (error)
    {
        if (!can_retry_from_error(error.code()))
        {
            tr_logAddTraceIo(
                this,
                fmt::format("try_write err: wrote:{}, errno:{} ({})", n_written, error.code(), error.message()));
            call_error_callback(error);
        }
    }
    else if (n_written > 0U)
    {
        did_write_wrapper(n_written);
    }

    return n_written;
}

void tr_peerIo::event_write_cb([[maybe_unused]] evutil_socket_t fd, short /*event*/, void* vio)
{
    auto* const io = static_cast<tr_peerIo*>(vio);
    tr_logAddTraceIo(io, "libevent says this peer socket is ready for writing");

    TR_ASSERT(io->socket_.is_tcp());
    TR_ASSERT(io->socket_.handle.tcp == fd);

    io->pending_events_ &= ~EV_WRITE;

    // Write as much as possible. Since the socket is non-blocking,
    // write() will return if it can't write any more without blocking
    io->try_write(SIZE_MAX);
}

// ---

void tr_peerIo::can_read_wrapper()
{
    // try to consume the input buffer

    if (can_read_ == nullptr)
    {
        return;
    }

    auto const lock = session_->unique_lock();
    auto const keep_alive = shared_from_this();

    auto const now = tr_time_msec();
    auto done = false;
    auto err = false;

    // In normal conditions, only continue processing if we still have bandwidth
    // quota for it.
    //
    // The read buffer will grow indefinitely if libutp or the TCP stack keeps buffering
    // data faster than the bandwidth limit allows. To safeguard against that, we keep
    // processing if the read buffer is more than twice as large as the target size.
    while (!done && !err && (read_buffer_size() > RcvBuf * 2U || bandwidth().clamp(TR_DOWN, read_buffer_size()) != 0U))
    {
        size_t piece = 0U;
        auto const old_len = read_buffer_size();
        auto const read_state = can_read_ != nullptr ? can_read_(this, user_data_, &piece) : ReadState::Err;
        auto const used = old_len - read_buffer_size();
        auto const overhead = socket_.guess_packet_overhead(used);

        if (piece != 0U)
        {
            bandwidth().notify_bandwidth_consumed(TR_DOWN, piece, true, now);
        }

        if (used != piece)
        {
            bandwidth().notify_bandwidth_consumed(TR_DOWN, used - piece, false, now);
        }

        if (overhead > 0U)
        {
            bandwidth().notify_bandwidth_consumed(TR_DOWN, overhead, false, now);
        }

        switch (read_state)
        {
        case ReadState::Now:
        case ReadState::Break:
            if (std::empty(inbuf_))
            {
                done = true;
            }
            break;

        case ReadState::Later:
            done = true;
            break;

        case ReadState::Err:
            err = true;
            break;
        }
    }
}

size_t tr_peerIo::try_read(size_t max)
{
    static auto constexpr Dir = TR_DOWN;

    if (max == 0U)
    {
        return {};
    }

    // Do not write more than the bandwidth allows.
    // If there is no bandwidth left available, disable writes.
    max = bandwidth().clamp(Dir, max);
    if (max == 0U)
    {
        set_enabled(Dir, false);
        return {};
    }

    auto& buf = inbuf_;
    auto error = tr_error{};
    auto const n_read = socket_.try_read(buf, max, std::empty(buf), &error);
    set_enabled(Dir, !error || can_retry_from_error(error.code()));

    if (error)
    {
        if (!can_retry_from_error(error.code()))
        {
            tr_logAddTraceIo(this, fmt::format("try_read err: n_read:{} errno:{} ({})", n_read, error.code(), error.message()));
            call_error_callback(error);
        }
    }
    else if (!std::empty(buf))
    {
        can_read_wrapper();
    }

    return n_read;
}

void tr_peerIo::event_read_cb([[maybe_unused]] evutil_socket_t fd, short /*event*/, void* vio)
{
    static auto constexpr MaxLen = RcvBuf;

    auto* const io = static_cast<tr_peerIo*>(vio);
    tr_logAddTraceIo(io, "libevent says this peer socket is ready for reading");

    TR_ASSERT(io->socket_.is_tcp());
    TR_ASSERT(io->socket_.handle.tcp == fd);

    io->pending_events_ &= ~EV_READ;

    // if we don't have any bandwidth left, stop reading
    auto const n_used = std::size(io->inbuf_);
    auto const n_left = n_used >= MaxLen ? 0U : MaxLen - n_used;
    io->try_read(n_left);
}

// ---

void tr_peerIo::event_enable(short event)
{
    TR_ASSERT(session_ != nullptr);

    bool const need_events = socket_.is_tcp();
    TR_ASSERT(!need_events || event_read_);
    TR_ASSERT(!need_events || event_write_);

    if ((event & EV_READ) != 0 && (pending_events_ & EV_READ) == 0)
    {
        tr_logAddTraceIo(this, "enabling ready-to-read polling");

        if (need_events)
        {
            event_add(event_read_.get(), nullptr);
        }

        pending_events_ |= EV_READ;
    }

    if ((event & EV_WRITE) != 0 && (pending_events_ & EV_WRITE) == 0)
    {
        tr_logAddTraceIo(this, "enabling ready-to-write polling");

        if (need_events)
        {
            event_add(event_write_.get(), nullptr);
        }

        pending_events_ |= EV_WRITE;
    }
}

void tr_peerIo::event_disable(short event)
{
    bool const need_events = socket_.is_tcp();
    TR_ASSERT(!need_events || event_read_);
    TR_ASSERT(!need_events || event_write_);

    if ((event & EV_READ) != 0 && (pending_events_ & EV_READ) != 0)
    {
        tr_logAddTraceIo(this, "disabling ready-to-read polling");

        if (need_events)
        {
            event_del(event_read_.get());
        }

        pending_events_ &= ~EV_READ;
    }

    if ((event & EV_WRITE) != 0 && (pending_events_ & EV_WRITE) != 0)
    {
        tr_logAddTraceIo(this, "disabling ready-to-write polling");

        if (need_events)
        {
            event_del(event_write_.get());
        }

        pending_events_ &= ~EV_WRITE;
    }
}

void tr_peerIo::set_enabled(tr_direction dir, bool is_enabled)
{
    TR_ASSERT(tr_isDirection(dir));

    short const event = dir == TR_UP ? EV_WRITE : EV_READ;

    if (is_enabled)
    {
        event_enable(event);
    }
    else
    {
        event_disable(event);
    }
}

size_t tr_peerIo::flush(tr_direction dir, size_t limit)
{
    TR_ASSERT(tr_isDirection(dir));

    return dir == TR_DOWN ? try_read(limit) : try_write(limit);
}

size_t tr_peerIo::flush_outgoing_protocol_msgs()
{
    size_t byte_count = 0U;

    /* count up how many bytes are used by non-piece-data messages
       at the front of our outbound queue */
    for (auto const& [n_bytes, is_piece_data] : outbuf_info_)
    {
        if (is_piece_data)
        {
            break;
        }

        byte_count += n_bytes;
    }

    return flush(TR_UP, byte_count);
}

// ---

size_t tr_peerIo::get_write_buffer_space(uint64_t now) const noexcept
{
    size_t const desired_len = get_desired_output_buffer_size(this, now);
    size_t const current_len = std::size(outbuf_);
    return desired_len > current_len ? desired_len - current_len : 0U;
}

// ---

void tr_peerIo::read_bytes(void* bytes, size_t n_bytes)
{
    auto walk = reinterpret_cast<std::byte*>(bytes);
    n_bytes = std::min(n_bytes, std::size(inbuf_));
    if (n_decrypt_remain_)
    {
        if (auto& n_remain = *n_decrypt_remain_; n_remain <= n_bytes)
        {
            filter_.decrypt(std::data(inbuf_), n_remain, walk);
            inbuf_.drain(n_remain);
            if (walk != nullptr)
            {
                walk += n_remain;
            }
            n_bytes -= n_remain;
            filter_.decrypt_disable();
            n_decrypt_remain_.reset();
        }
        else
        {
            n_remain -= n_bytes;
        }
    }
    filter_.decrypt(std::data(inbuf_), n_bytes, walk);
    inbuf_.drain(n_bytes);
}

void tr_peerIo::read_uint16(uint16_t* setme)
{
    auto tmp = uint16_t{};
    read_bytes(&tmp, sizeof(tmp));
    *setme = ntohs(tmp);
}

void tr_peerIo::read_uint32(uint32_t* setme)
{
    auto tmp = uint32_t{};
    read_bytes(&tmp, sizeof(tmp));
    *setme = ntohl(tmp);
}

// --- UTP

#ifdef WITH_UTP

void tr_peerIo::on_utp_state_change(int state)
{
    if (state == UTP_STATE_CONNECT)
    {
        tr_logAddTraceIo(this, "utp_on_state_change -- changed to connected");
    }
    else if (state == UTP_STATE_WRITABLE)
    {
        tr_logAddTraceIo(this, "utp_on_state_change -- changed to writable");

        if ((pending_events_ & EV_WRITE) != 0)
        {
            try_write(SIZE_MAX);
        }
    }
    else if (state == UTP_STATE_EOF)
    {
        auto error = tr_error{};
        error.set_from_errno(ENOTCONN);
        call_error_callback(error);
    }
    else if (state == UTP_STATE_DESTROYING)
    {
        tr_logAddErrorIo(this, "Impossible state UTP_STATE_DESTROYING");
    }
    else
    {
        tr_logAddErrorIo(this, fmt::format(_("Unknown state: {state}"), fmt::arg("state", state)));
    }
}

void tr_peerIo::on_utp_error(int errcode)
{
    tr_logAddTraceIo(this, fmt::format("utp_on_error -- {}", utp_error_code_names[errcode]));

    if (got_error_ == nullptr)
    {
        return;
    }

    auto error = tr_error{};
    switch (errcode)
    {
    case UTP_ECONNREFUSED:
        error.set_from_errno(ECONNREFUSED);
        break;
    case UTP_ECONNRESET:
        error.set_from_errno(ECONNRESET);
        break;
    case UTP_ETIMEDOUT:
        error.set_from_errno(ETIMEDOUT);
        break;
    default:
        error.set(errcode, utp_error_code_names[errcode]);
        break;
    }

    call_error_callback(error);
}

#endif /* #ifdef WITH_UTP */

void tr_peerIo::utp_init([[maybe_unused]] struct_utp_context* ctx)
{
#ifdef WITH_UTP
    utp_context_set_option(ctx, UTP_RCVBUF, RcvBuf);

    // note: all the callback handlers here need to check `userdata` for nullptr
    // because libutp can fire callbacks on a socket after utp_close() is called

    utp_set_callback(
        ctx,
        UTP_ON_READ,
        [](utp_callback_arguments* args) -> uint64
        {
            if (auto* const io = static_cast<tr_peerIo*>(utp_get_userdata(args->socket)); io != nullptr)
            {
                // The peer io object can destruct inside can_read_wrapper(), so keep
                // it alive for the duration of this code block. This can happen when
                // a BT handshake did not complete successfully for example.
                auto const keep_alive = io->shared_from_this();

                io->inbuf_.add(args->buf, args->len);
                io->set_enabled(TR_DOWN, true);
                io->can_read_wrapper();

                // utp_read_drained() notifies libutp that we read a packet from them.
                // It opens up the congestion window by sending an ACK (soonish) if
                // one was not going to be sent.
                utp_read_drained(args->socket);
            }
            return {};
        });

    utp_set_callback(
        ctx,
        UTP_GET_READ_BUFFER_SIZE,
        [](utp_callback_arguments* args) -> uint64
        {
            if (auto const* const io = static_cast<tr_peerIo*>(utp_get_userdata(args->socket)); io != nullptr)
            {
                return io->read_buffer_size();
            }
            return {};
        });

    utp_set_callback(
        ctx,
        UTP_ON_ERROR,
        [](utp_callback_arguments* args) -> uint64
        {
            if (auto* const io = static_cast<tr_peerIo*>(utp_get_userdata(args->socket)); io != nullptr)
            {
                io->on_utp_error(args->error_code);
            }
            return {};
        });

    utp_set_callback(
        ctx,
        UTP_ON_OVERHEAD_STATISTICS,
        [](utp_callback_arguments* args) -> uint64
        {
            if (auto* const io = static_cast<tr_peerIo*>(utp_get_userdata(args->socket)); io != nullptr)
            {
                tr_logAddTraceIo(io, fmt::format("{:d} overhead bytes via utp", args->len));
                io->bandwidth().notify_bandwidth_consumed(args->send != 0 ? TR_UP : TR_DOWN, args->len, false, tr_time_msec());
            }
            return {};
        });

    utp_set_callback(
        ctx,
        UTP_ON_STATE_CHANGE,
        [](utp_callback_arguments* args) -> uint64
        {
            if (auto* const io = static_cast<tr_peerIo*>(utp_get_userdata(args->socket)); io != nullptr)
            {
                io->on_utp_state_change(args->state);
            }
            return {};
        });
#endif
}
