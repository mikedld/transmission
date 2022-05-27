// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

/**
***
**/

#include <cstddef> // size_t
#include <cstdint> // uintX_t
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <utility> // std::make_pair

#include <event2/buffer.h>

#include "transmission.h"

#include "bandwidth.h"
#include "crypto.h"
#include "net.h" // tr_address
#include "peer-socket.h"
#include "tr-assert.h"

class tr_peerIo;
struct Bandwidth;
struct tr_datatype;

/**
 * @addtogroup networked_io Networked IO
 * @{
 */

enum ReadState
{
    READ_NOW,
    READ_LATER,
    READ_ERR
};

enum tr_encryption_type
{
    /* these match the values in MSE's crypto_select */
    PEER_ENCRYPTION_NONE = (1 << 0),
    PEER_ENCRYPTION_RC4 = (1 << 1)
};

using tr_can_read_cb = ReadState (*)(tr_peerIo* io, void* user_data, size_t* setme_piece_byte_count);

using tr_did_write_cb = void (*)(tr_peerIo* io, size_t bytesWritten, bool wasPieceData, void* userData);

using tr_net_error_cb = void (*)(tr_peerIo* io, short what, void* userData);

auto inline constexpr PEER_IO_MAGIC_NUMBER = 206745;

struct evbuffer_deleter
{
    void operator()(struct evbuffer* buf) const noexcept
    {
        evbuffer_free(buf);
    }
};

using tr_evbuffer_ptr = std::unique_ptr<evbuffer, evbuffer_deleter>;

class tr_peerIo
{
public:
    tr_peerIo(
        tr_session* session_in,
        tr_sha1_digest_t const* torrent_hash,
        bool is_incoming,
        tr_address const& addr,
        tr_port port,
        bool is_seed,
        time_t current_time)
        : crypto{ torrent_hash, is_incoming }
        , session{ session_in }
        , time_created{ current_time }
        , addr_{ addr }
        , port_{ port }
        , is_seed_{ is_seed }
    {
    }

    [[nodiscard]] constexpr tr_address const& address() const noexcept
    {
        return addr_;
    }

    [[nodiscard]] constexpr std::pair<tr_address, tr_port> socketAddress() const noexcept
    {
        return std::make_pair(addr_, port_);
    }

    std::string addrStr() const;

    [[nodiscard]] constexpr bool isIncoming() noexcept
    {
        return crypto.is_incoming;
    }

    [[nodiscard]] auto getReadBuffer() noexcept
    {
        return inbuf.get();
    }

    [[nodiscard]] auto hasBandwidthLeft(tr_direction dir) noexcept
    {
        return bandwidth->clamp(dir, 1024) > 0;
    }

    [[nodiscard]] auto getPieceSpeed_Bps(uint64_t now, tr_direction dir) noexcept
    {
        return bandwidth->getPieceSpeedBytesPerSecond(now, dir);
    }

    constexpr void enableFEXT(bool flag) noexcept
    {
        fast_extension_supported_ = flag;
    }

    [[nodiscard]] constexpr auto supportsFEXT() const noexcept
    {
        return fast_extension_supported_;
    }

    constexpr void enableLTEP(bool flag) noexcept
    {
        extended_protocol_supported_ = flag;
    }

    [[nodiscard]] constexpr auto supportsLTEP() const noexcept
    {
        return extended_protocol_supported_;
    }

    constexpr void enableDHT(bool flag) noexcept
    {
        dht_supported_ = flag;
    }

    [[nodiscard]] constexpr auto supportsDHT() const noexcept
    {
        return dht_supported_;
    }

    [[nodiscard]] constexpr auto supportsUTP() const noexcept
    {
        return utp_supported_;
    }

    [[nodiscard]] constexpr auto isSeed() const noexcept
    {
        return is_seed_;
    }

    tr_crypto crypto;

    // TODO(ckerr): yikes, unlike other class' magic_numbers it looks
    // like this one isn't being used just for assertions, but also in
    // didWriteWrapper() to see if the tr_peerIo got freed during the
    // notify-consumed events. Fix this before removing this field.
    int magic_number = PEER_IO_MAGIC_NUMBER;

    struct tr_peer_socket socket = {};

    tr_session* const session;

    time_t const time_created;

    tr_can_read_cb canRead = nullptr;
    tr_did_write_cb didWrite = nullptr;
    tr_net_error_cb gotError = nullptr;
    void* userData = nullptr;

    // Changed to non-owning pointer temporarily till tr_peerIo becomes C++-constructible and destructible
    // TODO: change tr_bandwidth* to owning pointer to the bandwidth, or remove * and own the value
    Bandwidth* bandwidth = nullptr;

    tr_evbuffer_ptr const inbuf = tr_evbuffer_ptr{ evbuffer_new() };
    tr_evbuffer_ptr const outbuf = tr_evbuffer_ptr{ evbuffer_new() };

    struct tr_datatype* outbuf_datatypes = nullptr;

    struct event* event_read = nullptr;
    struct event* event_write = nullptr;

    // TODO(ckerr): this could be narrowed to 1 byte
    tr_encryption_type encryption_type = PEER_ENCRYPTION_NONE;

    // TODO: use std::shared_ptr instead of manual refcounting?
    int refCount = 1;

    short int pendingEvents = 0;

    tr_priority_t priority = TR_PRI_NORMAL;

    bool utp_supported_ = false;

private:
    tr_address const addr_;
    tr_port const port_;

    bool const is_seed_;

    bool dht_supported_ = false;
    bool extended_protocol_supported_ = false;
    bool fast_extension_supported_ = false;
};

/**
***
**/

// TODO: 8 constructor args is too many; maybe a builder object?
tr_peerIo* tr_peerIoNewOutgoing(
    tr_session* session,
    Bandwidth* parent,
    struct tr_address const* addr,
    tr_port port,
    time_t current_time,
    tr_sha1_digest_t const& torrent_hash,
    bool is_seed,
    bool utp);

tr_peerIo* tr_peerIoNewIncoming(
    tr_session* session,
    Bandwidth* parent,
    struct tr_address const* addr,
    tr_port port,
    time_t current_time,
    struct tr_peer_socket const socket);

void tr_peerIoRefImpl(char const* file, int line, tr_peerIo* io);

#define tr_peerIoRef(io) tr_peerIoRefImpl(__FILE__, __LINE__, (io))

void tr_peerIoUnrefImpl(char const* file, int line, tr_peerIo* io);

#define tr_peerIoUnref(io) tr_peerIoUnrefImpl(__FILE__, __LINE__, (io))

constexpr bool tr_isPeerIo(tr_peerIo const* io)
{
    return io != nullptr && io->magic_number == PEER_IO_MAGIC_NUMBER && io->refCount >= 0 &&
        tr_address_is_valid(&io->address());
}

/**
***
**/

constexpr tr_session* tr_peerIoGetSession(tr_peerIo* io)
{
    TR_ASSERT(tr_isPeerIo(io));
    TR_ASSERT(io->session != nullptr);

    return io->session;
}

std::optional<tr_sha1_digest_t> tr_peerIoGetTorrentHash(tr_peerIo const* io);

void tr_peerIoSetTorrentHash(tr_peerIo* io, tr_sha1_digest_t const& info_hash);

int tr_peerIoReconnect(tr_peerIo* io);

/**
***
**/

void tr_peerIoSetIOFuncs(tr_peerIo* io, tr_can_read_cb readcb, tr_did_write_cb writecb, tr_net_error_cb errcb, void* user_data);

void tr_peerIoClear(tr_peerIo* io);

/**
***
**/

void tr_peerIoWriteBytes(tr_peerIo* io, void const* writeme, size_t writemeLen, bool isPieceData);

void tr_peerIoWriteBuf(tr_peerIo* io, struct evbuffer* buf, bool isPieceData);

/**
***
**/

constexpr tr_crypto* tr_peerIoGetCrypto(tr_peerIo* io)
{
    return &io->crypto;
}

void tr_peerIoSetEncryption(tr_peerIo* io, tr_encryption_type encryption_type);

constexpr bool tr_peerIoIsEncrypted(tr_peerIo const* io)
{
    return io != nullptr && io->encryption_type == PEER_ENCRYPTION_RC4;
}

void evbuffer_add_uint8(struct evbuffer* outbuf, uint8_t byte);
void evbuffer_add_uint16(struct evbuffer* outbuf, uint16_t hs);
void evbuffer_add_uint32(struct evbuffer* outbuf, uint32_t hl);
void evbuffer_add_uint64(struct evbuffer* outbuf, uint64_t hll);

static inline void evbuffer_add_hton_16(struct evbuffer* buf, uint16_t val)
{
    evbuffer_add_uint16(buf, val);
}

static inline void evbuffer_add_hton_32(struct evbuffer* buf, uint32_t val)
{
    evbuffer_add_uint32(buf, val);
}

static inline void evbuffer_add_hton_64(struct evbuffer* buf, uint64_t val)
{
    evbuffer_add_uint64(buf, val);
}

void tr_peerIoReadBytesToBuf(tr_peerIo* io, struct evbuffer* inbuf, struct evbuffer* outbuf, size_t byteCount);

void tr_peerIoReadBytes(tr_peerIo* io, struct evbuffer* inbuf, void* bytes, size_t byteCount);

static inline void tr_peerIoReadUint8(tr_peerIo* io, struct evbuffer* inbuf, uint8_t* setme)
{
    tr_peerIoReadBytes(io, inbuf, setme, sizeof(uint8_t));
}

void tr_peerIoReadUint16(tr_peerIo* io, struct evbuffer* inbuf, uint16_t* setme);

void tr_peerIoReadUint32(tr_peerIo* io, struct evbuffer* inbuf, uint32_t* setme);

void tr_peerIoDrain(tr_peerIo* io, struct evbuffer* inbuf, size_t byteCount);

/**
***
**/

size_t tr_peerIoGetWriteBufferSpace(tr_peerIo const* io, uint64_t now);

static inline void tr_peerIoSetParent(tr_peerIo* io, Bandwidth* parent)
{
    TR_ASSERT(tr_isPeerIo(io));

    io->bandwidth->setParent(parent);
}

void tr_peerIoBandwidthUsed(tr_peerIo* io, tr_direction direction, size_t byteCount, int isPieceData);

/**
***
**/

void tr_peerIoSetEnabled(tr_peerIo* io, tr_direction dir, bool isEnabled);

int tr_peerIoFlush(tr_peerIo* io, tr_direction dir, size_t byteLimit);

int tr_peerIoFlushOutgoingProtocolMsgs(tr_peerIo* io);

/* @} */
