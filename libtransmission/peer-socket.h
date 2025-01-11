// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <atomic>
#include <cstddef> // size_t
#include <string>
#include <utility> // for std::make_pair()

#include "libtransmission/net.h"
#include "libtransmission/tr-buffer.h"

struct UTPSocket;
struct tr_session;

class tr_peer_socket
{
public:
    using InBuf = libtransmission::BufferWriter<std::byte>;
    using OutBuf = libtransmission::BufferReader<std::byte>;

    tr_peer_socket() = default;
    tr_peer_socket(tr_session const* session, tr_socket_address const& socket_address, tr_socket_t sock);
    tr_peer_socket(tr_socket_address const& socket_address, struct UTPSocket* sock);
    tr_peer_socket(tr_peer_socket&& s) noexcept
    {
        *this = std::move(s);
    }
    tr_peer_socket(tr_peer_socket const&) = delete;
    tr_peer_socket& operator=(tr_peer_socket&& s) noexcept
    {
        close();
        handle = s.handle;
        socket_address_ = s.socket_address_;
        type_ = s.type_;
        // invalidate s.type_, s.handle so s.close() won't break anything
        s.type_ = Type::None;
        s.handle = {};
        return *this;
    }
    tr_peer_socket& operator=(tr_peer_socket const&) = delete;
    ~tr_peer_socket()
    {
        close();
    }
    void close();

    size_t try_read(InBuf& buf, size_t max, bool buf_is_empty, tr_error* error) const;
    size_t try_write(OutBuf& buf, size_t max, tr_error* error) const;

    [[nodiscard]] constexpr auto const& socket_address() const noexcept
    {
        return socket_address_;
    }

    [[nodiscard]] constexpr auto const& address() const noexcept
    {
        return socket_address_.address();
    }

    [[nodiscard]] constexpr auto port() const noexcept
    {
        return socket_address_.port();
    }

    [[nodiscard]] std::string display_name() const
    {
        return socket_address_.display_name();
    }

    [[nodiscard]] constexpr auto is_utp() const noexcept
    {
        return type_ == Type::UTP;
    }

    [[nodiscard]] constexpr auto is_tcp() const noexcept
    {
        return type_ == Type::TCP;
    }

    [[nodiscard]] constexpr auto is_valid() const noexcept
    {
#ifdef WITH_UTP
        return is_tcp() || is_utp();
#else
        return is_tcp();
#endif
    }

    union
    {
        tr_socket_t tcp;
        struct UTPSocket* utp;
    } handle = {};

    [[nodiscard]] static bool limit_reached(tr_session const* session) noexcept;

private:
    enum class Type
    {
        None,
        TCP,
        UTP
    };

    tr_socket_address socket_address_;

    enum Type type_ = Type::None;

    static inline std::atomic<size_t> n_open_sockets_ = {};
};
