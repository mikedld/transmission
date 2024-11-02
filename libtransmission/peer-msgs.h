// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <array>
#include <atomic>
#include <cstddef> // for size_t
#include <memory>

#include "libtransmission/transmission.h" // for tr_direction, tr_block_ind...

#include "libtransmission/interned-string.h"
#include "libtransmission/net.h" // tr_socket_address
#include "libtransmission/peer-common.h" // for tr_peer

class tr_peerIo;
class tr_peerMsgs;
class tr_peer_info;
struct tr_torrent;

/**
 * @addtogroup peers Peers
 * @{
 */

using tr_peer_callback_bt = void (*)(tr_peerMsgs* peer, tr_peer_event const& event, void* client_data);

class tr_peerMsgs : public tr_peer
{
public:
    tr_peerMsgs(
        tr_torrent const& tor,
        std::shared_ptr<tr_peer_info> peer_info_in,
        tr_interned_string user_agent,
        bool connection_is_encrypted,
        bool connection_is_incoming,
        bool connection_is_utp);

    ~tr_peerMsgs() override;

    [[nodiscard]] static auto size() noexcept
    {
        return n_peers.load();
    }

    [[nodiscard]] constexpr auto client_is_choked() const noexcept
    {
        return client_is_choked_;
    }

    [[nodiscard]] constexpr auto client_is_interested() const noexcept
    {
        return client_is_interested_;
    }

    [[nodiscard]] constexpr auto peer_is_choked() const noexcept
    {
        return peer_is_choked_;
    }

    [[nodiscard]] constexpr auto peer_is_interested() const noexcept
    {
        return peer_is_interested_;
    }

    [[nodiscard]] constexpr auto is_encrypted() const noexcept
    {
        return connection_is_encrypted_;
    }

    [[nodiscard]] constexpr auto is_incoming_connection() const noexcept
    {
        return connection_is_incoming_;
    }

    [[nodiscard]] constexpr auto is_utp_connection() const noexcept
    {
        return connection_is_utp_;
    }

    [[nodiscard]] constexpr auto const& user_agent() const noexcept
    {
        return user_agent_;
    }

    [[nodiscard]] constexpr auto is_active(tr_direction direction) const noexcept
    {
        return is_active_[direction];
    }

    [[nodiscard]] constexpr auto is_disconnecting() const noexcept
    {
        return is_disconnecting_;
    }

    constexpr void disconnect_soon() noexcept
    {
        is_disconnecting_ = true;
    }

    [[nodiscard]] virtual tr_socket_address socket_address() const = 0;

    virtual void set_choke(bool peer_is_choked) = 0;
    virtual void set_interested(bool client_is_interested) = 0;

    virtual void pulse() = 0;

    virtual void on_torrent_got_metainfo() noexcept = 0;

    virtual void on_piece_completed(tr_piece_index_t) = 0;

    static tr_peerMsgs* create(
        tr_torrent& torrent,
        std::shared_ptr<tr_peer_info> peer_info,
        std::shared_ptr<tr_peerIo> io,
        tr_interned_string user_agent,
        tr_peer_callback_bt callback,
        void* callback_data);

protected:
    constexpr void set_client_choked(bool val) noexcept
    {
        client_is_choked_ = val;
    }

    constexpr void set_client_interested(bool val) noexcept
    {
        client_is_interested_ = val;
    }

    constexpr void set_peer_choked(bool val) noexcept
    {
        peer_is_choked_ = val;
    }

    constexpr void set_peer_interested(bool val) noexcept
    {
        peer_is_interested_ = val;
    }

    constexpr void set_active(tr_direction direction, bool active) noexcept
    {
        is_active_[direction] = active;
    }

    constexpr void set_user_agent(tr_interned_string val) noexcept
    {
        user_agent_ = val;
    }

public:
    std::shared_ptr<tr_peer_info> const peer_info;

private:
    static inline auto n_peers = std::atomic<size_t>{};

    // What software the peer is running.
    // Derived from the `v` string in LTEP's handshake dictionary, when available.
    tr_interned_string user_agent_;

    bool const connection_is_encrypted_;
    bool const connection_is_incoming_;
    bool const connection_is_utp_;

    std::array<bool, 2> is_active_ = {};

    // whether or not the peer is choking us.
    bool client_is_choked_ = true;

    // whether or not we've indicated to the peer that we would download from them if unchoked
    bool client_is_interested_ = false;

    // whether or not we've choked this peer
    bool peer_is_choked_ = true;

    // whether or not the peer has indicated it will download from us
    bool peer_is_interested_ = false;

    // whether or not we should free this peer soon.
    bool is_disconnecting_ = false;
};

/* @} */
