// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <array>
#include <cstddef> // size_t
#include <cstdint> // uint64_t
#include <vector>

#include "transmission.h"

#include "tr-assert.h"

class tr_peerIo;

/**
 * @addtogroup networked_io Networked IO
 * @{
 */

struct tr_bandwidth_limits
{
    unsigned int up_limit_KBps = 0;
    unsigned int down_limit_KBps = 0;
    bool up_limited = false;
    bool down_limited = false;
};

/**
 * Bandwidth is an object for measuring and constraining bandwidth speeds.
 *
 * Bandwidth objects can be "stacked" so that a peer can be made to obey
 * multiple constraints (for example, obeying the global speed limit and a
 * per-torrent speed limit).
 *
 * HIERARCHY
 *
 *   Transmission's bandwidth hierarchy is a tree.
 *   At the top is the global bandwidth object owned by tr_session.
 *   Its children are per-torrent bandwidth objects owned by tr_torrent.
 *   Underneath those are per-peer bandwidth objects owned by tr_peer.
 *
 *   tr_session also owns a tr_handshake's bandwidths, so that the handshake
 *   I/O can be counted in the global raw totals. When the handshake is done,
 *   the bandwidth's ownership passes to a tr_peer.
 *
 * MEASURING
 *
 *   When you ask a bandwidth object for its speed, it gives the speed of the
 *   subtree underneath it as well. So you can get Transmission's overall
 *   speed by quering tr_session's bandwidth, per-torrent speeds by asking
 *   tr_torrent's bandwidth, and per-peer speeds by asking tr_peer's bandwidth.
 *
 * CONSTRAINING
 *
 *   Call Bandwidth::allocate() periodically. tr_bandwidth knows its current
 *   speed and will decide how many bytes to make available over the
 *   user-specified period to reach the user-specified desired speed.
 *   If appropriate, it notifies its peer-ios that new bandwidth is available.
 *
 *   Bandwidth::allocate() operates on the tr_bandwidth subtree, so usually
 *   you'll only need to invoke it for the top-level tr_session bandwidth.
 *
 *   The peer-ios all have a pointer to their associated tr_bandwidth object,
 *   and call Bandwidth::clamp() before performing I/O to see how much
 *   bandwidth they can safely use.
 */
struct Bandwidth
{
public:
    explicit Bandwidth(Bandwidth* newParent);

    Bandwidth()
        : Bandwidth(nullptr)
    {
    }

    ~Bandwidth()
    {
        this->setParent(nullptr);
    }

    Bandwidth& operator=(Bandwidth&&) = delete;
    Bandwidth& operator=(Bandwidth) = delete;
    Bandwidth(Bandwidth&&) = delete;
    Bandwidth(Bandwidth&) = delete;

    /**
     * @brief Sets new peer, nullptr is allowed.
     */
    constexpr void setPeer(tr_peerIo* peer)
    {
        this->peer_ = peer;
    }

    /**
     * @brief Notify the bandwidth object that some of its allocated bandwidth has been consumed.
     * This is is usually invoked by the peer-io after a read or write.
     */
    void notifyBandwidthConsumed(tr_direction dir, size_t byte_count, bool is_piece_data, uint64_t now);

    /**
     * @brief allocate the next period_msec's worth of bandwidth for the peer-ios to consume
     */
    void allocate(tr_direction dir, unsigned int period_msec);

    void setParent(Bandwidth* newParent);

    [[nodiscard]] constexpr tr_priority_t getPriority() const noexcept
    {
        return this->priority_;
    }

    constexpr void setPriority(tr_priority_t prio) noexcept
    {
        this->priority_ = prio;
    }

    /**
     * @brief clamps byte_count down to a number that this bandwidth will allow to be consumed
     */
    [[nodiscard]] unsigned int clamp(tr_direction dir, unsigned int byte_count) const noexcept
    {
        return this->clamp(0, dir, byte_count);
    }

    /** @brief Get the raw total of bytes read or sent by this bandwidth subtree. */
    [[nodiscard]] unsigned int getRawSpeedBytesPerSecond(uint64_t const now, tr_direction const dir) const
    {
        TR_ASSERT(tr_isDirection(dir));

        return getSpeedBytesPerSecond(this->band_[dir].raw_, HistoryMSec, now);
    }

    /** @brief Get the number of piece data bytes read or sent by this bandwidth subtree. */
    [[nodiscard]] unsigned int getPieceSpeedBytesPerSecond(uint64_t const now, tr_direction const dir) const
    {
        TR_ASSERT(tr_isDirection(dir));

        return getSpeedBytesPerSecond(this->band_[dir].piece_, HistoryMSec, now);
    }

    /**
     * @brief Set the desired speed for this bandwidth subtree.
     * @see Bandwidth::allocate
     * @see Bandwidth::getDesiredSpeed
     */
    constexpr bool setDesiredSpeedBytesPerSecond(tr_direction dir, unsigned int desired_speed)
    {
        auto& value = this->band_[dir].desired_speed_bps_;
        bool const did_change = desired_speed != value;
        value = desired_speed;
        return did_change;
    }

    /**
     * @brief Get the desired speed for the bandwidth subtree.
     * @see Bandwidth::setDesiredSpeed
     */
    [[nodiscard]] constexpr double getDesiredSpeedBytesPerSecond(tr_direction dir) const
    {
        return this->band_[dir].desired_speed_bps_;
    }

    /**
     * @brief Set whether or not this bandwidth should throttle its peer-io's speeds
     */
    constexpr bool setLimited(tr_direction dir, bool is_limited)
    {
        bool* value = &this->band_[dir].is_limited_;
        bool const did_change = is_limited != *value;
        *value = is_limited;
        return did_change;
    }

    /**
     * @return nonzero if this bandwidth throttles its peer-ios speeds
     */
    [[nodiscard]] constexpr bool isLimited(tr_direction dir) const
    {
        return this->band_[dir].is_limited_;
    }

    /**
     * Almost all the time we do want to honor a parents' bandwidth cap, so that
     * (for example) a peer is constrained by a per-torrent cap and the global cap.
     * But when we set a torrent's speed mode to TR_SPEEDLIMIT_UNLIMITED, then
     * in that particular case we want to ignore the global speed limit...
     */
    constexpr bool honorParentLimits(tr_direction direction, bool is_enabled)
    {
        bool* value = &this->band_[direction].honor_parent_limits_;
        bool const did_change = is_enabled != *value;
        *value = is_enabled;
        return did_change;
    }

    [[nodiscard]] constexpr bool areParentLimitsHonored(tr_direction direction) const
    {
        TR_ASSERT(tr_isDirection(direction));

        return this->band_[direction].honor_parent_limits_;
    }

    static constexpr size_t HistoryMSec = 2000U;
    static constexpr size_t IntervalMSec = HistoryMSec;
    static constexpr size_t GranularityMSec = 250;
    static constexpr size_t HistorySize = (IntervalMSec / GranularityMSec);

    struct RateControl
    {
        std::array<uint64_t, HistorySize> date_;
        std::array<uint32_t, HistorySize> size_;
        uint64_t cache_time_;
        unsigned int cache_val_;
        int newest_;
    };

    struct Band
    {
        RateControl raw_;
        RateControl piece_;
        unsigned int bytes_left_;
        unsigned int desired_speed_bps_;
        bool is_limited_ = false;
        bool honor_parent_limits_ = true;
    };

    tr_bandwidth_limits getLimits() const;
    void setLimits(tr_bandwidth_limits const* limits);

private:
    static unsigned int getSpeedBytesPerSecond(RateControl& r, unsigned int interval_msec, uint64_t now);

    static void notifyBandwidthConsumedBytes(uint64_t now, RateControl* r, size_t size);

    [[nodiscard]] unsigned int clamp(uint64_t now, tr_direction dir, unsigned int byte_count) const;

    static void phaseOne(std::vector<tr_peerIo*>& peer_array, tr_direction dir);

    void allocateBandwidth(
        tr_priority_t parent_priority,
        tr_direction dir,
        unsigned int period_msec,
        std::vector<tr_peerIo*>& peer_pool);

    mutable std::array<Band, 2> band_ = {};
    std::vector<Bandwidth*> children_;
    Bandwidth* parent_ = nullptr;
    tr_peerIo* peer_ = nullptr;
    tr_priority_t priority_ = 0;
};

/* @} */
