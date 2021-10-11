/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <array>
#include <unordered_set>

#include "transmission.h"
#include "tr-assert.h"
#include "utils.h" /* tr_new(), tr_free() */

struct tr_peerIo;

/**
 * @addtogroup networked_io Networked IO
 * @{
 */

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

    /**
     * @brief Sets new peer, nullptr is allowed.
     */
    void setPeer(tr_peerIo* newPeer)
    {
        this->peer_ = newPeer;
    }

    /**
     * @brief Notify the bandwidth object that some of its allocated bandwidth has been consumed.
     * This is is usually invoked by the peer-io after a read or write.
     */
    void notifyBandwidthConsumed(tr_direction dir, size_t byteCount, bool isPieceData, uint64_t now);

    /**
     * @brief allocate the next period_msec's worth of bandwidth for the peer-ios to consume
     */
    void allocate(tr_direction dir, unsigned int period_msec);

    void setParent(Bandwidth* newParent);

    [[nodiscard]] tr_priority_t getPriority() const
    {
        return this->priority;
    }

    void setPriority(tr_priority_t prio)
    {
        this->priority = prio;
    }

    /**
     * @brief clamps byteCount down to a number that this bandwidth will allow to be consumed
    */
    [[nodiscard]] unsigned int clamp(tr_direction dir, unsigned int byteCount) const
    {
        return this->clamp(0, dir, byteCount);
    }

    /** @brief Get the raw total of bytes read or sent by this bandwidth subtree. */
    [[nodiscard]] unsigned int getRawSpeed_Bps(uint64_t const now, tr_direction const dir) const
    {
        TR_ASSERT(tr_isDirection(dir));

        return getSpeed_Bps(&this->band_[dir].raw_, HISTORY_MSEC, now);
    }

    /** @brief Get the number of piece data bytes read or sent by this bandwidth subtree. */
    [[nodiscard]] unsigned int getPieceSpeed_Bps(uint64_t const now, tr_direction const dir) const
    {
        TR_ASSERT(tr_isDirection(dir));

        return getSpeed_Bps(&this->band_[dir].piece_, HISTORY_MSEC, now);
    }

    /**
     * @brief Set the desired speed for this bandwidth subtree.
     * @see Bandwidth::allocate
     * @see Bandwidth::getDesiredSpeed
     */
    constexpr bool setDesiredSpeed_Bps(tr_direction dir, unsigned int desiredSpeed)
    {
        unsigned int* value = &this->band_[dir].desired_speed_bps_;
        bool const didChange = desiredSpeed != *value;
        *value = desiredSpeed;
        return didChange;
    }

    /**
     * @brief Get the desired speed for the bandwidth subtree.
     * @see Bandwidth::setDesiredSpeed
     */
    [[nodiscard]] constexpr double getDesiredSpeed_Bps(tr_direction dir) const
    {
        return this->band_[dir].desired_speed_bps_;
    }

    /**
     * @brief Set whether or not this bandwidth should throttle its peer-io's speeds
     */
    constexpr bool setLimited(tr_direction dir, bool isLimited)
    {
        bool* value = &this->band_[dir].is_limited_;
        bool const didChange = isLimited != *value;
        *value = isLimited;
        return didChange;
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
    constexpr bool honorParentLimits(tr_direction direction, bool isEnabled)
    {
        bool* value = &this->band_[direction].honor_parent_limits_;
        bool const didChange = isEnabled != *value;
        *value = isEnabled;
        return didChange;
    }

    [[nodiscard]] constexpr bool areParentLimitsHonored(tr_direction direction) const
    {
        TR_ASSERT(tr_isDirection(direction));

        return this->band_[direction].honor_parent_limits_;
    }

    static constexpr size_t HISTORY_MSEC = 2000U;
    static constexpr size_t INTERVAL_MSEC = HISTORY_MSEC;
    static constexpr size_t GRANULARITY_MSEC = 200;
    static constexpr size_t HISTORY_SIZE = (INTERVAL_MSEC / GRANULARITY_MSEC);

    struct RateControl
    {
        int newest_;
        struct Transfer
        {
            uint64_t date_;
            uint64_t size_;
        };
        std::array<Transfer, HISTORY_SIZE> transfers_;
        uint64_t cache_time_;
        unsigned int cache_val_;
    };

    struct Band
    {
        bool is_limited_;
        bool honor_parent_limits_;
        unsigned int bytes_left_;
        unsigned int desired_speed_bps_;
        RateControl raw_;
        RateControl piece_;
    };

private:
    static unsigned int getSpeed_Bps(RateControl const* r, unsigned int interval_msec, uint64_t now);

    static void notifyBandwidthConsumedBytes(uint64_t now, RateControl* r, size_t size);

    [[nodiscard]] unsigned int clamp(uint64_t now, tr_direction dir, unsigned int byteCount) const;

    static void phaseOne(std::vector<tr_peerIo*>& peerArray, tr_direction dir);

    void allocateBandwidth(
        tr_priority_t parent_priority,
        tr_direction dir,
        unsigned int period_msec,
        std::vector<tr_peerIo*>& peer_pool);

    tr_priority_t priority = 0;
    std::array<Band, 2> band_;
    Bandwidth* parent_;
    std::unordered_set<Bandwidth*> children_;
    tr_peerIo* peer_;
};

/* @} */
