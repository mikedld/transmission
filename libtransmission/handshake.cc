// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno> // ECONNREFUSED, ETIMEDOUT
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/bitfield.h"
#include "libtransmission/clients.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/handshake.h"
#include "libtransmission/log.h"
#include "libtransmission/peer-io.h"
#include "libtransmission/peer-mse.h" // tr_message_stream_encryption::DH
#include "libtransmission/timer.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-buffer.h"
#include "libtransmission/tr-macros.h" // tr_peer_id_t

#define tr_logAddTraceHand(handshake, msg) \
    tr_logAddTrace(msg, fmt::format("handshake {}", (handshake)->peer_io_->display_name()))

using namespace std::literals;
using key_bigend_t = tr_message_stream_encryption::DH::key_bigend_t;

// --- Outgoing Connections

// 1 A->B: our public key (Ya) and some padding (PadA)
void tr_handshake::send_ya(tr_peerIo* io)
{
    tr_logAddTraceHand(this, "sending MSE handshake (Ya)");
    pad_a_len_ = send_public_key_and_pad<PadaMaxlen>(io);
    set_state(tr_handshake::State::AwaitingYb);
}

ReadState tr_handshake::read_yb(tr_peerIo* peer_io)
{
    if (peer_io->read_buffer_size() < std::size(HandshakeName))
    {
        return ReadState::Later;
    }

    // Jump to plain handshake
    if (peer_io->read_buffer_starts_with(HandshakeName))
    {
        tr_logAddTraceHand(this, "in read_yb... got a plain incoming handshake");
        set_state(tr_handshake::State::AwaitingHandshake);
        return ReadState::Now;
    }

    auto peer_public_key = key_bigend_t{};
    tr_logAddTraceHand(
        this,
        fmt::format("in read_yb... need {}, have {}", std::size(peer_public_key), peer_io->read_buffer_size()));
    if (peer_io->read_buffer_size() < std::size(peer_public_key))
    {
        return ReadState::Later;
    }

    have_read_anything_from_peer_ = true;

    // get the peer's public key
    peer_io->read_bytes(std::data(peer_public_key), std::size(peer_public_key));
    get_dh().setPeerPublicKey(peer_public_key);

    // everything received so far is Yb+PadB; peer has not yet sent VC for resync.
    // so throw away buffer, and do early exit check: we know it's not legit MSE if > max PadB
    pad_b_len_ = peer_io->read_buffer_size();
    if (pad_b_len_ > PadbMaxlen)
    {
        return done(false);
    }
    peer_io->read_buffer_discard(pad_b_len_);

    /* now send these: HASH('req1', S), HASH('req2', SKEY) xor HASH('req3', S),
     * ENCRYPT(VC, crypto_provide, len(PadC), PadC, len(IA)), ENCRYPT(IA) */
    static auto constexpr BufSize = (std::tuple_size_v<tr_sha1_digest_t> * 2U) + std::size(VC) + sizeof(crypto_provide_) +
        sizeof(pad_c_len_) + sizeof(ia_len_) + HandshakeSize;
    auto outbuf = libtransmission::StackBuffer<BufSize, std::byte>{};

    /* HASH('req1', S) */
    outbuf.add(tr_sha1::digest("req1"sv, get_dh().secret()));

    auto const& info_hash = peer_io->torrent_hash();
    TR_ASSERT_MSG(info_hash != tr_sha1_digest_t{}, "readYb requires an info_hash");

    /* HASH('req2', SKEY) xor HASH('req3', S) */
    {
        auto const req2 = tr_sha1::digest("req2"sv, info_hash);
        auto const req3 = tr_sha1::digest("req3"sv, get_dh().secret());
        auto [x_or, n_x_or] = outbuf.reserve_space(std::tuple_size_v<tr_sha1_digest_t>);
        for (size_t i = 0; i < n_x_or; ++i)
        {
            x_or[i] = req2[i] ^ req3[i];
        }
        outbuf.commit_space(n_x_or);
    }

    /* ENCRYPT(VC, crypto_provide, len(PadC), PadC
     * PadC is reserved for future extensions to the handshake...
     * standard practice at this time is for it to be zero-length */
    crypto_provide_ = crypto_provide();
    peer_io->write(outbuf, false);
    peer_io->encrypt_init(peer_io->is_incoming(), get_dh(), info_hash);
    outbuf.add(VC);
    outbuf.add_uint32(crypto_provide_);
    outbuf.add_uint16(0);

    /* ENCRYPT len(IA)), ENCRYPT(IA) */
    outbuf.add_uint16(HandshakeSize);
    if (build_handshake_message(peer_io, outbuf))
    {
        have_sent_bittorrent_handshake_ = true;
    }
    else
    {
        return done(false);
    }

    /* send it */
    peer_io->write(outbuf, false);

    set_state(State::AwaitingVc);
    // LATER, not NOW: recv buffer was just drained and peer was blocking
    return ReadState::Later;
}

// MSE spec: "Since the length of [PadB is] unknown,
// A will be able to resynchronize on ENCRYPT(VC)"
ReadState tr_handshake::read_vc(tr_peerIo* peer_io)
{
    auto const info_hash = peer_io->torrent_hash();
    TR_ASSERT_MSG(info_hash != tr_sha1_digest_t{}, "read_vc requires an info_hash");

    // We need to find the end of PadB by looking for `ENCRYPT(VC)`,
    // so calculate and cache the value of `ENCRYPT(VC)`.
    if (!encrypted_vc_)
    {
        auto filter = tr_message_stream_encryption::Filter{};
        filter.encrypt_init(true, get_dh(), info_hash);

        encrypted_vc_.emplace();
        filter.encrypt(std::data(VC), std::size(VC), std::data(*encrypted_vc_));
    }

    for (; pad_b_len_ <= PadbMaxlen; ++pad_b_len_)
    {
        static auto constexpr Needlen = std::size(VC);
        if (peer_io->read_buffer_size() < Needlen)
        {
            tr_logAddTraceHand(
                this,
                fmt::format("in read_vc... need {}, read {}, have {}", Needlen, pad_b_len_, peer_io->read_buffer_size()));
            return ReadState::Later;
        }

        if (peer_io->read_buffer_starts_with(*encrypted_vc_))
        {
            tr_logAddTraceHand(this, "found ENCRYPT(VC)!");
            // We already know it's a match; now we just need to
            // consume it from the read buffer.
            peer_io->decrypt_init(peer_io->is_incoming(), get_dh(), info_hash);
            peer_io->read_buffer_discard(Needlen);
            set_state(tr_handshake::State::AwaitingCryptoSelect);
            return ReadState::Now;
        }

        peer_io->read_buffer_discard(1U);
    }

    tr_logAddTraceHand(this, "couldn't find ENCRYPT(VC)");
    return done(false);
}

ReadState tr_handshake::read_crypto_select(tr_peerIo* peer_io)
{
    if (static auto constexpr NeedLen = sizeof(crypto_select_) + sizeof(pad_d_len_); peer_io->read_buffer_size() < NeedLen)
    {
        return ReadState::Later;
    }

    peer_io->read_uint32(&crypto_select_);
    tr_logAddTraceHand(this, fmt::format("crypto select is {}", crypto_select_));

    if ((crypto_select_ & crypto_provide_) == 0U)
    {
        tr_logAddTraceHand(this, "peer selected an encryption option we didn't offer");
        return done(false);
    }

    peer_io->read_uint16(&pad_d_len_);
    tr_logAddTraceHand(this, fmt::format("len(PadD) is {}", pad_d_len_));
    if (pad_d_len_ > PaddMaxlen)
    {
        tr_logAddTraceHand(this, "MSE handshake: len(PadD) is too long");
        return done(false);
    }

    set_state(tr_handshake::State::AwaitingPadD);
    return ReadState::Now;
}

ReadState tr_handshake::read_pad_d(tr_peerIo* peer_io)
{
    tr_logAddTraceHand(this, fmt::format("PadD: need {}, got {}", pad_d_len_, peer_io->read_buffer_size()));
    if (peer_io->read_buffer_size() < pad_d_len_)
    {
        return ReadState::Later;
    }

    peer_io->read_buffer_discard(pad_d_len_);

    /* maybe de-encrypt our connection */
    if (crypto_select_ == CryptoProvidePlaintext)
    {
        peer_io->encrypt_disable();
        peer_io->decrypt_disable();
    }

    set_state(tr_handshake::State::AwaitingHandshake);
    return ReadState::Now;
}

// --- Incoming and Outgoing Connections

ReadState tr_handshake::read_handshake(tr_peerIo* peer_io)
{
    static auto constexpr Needlen = IncomingHandshakeLen;
    tr_logAddTraceHand(this, fmt::format("read_handshake: need {}, got {}", Needlen, peer_io->read_buffer_size()));
    if (peer_io->read_buffer_size() < Needlen)
    {
        return ReadState::Later;
    }

    if (ia_len_ > 0U)
    {
        // do nothing, the check below won't work correctly
    }
    else if (peer_io->read_buffer_starts_with(HandshakeName)) // unencrypted
    {
        if (encryption_mode_ == TR_ENCRYPTION_REQUIRED)
        {
            tr_logAddTraceHand(this, "peer is unencrypted, and we're disallowing that");
            return done(false);
        }
        if (crypto_select_ == CryptoProvideRC4)
        {
            tr_logAddTraceHand(this, "peer is unencrypted, and that does not agree with our handshake");
            return done(false);
        }
    }
    else if (crypto_select_ == CryptoProvidePlaintext) // encrypted
    {
        tr_logAddTraceHand(this, "peer is encrypted, and that does not agree with our handshake");
        return done(false);
    }

    have_read_anything_from_peer_ = true;

    auto name = decltype(HandshakeName){};
    peer_io->read_bytes(std::data(name), std::size(name));
    if (name != HandshakeName)
    {
        tr_logAddTraceHand(this, "handshake prefix not correct");
        return done(false);
    }

    // reserved bytes / flags
    auto reserved = std::array<uint8_t, HandshakeFlagsBytes>{};
    auto flags = tr_bitfield{ HandshakeFlagsBits };
    peer_io->read_bytes(std::data(reserved), std::size(reserved));
    flags.set_raw(std::data(reserved), std::size(reserved));
    peer_io->set_supports_dht(flags.test(DhtFlag));
    peer_io->set_supports_ltep(flags.test(LtepFlag));
    peer_io->set_supports_fext(flags.test(FextFlag));

    /* torrent hash */
    auto hash = tr_sha1_digest_t{};
    peer_io->read_bytes(std::data(hash), std::size(hash));

    if (is_incoming() && peer_io->torrent_hash() == tr_sha1_digest_t{}) // incoming plain handshake
    {
        if (!mediator_->torrent(hash))
        {
            tr_logAddTraceHand(this, "peer is trying to connect to us for a torrent we don't have.");
            return done(false);
        }

        peer_io->set_torrent_hash(hash);
    }
    else // outgoing, or incoming MSE handshake
    {
        if (peer_io->torrent_hash() != hash)
        {
            tr_logAddTraceHand(this, "peer returned the wrong hash. wtf?");
            return done(false);
        }
    }

    // If it's an incoming message, we need to send a response handshake
    if (!have_sent_bittorrent_handshake_)
    {
        tr_logAddTraceHand(this, "sending handshake in reply");
        if (!send_handshake(peer_io))
        {
            return done(false);
        }
    }

    set_state(State::AwaitingPeerId);
    return ReadState::Now;
}

ReadState tr_handshake::read_peer_id(tr_peerIo* peer_io)
{
    // read the peer_id
    auto peer_id = tr_peer_id_t{};
    static auto constexpr Needlen = std::size(peer_id);
    tr_logAddTraceHand(this, fmt::format("read_peer_id: need {}, got {}", Needlen, peer_io->read_buffer_size()));
    if (peer_io->read_buffer_size() < Needlen)
    {
        return ReadState::Later;
    }
    peer_io->read_bytes(std::data(peer_id), Needlen);
    set_peer_id(peer_id);

    auto client = std::array<char, 128>{};
    tr_clientForId(std::data(client), std::size(client), peer_id);
    tr_logAddTraceHand(this, fmt::format("peer-id is '{}' ... isIncoming is {}", std::data(client), is_incoming()));

    // if we've somehow connected to ourselves, don't keep the connection
    auto const info_hash = peer_io_->torrent_hash();
    auto const info = mediator_->torrent(info_hash);
    auto const connected_to_self = info && info->client_peer_id == peer_id;

    return done(!connected_to_self);
}

// --- Incoming Connections

ReadState tr_handshake::read_ya(tr_peerIo* peer_io)
{
    if (peer_io->read_buffer_size() < std::size(HandshakeName))
    {
        return ReadState::Later;
    }

    // Jump to plain handshake
    if (peer_io->read_buffer_starts_with(HandshakeName))
    {
        tr_logAddTraceHand(this, "in read_ya... got a plain incoming handshake");
        set_state(tr_handshake::State::AwaitingHandshake);
        return ReadState::Now;
    }

    auto peer_public_key = key_bigend_t{};
    tr_logAddTraceHand(
        this,
        fmt::format("in read_ya... need {}, have {}", std::size(peer_public_key), peer_io->read_buffer_size()));
    if (peer_io->read_buffer_size() < std::size(peer_public_key))
    {
        return ReadState::Later;
    }

    have_read_anything_from_peer_ = true;

    /* read the incoming peer's public key */
    peer_io->read_bytes(std::data(peer_public_key), std::size(peer_public_key));
    get_dh().setPeerPublicKey(peer_public_key);

    // everything received so far is Ya+PadA; haven't sent Yb and peer has not yet sent HASH('req1').
    // so throw away buffer, and do early exit check: we know it's not legit MSE if > max PadA
    pad_a_len_ = peer_io->read_buffer_size();
    if (pad_a_len_ > PadaMaxlen)
    {
        return done(false);
    }
    peer_io->read_buffer_discard(pad_a_len_);

    // send our public key to the peer
    tr_logAddTraceHand(this, "sending B->A: Diffie Hellman Yb, PadB");
    pad_b_len_ = send_public_key_and_pad<PadbMaxlen>(peer_io);

    set_state(State::AwaitingPadA);
    // LATER, not NOW: recv buffer was just drained and peer was blocking
    return ReadState::Later;
}

ReadState tr_handshake::read_pad_a(tr_peerIo* peer_io)
{
    // find the end of PadA by looking for HASH('req1', S)
    auto const needle = tr_sha1::digest("req1"sv, get_dh().secret());

    for (; pad_a_len_ <= PadaMaxlen; ++pad_a_len_)
    {
        static auto constexpr Needlen = std::size(needle);
        if (peer_io->read_buffer_size() < Needlen)
        {
            tr_logAddTraceHand(
                this,
                fmt::format("in read_pad_a... need {}, read {}, have {}", Needlen, pad_a_len_, peer_io->read_buffer_size()));
            return ReadState::Later;
        }

        if (peer_io->read_buffer_starts_with(needle))
        {
            tr_logAddTraceHand(this, "found HASH('req1', S)!");
            peer_io->read_buffer_discard(Needlen);
            set_state(State::AwaitingCryptoProvide);
            return ReadState::Now;
        }

        peer_io->read_buffer_discard(1U);
    }

    tr_logAddTraceHand(this, "couldn't find HASH('req1', S)");
    return done(false);
}

ReadState tr_handshake::read_crypto_provide(tr_peerIo* peer_io)
{
    /* HASH('req2', SKEY) xor HASH('req3', S), ENCRYPT(VC, crypto_provide, len(PadC)) */
    auto x_or = tr_sha1_digest_t{};
    static auto constexpr Needlen = std::size(x_or) + /* HASH('req2', SKEY) xor HASH('req3', S) */
        std::size(VC) + sizeof(crypto_provide_) + sizeof(pad_c_len_);

    if (peer_io->read_buffer_size() < Needlen)
    {
        return ReadState::Later;
    }

    /* This next piece is HASH('req2', SKEY) xor HASH('req3', S) ...
     * we can get the first half of that (the obfuscatedTorrentHash)
     * by building the latter and xor'ing it with what the peer sent us */
    tr_logAddTraceHand(this, "reading obfuscated torrent hash...");
    peer_io->read_bytes(std::data(x_or), std::size(x_or));

    auto obfuscated_hash = tr_sha1_digest_t{};
    auto const req3 = tr_sha1::digest("req3"sv, get_dh().secret());
    for (size_t i = 0; i < std::size(obfuscated_hash); ++i)
    {
        obfuscated_hash[i] = x_or[i] ^ req3[i];
    }

    if (auto const info = mediator_->torrent_from_obfuscated(obfuscated_hash); info)
    {
        tr_logAddTraceHand(this, fmt::format("got INCOMING connection's MSE handshake for torrent [{}]", info->id));
        peer_io->set_torrent_hash(info->info_hash);
    }
    else
    {
        tr_logAddTraceHand(this, "can't find that torrent...");
        return done(false);
    }

    /* next part: ENCRYPT(VC, crypto_provide, len(PadC), */
    auto const& info_hash = peer_io->torrent_hash();
    TR_ASSERT_MSG(info_hash != tr_sha1_digest_t{}, "read_crypto_provide requires an info_hash");
    peer_io->decrypt_init(peer_io->is_incoming(), get_dh(), info_hash);

    auto vc_in = vc_t{};
    peer_io->read_bytes(std::data(vc_in), std::size(vc_in));
    if (vc_in != VC)
    {
        tr_logAddTraceHand(this, "peer's VC is not all 0");
        return done(false);
    }

    peer_io->read_uint32(&crypto_provide_);
    tr_logAddTraceHand(this, fmt::format("crypto_provide is {}", crypto_provide_));

    peer_io->read_uint16(&pad_c_len_);
    tr_logAddTraceHand(this, fmt::format("len(PadC) is {}", pad_c_len_));
    if (pad_c_len_ > PadcMaxlen)
    {
        tr_logAddTraceHand(this, "peer's PadC is too big");
        return done(false);
    }

    set_state(State::AwaitingPadC);
    return ReadState::Now;
}

ReadState tr_handshake::read_pad_c(tr_peerIo* peer_io)
{
    if (auto const needlen = pad_c_len_ + sizeof(ia_len_); peer_io->read_buffer_size() < needlen)
    {
        return ReadState::Later;
    }

    // read the throwaway padc
    peer_io->read_buffer_discard(pad_c_len_);

    /* read ia_len */
    peer_io->read_uint16(&ia_len_);
    tr_logAddTraceHand(this, fmt::format("len(IA) is {}", ia_len_));
    set_state(State::AwaitingIa);
    return ReadState::Now;
}

ReadState tr_handshake::read_ia(tr_peerIo* peer_io)
{
    size_t const needlen = ia_len_;

    tr_logAddTraceHand(this, fmt::format("reading IA... have {}, need {}", peer_io->read_buffer_size(), needlen));

    if (peer_io->read_buffer_size() < needlen)
    {
        return ReadState::Later;
    }

    // B->A: ENCRYPT(VC, crypto_select, len(padD), padD), ENCRYPT2(Payload Stream)
    auto const& info_hash = peer_io->torrent_hash();
    TR_ASSERT_MSG(info_hash != tr_sha1_digest_t{}, "read_ia requires an info_hash");

    static auto constexpr BufSize = std::size(VC) + sizeof(crypto_select_) + sizeof(pad_d_len_) + HandshakeSize;
    auto outbuf = libtransmission::StackBuffer<BufSize, std::byte>{};
    peer_io->encrypt_init(peer_io->is_incoming(), get_dh(), info_hash);

    // send VC
    tr_logAddTraceHand(this, "sending vc");
    outbuf.add(VC);

    /* send crypto_select */
    crypto_select_ = get_crypto_select(encryption_mode_, crypto_provide_);
    if (crypto_select_ != 0U)
    {
        tr_logAddTraceHand(this, fmt::format("selecting crypto mode '{}'", crypto_select_));
        outbuf.add_uint32(crypto_select_);
    }
    else
    {
        tr_logAddTraceHand(this, "peer didn't offer an encryption mode we like.");
        return done(false);
    }

    tr_logAddTraceHand(this, "sending pad d");

    /* ENCRYPT(VC, crypto_select, len(PadD), PadD
     * PadD is reserved for future extensions to the handshake...
     * standard practice at this time is for it to be zero-length */
    outbuf.add_uint16(0U);

    // send it
    peer_io->write(outbuf, false);

    /* maybe de-encrypt our connection */
    if (crypto_select_ == CryptoProvidePlaintext)
    {
        TR_ASSERT(std::empty(outbuf));

        // All future communications will use ENCRYPT2()
        peer_io->encrypt_disable();
        peer_io->decrypt_disable(ia_len_);
    }

    /* now await the handshake */
    set_state(State::AwaitingHandshake);
    return ReadState::Now;
}

// ---

ReadState tr_handshake::can_read(tr_peerIo* peer_io, void* vhandshake, size_t* piece)
{
    auto* handshake = static_cast<tr_handshake*>(vhandshake);

    /* no piece data in handshake */
    *piece = 0;

    tr_logAddTraceHand(handshake, fmt::format("handling can_read; state is [{}]", handshake->state_string()));

    auto ret = ReadState::Now;
    while (ret == ReadState::Now)
    {
        switch (handshake->state())
        {
        case State::AwaitingHandshake:
            ret = handshake->read_handshake(peer_io);
            break;

        case State::AwaitingPeerId:
            ret = handshake->read_peer_id(peer_io);
            break;

        case State::AwaitingYa:
            ret = handshake->read_ya(peer_io);
            break;

        case State::AwaitingPadA:
            ret = handshake->read_pad_a(peer_io);
            break;

        case State::AwaitingCryptoProvide:
            ret = handshake->read_crypto_provide(peer_io);
            break;

        case State::AwaitingPadC:
            ret = handshake->read_pad_c(peer_io);
            break;

        case State::AwaitingIa:
            ret = handshake->read_ia(peer_io);
            break;

        case State::AwaitingYb:
            ret = handshake->read_yb(peer_io);
            break;

        case State::AwaitingVc:
            ret = handshake->read_vc(peer_io);
            break;

        case State::AwaitingCryptoSelect:
            ret = handshake->read_crypto_select(peer_io);
            break;

        case State::AwaitingPadD:
            ret = handshake->read_pad_d(peer_io);
            break;

        default:
            TR_ASSERT_MSG(false, fmt::format("unhandled handshake state {:d}", static_cast<int>(handshake->state())));
            return ReadState::Err;
        }
    }

    return ret;
}

void tr_handshake::on_error(tr_peerIo* io, tr_error const& error, void* vhandshake)
{
    auto* handshake = static_cast<tr_handshake*>(vhandshake);

    auto const retry_plain = [&]()
    {
        handshake->send_handshake(io);
        handshake->set_state(State::AwaitingHandshake);
    };
    auto const fail = [&]()
    {
        tr_logAddTraceHand(handshake, fmt::format("handshake socket err: {:s} ({:d})", error.message(), error.code()));
        handshake->done(false);
    };

    handshake->maybe_recycle_dh();

    if (io->is_utp() && !io->is_incoming() && handshake->is_state(State::AwaitingYb))
    {
        // the peer probably doesn't speak µTP.

        auto const info_hash = io->torrent_hash();
        auto const info = handshake->mediator_->torrent(info_hash);

        /* Don't mark a peer as non-µTP unless it's really a connect failure. */
        if ((error.code() == ETIMEDOUT || error.code() == ECONNREFUSED) && info)
        {
            handshake->mediator_->set_utp_failed(info_hash, io->socket_address());
        }

        if (handshake->mediator_->allows_tcp() && io->reconnect())
        {
            tr_logAddTraceHand(handshake, "uTP connection failed, trying TCP...");
            if (handshake->encryption_mode_ != TR_CLEAR_PREFERRED)
            {
                handshake->send_ya(io);
            }
            else
            {
                retry_plain();
            }
            return;
        }

        fail();
        return;
    }

    /* if the error happened while we were sending a public key, we might
     * have encountered a peer that doesn't do encryption... reconnect and
     * try a plaintext handshake */
    if (handshake->is_state(State::AwaitingYb) && handshake->encryption_mode_ != TR_ENCRYPTION_REQUIRED &&
        handshake->mediator_->allows_tcp() && io->reconnect())
    {
        tr_logAddTraceHand(handshake, "MSE handshake failed, trying plaintext...");
        retry_plain();
        return;
    }

    fail();
}

// ---

bool tr_handshake::build_handshake_message(tr_peerIo* io, libtransmission::BufferWriter<std::byte>& buf) const
{
    auto const& info_hash = io->torrent_hash();
    TR_ASSERT_MSG(info_hash != tr_sha1_digest_t{}, "build_handshake_message requires an info_hash");

    auto const info = mediator_->torrent(info_hash);
    if (!info)
    {
        return false;
    }

    auto flags = tr_bitfield{ HandshakeFlagsBits };
    flags.set(LtepFlag);
    flags.set(FextFlag);
    if (mediator_->allows_dht())
    {
        flags.set(DhtFlag);
    }
    auto const flag_bytes = flags.raw();

    buf.add(HandshakeName);
    buf.add(flag_bytes);
    buf.add(info_hash);
    buf.add(info->client_peer_id);

    return true;
}

bool tr_handshake::send_handshake(tr_peerIo* io)
{
    auto msg = libtransmission::StackBuffer<HandshakeSize, std::byte>{};
    if (!build_handshake_message(io, msg))
    {
        return false;
    }
    TR_ASSERT(std::size(msg) == HandshakeSize);
    io->write(msg, false);
    have_sent_bittorrent_handshake_ = true;
    return true;
}

uint32_t tr_handshake::crypto_provide() const noexcept
{
    auto provide = uint32_t{};

    switch (encryption_mode_)
    {
    case TR_ENCRYPTION_REQUIRED:
    case TR_ENCRYPTION_PREFERRED:
        provide |= CryptoProvideRC4;
        break;

    case TR_CLEAR_PREFERRED:
        provide |= CryptoProvideRC4 | CryptoProvidePlaintext;
        break;
    }

    return provide;
}

[[nodiscard]] uint32_t tr_handshake::get_crypto_select(tr_encryption_mode encryption_mode, uint32_t crypto_provide) noexcept
{
    auto choices = std::array<uint32_t, 2>{};
    int n_choices = 0;

    switch (encryption_mode)
    {
    case TR_ENCRYPTION_REQUIRED:
        choices[n_choices++] = CryptoProvideRC4;
        break;

    case TR_ENCRYPTION_PREFERRED:
        choices[n_choices++] = CryptoProvideRC4;
        choices[n_choices++] = CryptoProvidePlaintext;
        break;

    case TR_CLEAR_PREFERRED:
        choices[n_choices++] = CryptoProvidePlaintext;
        choices[n_choices++] = CryptoProvideRC4;
        break;
    }

    for (auto const& choice : choices)
    {
        if ((crypto_provide & choice) != 0)
        {
            return choice;
        }
    }

    return 0;
}

bool tr_handshake::fire_done(bool is_connected)
{
    peer_io_->clear_callbacks();
    maybe_recycle_dh();

    if (!on_done_)
    {
        return false;
    }

    // handshake could get destroyed inside on_done,
    // so handle all our housekeeping *before* calling it

    auto cb = DoneFunc{};
    std::swap(cb, on_done_);

    return (cb)(Result{ peer_io_, peer_id_, have_read_anything_from_peer_, is_connected });
}

void tr_handshake::fire_timer()
{
    tr_logAddTraceHand(this, "timer expired");
    fire_done(false);
}

std::string_view tr_handshake::state_string(State state) noexcept
{
    switch (state)
    {
    case State::AwaitingHandshake:
        return "awaiting handshake";
    case State::AwaitingPeerId:
        return "awaiting peer id";
    case State::AwaitingYa:
        return "awaiting ya";
    case State::AwaitingPadA:
        return "awaiting pad a";
    case State::AwaitingCryptoProvide:
        return "awaiting crypto provide";
    case State::AwaitingPadC:
        return "awaiting pad c";
    case State::AwaitingIa:
        return "awaiting ia";

    // outgoing
    case State::AwaitingYb:
        return "awaiting yb";
    case State::AwaitingVc:
        return "awaiting vc";
    case State::AwaitingCryptoSelect:
        return "awaiting crypto select";
    case State::AwaitingPadD:
        return "awaiting pad d";
    }

    return "unknown state";
}

tr_handshake::tr_handshake(Mediator* mediator, std::shared_ptr<tr_peerIo> peer_io, tr_encryption_mode mode, DoneFunc on_done)
    : on_done_{ std::move(on_done) }
    , peer_io_{ std::move(peer_io) }
    , timeout_timer_{ mediator->timer_maker().create([this]() { fire_timer(); }) }
    , mediator_{ mediator }
    , encryption_mode_{ mode }
{
    timeout_timer_->start_single_shot(HandshakeTimeoutSec);

    peer_io_->set_callbacks(&tr_handshake::can_read, nullptr, &tr_handshake::on_error, this);

    if (is_incoming())
    {
        set_state(State::AwaitingYa);
    }
    else if (encryption_mode_ != TR_CLEAR_PREFERRED)
    {
        send_ya(peer_io_.get());
    }
    else
    {
        tr_logAddTraceHand(this, "sending plain handshake");
        send_handshake(peer_io_.get());
        set_state(State::AwaitingHandshake);
    }
}
