// This file Copyright (C) 2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef> // size_t, std::byte
#include <cstdint> // uint8_t
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <unistd.h> // write()
#endif

#include <event2/util.h>

#include <libtransmission/transmission.h>

#include <libtransmission/crypto-utils.h> // tr_sha1_to_string, tr_base...
#include <libtransmission/handshake.h>
#include <libtransmission/net.h>
#include <libtransmission/peer-io.h>
#include <libtransmission/peer-mse.h>
#include <libtransmission/peer-socket.h>
#include <libtransmission/session.h> // tr_peerIdInit()
#include <libtransmission/timer.h>
#include <libtransmission/tr-assert.h>
#include <libtransmission/tr-macros.h>
#include <libtransmission/utils.h>

#include "gtest/gtest.h"
#include "test-fixtures.h"

using namespace std::literals;

#ifdef _WIN32
#define LOCAL_SOCKETPAIR_AF AF_INET
#else
#define LOCAL_SOCKETPAIR_AF AF_UNIX
#endif

namespace libtransmission::test
{

auto constexpr MaxWaitMsec = 5000;

class HandshakeTest : public SessionTest
{
public:
    class MediatorMock final : public tr_handshake::Mediator
    {
    public:
        explicit MediatorMock(tr_session* session)
            : session_{ session }
        {
        }

        [[nodiscard]] std::optional<TorrentInfo> torrent(tr_sha1_digest_t const& info_hash) const override
        {
            if (auto const iter = torrents.find(info_hash); iter != std::end(torrents))
            {
                return iter->second;
            }

            return {};
        }

        [[nodiscard]] std::optional<TorrentInfo> torrent_from_obfuscated(tr_sha1_digest_t const& obfuscated) const override
        {
            for (auto const& [info_hash, info] : torrents)
            {
                if (obfuscated == tr_sha1::digest("req2"sv, info.info_hash))
                {
                    return info;
                }
            }

            return {};
        }

        [[nodiscard]] libtransmission::TimerMaker& timer_maker() override
        {
            return session_->timerMaker();
        }

        [[nodiscard]] bool allows_dht() const override
        {
            return false;
        }

        [[nodiscard]] bool allows_tcp() const override
        {
            return true;
        }

        [[nodiscard]] size_t pad(void* setme, [[maybe_unused]] size_t maxlen) const override
        {
            TR_ASSERT(maxlen > 10);
            auto const len = size_t{ 10 };
            std::fill_n(static_cast<char*>(setme), 10, ' ');
            return len;
        }

        [[nodiscard]] tr_message_stream_encryption::DH::private_key_bigend_t private_key() const override
        {
            return private_key_;
        }

        void set_utp_failed(tr_sha1_digest_t const& /*info_hash*/, tr_socket_address const& /*socket_address*/) override
        {
        }

        void setPrivateKeyFromBase64(std::string_view b64)
        {
            auto const str = tr_base64_decode(b64);
            assert(std::size(str) == std::size(private_key_));
            std::copy_n(reinterpret_cast<std::byte const*>(std::data(str)), std::size(str), std::begin(private_key_));
        }

        tr_session* const session_;
        std::map<tr_sha1_digest_t, TorrentInfo> torrents;
        tr_message_stream_encryption::DH::private_key_bigend_t private_key_ = {};
    };

    template<typename Span>
    void sendToClient(evutil_socket_t sock, Span const& data)
    {
        auto const* walk = std::data(data);
        static_assert(sizeof(*walk) == 1);
        size_t len = std::size(data);

        while (len > 0)
        {
#if defined(_WIN32)
            auto const n = send(sock, reinterpret_cast<char const*>(walk), len, 0);
#else
            auto const n = write(sock, walk, len);
#endif
            assert(n >= 0);
            len -= n;
            walk += n;
        }
    }

    void sendB64ToClient(evutil_socket_t sock, std::string_view b64)
    {
        sendToClient(sock, tr_base64_decode(b64));
    }

    static auto constexpr ReservedBytesNoExtensions = std::array<uint8_t, 8>{ 0, 0, 0, 0, 0, 0, 0, 0 };
    static auto constexpr PlaintextProtocolName = "\023BitTorrent protocol"sv;

    tr_socket_address const DefaultPeerSockAddr{ *tr_address::from_string("127.0.0.1"sv), tr_port::from_host(8080) };
    tr_handshake::Mediator::TorrentInfo const TorrentWeAreSeeding{
        .info_hash = tr_sha1::digest("abcde"sv),
        .client_peer_id = tr_peerIdInit(),
        .id = tr_torrent_id_t{ 100 },
        .is_done = true,
    };
    tr_handshake::Mediator::TorrentInfo const UbuntuTorrent{
        .info_hash = *tr_sha1_from_string("2c6b6858d61da9543d4231a71db4b1c9264b0685"sv),
        .client_peer_id = tr_peerIdInit(),
        .id = tr_torrent_id_t{ 101 },
        .is_done = false,
    };

    auto createIncomingIo(tr_session* session)
    {
        auto sockpair = std::array<evutil_socket_t, 2>{ -1, -1 };
        EXPECT_EQ(0, evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, std::data(sockpair))) << tr_strerror(errno);
        return std::pair{ tr_peerIo::new_incoming(
                              session,
                              &session->top_bandwidth_,
                              tr_peer_socket(session, DefaultPeerSockAddr, sockpair[0])),
                          sockpair[1] };
    }

    auto createOutgoingIo(tr_session* session, tr_sha1_digest_t const& info_hash)
    {
        auto sockpair = std::array<evutil_socket_t, 2>{ -1, -1 };
        EXPECT_EQ(0, evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, std::data(sockpair))) << tr_strerror(errno);
        auto peer_io = tr_peerIo::create(session, &session->top_bandwidth_, &info_hash, false /*incoming*/, false /*seed*/);
        peer_io->set_socket(tr_peer_socket(session, DefaultPeerSockAddr, sockpair[0]));
        return std::pair{ std::move(peer_io), sockpair[1] };
    }

    static constexpr auto makePeerId(std::string_view sv)
    {
        auto peer_id = tr_peer_id_t{};
        for (size_t i = 0, n = std::size(sv); i < n; ++i)
        {
            peer_id[i] = sv[i];
        }
        return peer_id;
    }

    static auto makeRandomPeerId()
    {
        auto peer_id = tr_rand_obj<tr_peer_id_t>();
        auto const peer_id_prefix = "-UW110Q-"sv;
        std::copy(std::begin(peer_id_prefix), std::end(peer_id_prefix), std::begin(peer_id));
        return peer_id;
    }

    static auto runHandshake(
        tr_handshake::Mediator* mediator,
        std::shared_ptr<tr_peerIo> const& peer_io,
        tr_encryption_mode encryption_mode = TR_CLEAR_PREFERRED)
    {
        auto result = std::optional<tr_handshake::Result>{};

        auto handshake = tr_handshake{ mediator,
                                       peer_io,
                                       encryption_mode,
                                       [&result](auto const& resin)
                                       {
                                           result = resin;
                                           return true;
                                       } };

        waitFor([&result]() { return result.has_value(); }, MaxWaitMsec);
        return result;
    }
};

TEST_F(HandshakeTest, incomingPlaintext)
{
    auto const peer_id = makeRandomPeerId();
    auto mediator = MediatorMock{ session_ };
    mediator.torrents.emplace(TorrentWeAreSeeding.info_hash, TorrentWeAreSeeding);

    // The simplest handshake there is. "The handshake starts with character
    // nineteen (decimal) followed by the string 'BitTorrent protocol'.
    // The leading character is a length prefix[.]. After the fixed headers
    // come eight reserved bytes, which are all zero in all current
    // implementations[.] Next comes the 20 byte sha1 hash of the bencoded
    // form of the info value from the metainfo file[.] After the download
    // hash comes the 20-byte peer id which is reported in tracker requests
    // and contained in peer lists in tracker responses."
    // https://www.bittorrent.org/beps/bep_0052.html
    auto [io, sock] = createIncomingIo(session_);
    sendToClient(sock, PlaintextProtocolName);
    sendToClient(sock, ReservedBytesNoExtensions);
    sendToClient(sock, TorrentWeAreSeeding.info_hash);
    sendToClient(sock, peer_id);

    auto const res = runHandshake(&mediator, io);

    // check the results
    EXPECT_TRUE(res.has_value());
    assert(res.has_value());
    EXPECT_TRUE(res->is_connected);
    EXPECT_TRUE(res->read_anything_from_peer);
    EXPECT_EQ(io, res->io);
    EXPECT_TRUE(res->peer_id);
    EXPECT_EQ(peer_id, res->peer_id);
    EXPECT_EQ(TorrentWeAreSeeding.info_hash, io->torrent_hash());

    tr_net_close_socket(sock);
}

// The datastream is identical to HandshakeTest.incomingPlaintext,
// but this time we don't recognize the infohash sent by the peer.
TEST_F(HandshakeTest, incomingPlaintextUnknownInfoHash)
{
    auto mediator = MediatorMock{ session_ };
    mediator.torrents.emplace(TorrentWeAreSeeding.info_hash, TorrentWeAreSeeding);

    auto [io, sock] = createIncomingIo(session_);
    sendToClient(sock, PlaintextProtocolName);
    sendToClient(sock, ReservedBytesNoExtensions);
    sendToClient(sock, tr_sha1::digest("some other torrent unknown to us"sv));
    sendToClient(sock, makeRandomPeerId());

    auto const res = runHandshake(&mediator, io);

    // check the results
    EXPECT_TRUE(res.has_value());
    assert(res.has_value());
    EXPECT_FALSE(res->is_connected);
    EXPECT_TRUE(res->read_anything_from_peer);
    EXPECT_EQ(io, res->io);
    EXPECT_FALSE(res->peer_id);
    EXPECT_EQ(tr_sha1_digest_t{}, io->torrent_hash());

    tr_net_close_socket(sock);
}

TEST_F(HandshakeTest, outgoingPlaintext)
{
    auto const peer_id = makeRandomPeerId();
    auto mediator = MediatorMock{ session_ };
    mediator.torrents.emplace(UbuntuTorrent.info_hash, TorrentWeAreSeeding);

    auto [io, sock] = createOutgoingIo(session_, UbuntuTorrent.info_hash);
    sendToClient(sock, PlaintextProtocolName);
    sendToClient(sock, ReservedBytesNoExtensions);
    sendToClient(sock, UbuntuTorrent.info_hash);
    sendToClient(sock, peer_id);

    auto const res = runHandshake(&mediator, io);

    // check the results
    EXPECT_TRUE(res.has_value());
    assert(res.has_value());
    EXPECT_TRUE(res->is_connected);
    EXPECT_TRUE(res->read_anything_from_peer);
    EXPECT_EQ(io, res->io);
    EXPECT_TRUE(res->peer_id);
    EXPECT_EQ(peer_id, res->peer_id);
    EXPECT_EQ(UbuntuTorrent.info_hash, io->torrent_hash());
    EXPECT_EQ(tr_sha1_to_string(UbuntuTorrent.info_hash), tr_sha1_to_string(io->torrent_hash()));

    tr_net_close_socket(sock);
}

TEST_F(HandshakeTest, incomingEncrypted)
{
    static auto constexpr ExpectedPeerId = makePeerId("-TR300Z-w4bd4mkebkbi"sv);

    auto mediator = MediatorMock{ session_ };
    mediator.torrents.emplace(UbuntuTorrent.info_hash, UbuntuTorrent);
    mediator.setPrivateKeyFromBase64("0EYKCwBWQ4Dg9kX3c5xxjVtBDKw="sv);

    auto [io, sock] = createIncomingIo(session_);

    // Peer->Client data from a successful encrypted handshake recorded
    // in the wild for replay here
    sendB64ToClient(
        sock,
        "svkySIFcCsrDTeHjPt516UFbsoR+5vfbe5/m6stE7u5JLZ10kJ19NmP64E10qI"
        "nn78sCrJgjw1yEHHwrzOcKiRlYvcMotzJMe+SjrFUnaw3KBfn2bcKBhxb/sfM9"
        "J7nJ"sv);
    sendB64ToClient(
        sock,
        "ICAgICAgICAgIKdr4jIBZ4xFfO4xNiRV7Gl2azTSuTFuu06NU1WyRPif018JYe"
        "VGwrTPstEPu3V5lmzjtMGVLaL5EErlpJ93Xrz+ea6EIQEUZA+D4jKaV/to9NVi"
        "04/1W1A2PHgg+I9puac/i9BsFPcjdQeoVtU73lNCbTDQgTieyjDWmwo="sv);

    auto const res = runHandshake(&mediator, io);

    // check the results
    EXPECT_TRUE(res.has_value());
    assert(res.has_value());
    EXPECT_TRUE(res->is_connected);
    EXPECT_TRUE(res->read_anything_from_peer);
    EXPECT_EQ(io, res->io);
    EXPECT_TRUE(res->peer_id);
    EXPECT_EQ(ExpectedPeerId, res->peer_id);
    EXPECT_EQ(UbuntuTorrent.info_hash, io->torrent_hash());
    EXPECT_EQ(tr_sha1_to_string(UbuntuTorrent.info_hash), tr_sha1_to_string(io->torrent_hash()));

    tr_net_close_socket(sock);
}

// The datastream is identical to HandshakeTest.incomingEncrypted,
// but this time we don't recognize the infohash sent by the peer.
TEST_F(HandshakeTest, incomingEncryptedUnknownInfoHash)
{
    auto mediator = MediatorMock{ session_ };
    mediator.setPrivateKeyFromBase64("0EYKCwBWQ4Dg9kX3c5xxjVtBDKw="sv);

    auto [io, sock] = createIncomingIo(session_);

    // Peer->Client data from a successful encrypted handshake recorded
    // in the wild for replay here
    sendB64ToClient(
        sock,
        "svkySIFcCsrDTeHjPt516UFbsoR+5vfbe5/m6stE7u5JLZ10kJ19NmP64E10qI"
        "nn78sCrJgjw1yEHHwrzOcKiRlYvcMotzJMe+SjrFUnaw3KBfn2bcKBhxb/sfM9"
        "J7nJ"sv);
    sendB64ToClient(
        sock,
        "ICAgICAgICAgIKdr4jIBZ4xFfO4xNiRV7Gl2azTSuTFuu06NU1WyRPif018JYe"
        "VGwrTPstEPu3V5lmzjtMGVLaL5EErlpJ93Xrz+ea6EIQEUZA+D4jKaV/to9NVi"
        "04/1W1A2PHgg+I9puac/i9BsFPcjdQeoVtU73lNCbTDQgTieyjDWmwo="sv);

    auto const res = runHandshake(&mediator, io);

    // check the results
    EXPECT_TRUE(res.has_value());
    assert(res.has_value());
    EXPECT_FALSE(res->is_connected);
    EXPECT_TRUE(res->read_anything_from_peer);
    EXPECT_EQ(tr_sha1_digest_t{}, io->torrent_hash());

    tr_net_close_socket(sock);
}

TEST_F(HandshakeTest, outgoingEncrypted)
{
    static auto constexpr ExpectedPeerId = makePeerId("-qB4250-scysDI_JuVN3"sv);

    auto mediator = MediatorMock{ session_ };
    mediator.torrents.emplace(UbuntuTorrent.info_hash, UbuntuTorrent);
    mediator.setPrivateKeyFromBase64("0EYKCwBWQ4Dg9kX3c5xxjVtBDKw="sv);

    auto [io, sock] = createOutgoingIo(session_, UbuntuTorrent.info_hash);

    // Peer->Client data from a successful encrypted handshake recorded
    // in the wild for replay here
    sendB64ToClient(
        sock,
        "Sfgoq/nrQfD4Iwirfk+uhOmQMOC/QwK/vYiOact1NF9TpWXms3cvlKEKxs0VU"
        "mnmytRh9bh4Lcs1bswlC6R05XrJGzLhZqAqcLUUAR1VTLA5oKSjR1038zFbhn"
        "c71jqlpney15ChMTnx02Qt+88l0Z9OWLUUJrUVy+OoIaTMSKDDFVOjuj0y+Ii"
        "cE0ZnN61e0/R/g+APRK5tegw0SLZ3Nr8+y4Dl77sZyc141PR9xvDj0da1eAvf"
        "BvXyyDem4vUjqiLUNCEV8KDXEMPCPYAQoDZzLvMyOEtJM/if0o0UN88SWtt1k"
        "jRD8UNvUlXIfM0YsnJhKA6fJ7/4geK7+Wo2aicfaLFOyG5IEJbTg9OQYbDHFa"
        "oVzD0xY0Dx+J0loqM+CzrPj8UpeXIcbD7pJrT3XPECbFQ12cCY5LW5RymVIx8"
        "TP0ajGiTxou1L7DbGD54SYgV/4qFbafRsWp9AO+YDJcouFd/jiVN+r3loxvfT"
        "0A9H9DRAMR0rZKpQpXZ1ZAhAuAOXGHFIvtw8wd6dPybeu5+LoR2S90/IpwHWI"
        "jbNbypQZuA9hn4JfFMWPP9TG/E11loB4+MkrP22U72ezjL5ipd74AEEP0/u8w"
        "Gj1t2kXhND9ONfasA+pY25y8GM04M0B7+0xKmsHP7tntwQLAGZATH83rOxaSO"
        "3+o/RdiKQJAsGxMIU08scBc5VOmrAmjeYrLNpFnpXVuavH5if7490zMCu3DEn"
        "G9hpbYbiX95T+EUcRbM6pSCvr3Twq1Q="sv);

    auto const res = runHandshake(&mediator, io, TR_ENCRYPTION_PREFERRED);

    // check the results
    EXPECT_TRUE(res.has_value());
    assert(res.has_value());
    EXPECT_TRUE(res->is_connected);
    EXPECT_TRUE(res->read_anything_from_peer);
    EXPECT_EQ(io, res->io);
    EXPECT_TRUE(res->peer_id);
    EXPECT_EQ(ExpectedPeerId, res->peer_id);
    EXPECT_EQ(UbuntuTorrent.info_hash, io->torrent_hash());
    EXPECT_EQ(tr_sha1_to_string(UbuntuTorrent.info_hash), tr_sha1_to_string(io->torrent_hash()));

    tr_net_close_socket(sock);
}

} // namespace libtransmission::test
