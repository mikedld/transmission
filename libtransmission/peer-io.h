// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <algorithm>
#include <cstddef> // size_t
#include <cstdint> // uintX_t
#include <deque>
#include <memory>
#include <optional>
#include <utility> // std::pair

#include <event2/util.h> // for evutil_socket_t

#include "libtransmission/transmission.h"

#include "libtransmission/bandwidth.h"
#include "libtransmission/block-info.h"
#include "libtransmission/peer-mse.h"
#include "libtransmission/peer-socket.h"
#include "libtransmission/tr-buffer.h"
#include "libtransmission/tr-macros.h" // tr_sha1_digest_t, TR_CONSTEXPR20
#include "libtransmission/utils-ev.h"

struct struct_utp_context;
struct tr_error;
struct tr_session;
struct tr_socket_address;

namespace libtransmission::test
{
class HandshakeTest;
} // namespace libtransmission::test

enum class ReadState : uint8_t
{
    Now,
    Later,
    Break,
    Err
};

enum tr_preferred_transport : uint8_t
{
    // More preferred transports goes on top
    TR_PREFER_UTP,
    TR_PREFER_TCP,
    TR_NUM_PREFERRED_TRANSPORT
};

class tr_peerIo final : public std::enable_shared_from_this<tr_peerIo>
{
    using DH = tr_message_stream_encryption::DH;
    using Filter = tr_message_stream_encryption::Filter;
    using CanRead = ReadState (*)(tr_peerIo* io, void* user_data, size_t* setme_piece_byte_count);
    using DidWrite = void (*)(tr_peerIo* io, size_t bytesWritten, bool wasPieceData, void* userData);
    using GotError = void (*)(tr_peerIo* io, tr_error const& error, void* userData);

public:
    tr_peerIo(
        tr_session* session_in,
        tr_sha1_digest_t const* info_hash,
        bool is_incoming,
        bool client_is_seed,
        tr_bandwidth* parent_bandwidth);

    ~tr_peerIo();

    static std::shared_ptr<tr_peerIo> new_outgoing(
        tr_session* session,
        tr_bandwidth* parent,
        tr_socket_address const& socket_address,
        tr_sha1_digest_t const& info_hash,
        bool client_is_seed,
        bool utp);

    static std::shared_ptr<tr_peerIo> new_incoming(tr_session* session, tr_bandwidth* parent, tr_peer_socket socket);

    constexpr void set_callbacks(CanRead can_read, DidWrite did_write, GotError got_error, void* user_data)
    {
        can_read_ = can_read;
        did_write_ = did_write;
        got_error_ = got_error;
        user_data_ = user_data;
    }

    constexpr void clear_callbacks()
    {
        set_callbacks(nullptr, nullptr, nullptr, nullptr);
    }

    void set_socket(tr_peer_socket);

    [[nodiscard]] constexpr auto is_utp() const noexcept
    {
        return socket_.is_utp();
    }

    void clear();

    [[nodiscard]] bool reconnect();

    void set_enabled(tr_direction dir, bool is_enabled);

    ///

    [[nodiscard]] TR_CONSTEXPR20 auto read_buffer_size() const noexcept
    {
        return std::size(inbuf_);
    }

    template<typename T>
    [[nodiscard]] auto read_buffer_starts_with(T const& t) const noexcept
    {
        return inbuf_.starts_with(t);
    }

    void read_buffer_discard(size_t n_bytes)
    {
        read_bytes(nullptr, n_bytes);
    }

    void read_bytes(void* bytes, size_t n_bytes);

    void read_uint8(uint8_t* setme)
    {
        read_bytes(setme, sizeof(uint8_t));
    }

    void read_uint16(uint16_t* setme);

    void read_uint32(uint32_t* setme);

    ///

    [[nodiscard]] size_t get_write_buffer_space(uint64_t now) const noexcept;

    void write_bytes(void const* bytes, size_t n_bytes, bool is_piece_data)
    {
        outbuf_info_.emplace_back(n_bytes, is_piece_data);

        auto [resbuf, reslen] = outbuf_.reserve_space(n_bytes);
        filter_.encrypt(reinterpret_cast<std::byte const*>(bytes), n_bytes, resbuf);
        outbuf_.commit_space(n_bytes);
    }

    // Write all the data from `buf`.
    // This is a destructive add: `buf` is empty after this call.
    template<typename T>
    void write(libtransmission::BufferReader<T>& buf, bool is_piece_data)
    {
        auto const n_bytes = std::size(buf);
        write_bytes(std::data(buf), n_bytes, is_piece_data);
        buf.drain(n_bytes);
    }

    size_t flush_outgoing_protocol_msgs();

    size_t flush(tr_direction dir, size_t byte_limit);

    ///

    [[nodiscard]] auto has_bandwidth_left(tr_direction dir) const noexcept
    {
        return bandwidth_.clamp(dir, 1024) > 0;
    }

    [[nodiscard]] auto get_piece_speed(uint64_t now, tr_direction dir) const noexcept
    {
        return bandwidth_.get_piece_speed(now, dir);
    }

    ///

    [[nodiscard]] constexpr auto supports_fext() const noexcept
    {
        return fast_extension_supported_;
    }

    constexpr void set_supports_fext(bool flag) noexcept
    {
        fast_extension_supported_ = flag;
    }

    ///

    [[nodiscard]] constexpr auto supports_ltep() const noexcept
    {
        return extended_protocol_supported_;
    }

    constexpr void set_supports_ltep(bool flag) noexcept
    {
        extended_protocol_supported_ = flag;
    }

    ///

    [[nodiscard]] constexpr auto supports_dht() const noexcept
    {
        return dht_supported_;
    }

    constexpr void set_supports_dht(bool flag) noexcept
    {
        dht_supported_ = flag;
    }

    ///

    [[nodiscard]] constexpr auto const& bandwidth() const noexcept
    {
        return bandwidth_;
    }

    [[nodiscard]] constexpr auto& bandwidth() noexcept
    {
        return bandwidth_;
    }

    void set_bandwidth(tr_bandwidth* parent)
    {
        bandwidth_.set_parent(parent);
    }

    ///

    [[nodiscard]] constexpr auto const& torrent_hash() const noexcept
    {
        return info_hash_;
    }

    void set_torrent_hash(tr_sha1_digest_t const& hash) noexcept
    {
        info_hash_ = hash;
    }

    ///

    [[nodiscard]] constexpr auto priority() const noexcept
    {
        return priority_;
    }

    constexpr void set_priority(tr_priority_t priority)
    {
        priority_ = priority;
    }

    ///

    [[nodiscard]] constexpr auto is_incoming() const noexcept
    {
        return is_incoming_;
    }

    [[nodiscard]] constexpr auto const& address() const noexcept
    {
        return socket_.address();
    }

    [[nodiscard]] constexpr auto const& socket_address() const noexcept
    {
        return socket_.socket_address();
    }

    [[nodiscard]] auto display_name() const
    {
        return socket_.display_name();
    }

    ///

    [[nodiscard]] constexpr auto is_encrypted() const noexcept
    {
        return filter_.is_active();
    }

    void decrypt_init(bool is_incoming, DH const& dh, tr_sha1_digest_t const& info_hash)
    {
        n_decrypt_remain_.reset();
        filter_.decrypt_init(is_incoming, dh, info_hash);
    }

    TR_CONSTEXPR20 void decrypt_disable(size_t decrypt_len = 0U) noexcept
    {
        // optionally decrypt decrypt_len more bytes before disabling decryption
        n_decrypt_remain_ = decrypt_len;
    }

    void encrypt_init(bool is_incoming, DH const& dh, tr_sha1_digest_t const& info_hash)
    {
        filter_.encrypt_init(is_incoming, dh, info_hash);
    }

    constexpr void encrypt_disable() noexcept
    {
        // unlike the read buffer, we don't need to "encrypt xxx
        // more bytes before disabling encryption" since we control
        // whether we add data before or after calling encrypt_disable()
        filter_.encrypt_disable();
    }

    ///

    static void utp_init(struct_utp_context* ctx);

private:
    // Our target socket receive buffer size.
    // Gets read from the socket buffer into the PeerBuffer inbuf_.
    static constexpr auto RcvBuf = size_t{ 256 * 1024 };

    // The buffer size for incoming & outgoing peer messages.
    // Starts off with enough capacity to read a single BT Piece message,
    // but has a 5x GrowthFactor so that it can quickly to high volume.
    using PeerBuffer = libtransmission::StackBuffer<tr_block_info::BlockSize + 16U, std::byte, std::ratio<5, 1>>;

    friend class libtransmission::test::HandshakeTest;

    [[nodiscard]] constexpr auto client_is_seed() const noexcept
    {
        return client_is_seed_;
    }

    void call_error_callback(tr_error const& error)
    {
        if (got_error_ != nullptr)
        {
            got_error_(this, error, user_data_);
        }
    }

#ifdef WITH_UTP
    void on_utp_state_change(int new_state);
    void on_utp_error(int errcode);
#endif

    void close();

    static void event_read_cb(evutil_socket_t fd, short /*event*/, void* vio);
    static void event_write_cb(evutil_socket_t fd, short /*event*/, void* vio);

    void event_enable(short event);
    void event_disable(short event);

    void can_read_wrapper();
    void did_write_wrapper(size_t bytes_transferred);

    size_t try_read(size_t max);
    size_t try_write(size_t max);

    // this is only public for testing purposes.
    // production code should use new_outgoing() or new_incoming()
    static std::shared_ptr<tr_peerIo> create(
        tr_session* session,
        tr_bandwidth* parent,
        tr_sha1_digest_t const* info_hash,
        bool is_incoming,
        bool is_seed);

    Filter filter_;
    std::optional<size_t> n_decrypt_remain_;

    std::deque<std::pair<size_t /*n_bytes*/, bool /*is_piece_data*/>> outbuf_info_;

    tr_peer_socket socket_ = {};

    tr_bandwidth bandwidth_;

    tr_sha1_digest_t info_hash_;

    PeerBuffer inbuf_;
    PeerBuffer outbuf_;

    tr_session* const session_;

    CanRead can_read_ = nullptr;
    DidWrite did_write_ = nullptr;
    GotError got_error_ = nullptr;
    void* user_data_ = nullptr;

    libtransmission::evhelpers::event_unique_ptr event_read_;
    libtransmission::evhelpers::event_unique_ptr event_write_;

    short int pending_events_ = 0;

    tr_priority_t priority_ = TR_PRI_NORMAL;

    bool const client_is_seed_;
    bool const is_incoming_;

    bool dht_supported_ = false;
    bool extended_protocol_supported_ = false;
    bool fast_extension_supported_ = false;
};
