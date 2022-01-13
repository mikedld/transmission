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

#include <cstddef>
#include <vector>

/**
 * @brief Implementation of the BitTorrent spec's Bitfield array of bits.
 *
 * This is for tracking the pieces a peer has. Its functionality is like
 * a bitset or vector<bool> with some added use cases:
 *
 * - It needs to be able to read/write the left-to-right bitfield format
 *   specified in the bittorrent spec. This is what raw() and getRaw()
 *   are for.
 *
 * - "Have all" is a special case where we know the peer has all the
 *   pieces and don't need to check the bit array. This is useful since
 *   (a) it's very common (i.e. seeds) and saves memory and work of
 *   allocating a bit array and doing lookups, and (b) if we have a
 *   magnet link and haven't gotten the metainfo yet, we may not know
 *   how many pieces there are -- but we can still know "this peer has
 *   all of them".
 *
 * - "Have none" is another special case that has the same advantages
 *   and motivations as "Have all".
 */
class tr_bitfield
{
public:
    explicit tr_bitfield(size_t bit_count);

    void setHasAll();
    void setHasNone();

    // set one or more bits
    void set(size_t bit, bool value = true);
    void setSpan(size_t begin, size_t end, bool value = true);
    void unset(size_t bit)
    {
        set(bit, false);
    }
    void unsetSpan(size_t begin, size_t end)
    {
        setSpan(begin, end, false);
    }
    void setFromBools(bool const* bytes, size_t n);

    // "raw" here is in BEP0003 format: "The first byte of the bitfield
    // corresponds to indices 0 - 7 from high bit to low bit, respectively.
    // The next one 8-15, etc. Spare bits at the end are set to zero.
    void setRaw(uint8_t const* bits, size_t byte_count);
    std::vector<uint8_t> raw() const;

    [[nodiscard]] constexpr bool hasAll() const
    {
        return have_all_hint_ || (bit_count_ > 0 && bit_count_ == true_count_);
    }

    [[nodiscard]] constexpr bool hasNone() const
    {
        return have_none_hint_ || (bit_count_ > 0 && true_count_ == 0);
    }

    [[nodiscard]] bool test(size_t bit) const
    {
        return hasAll() || (!hasNone() && testFlag(bit));
    }

    [[nodiscard]] constexpr size_t count() const
    {
        return true_count_;
    }

    [[nodiscard]] size_t count(size_t begin, size_t end) const;

    [[nodiscard]] constexpr size_t size() const
    {
        return bit_count_;
    }

    [[nodiscard]] constexpr size_t empty() const
    {
        return size() == 0;
    }

    bool isValid() const;

private:
    std::vector<uint8_t> flags_;
    [[nodiscard]] size_t countFlags() const;
    [[nodiscard]] size_t countFlags(size_t begin, size_t end) const;
    [[nodiscard]] bool testFlag(size_t bit) const;

    void ensureBitsAlloced(size_t n);
    [[nodiscard]] bool ensureNthBitAlloced(size_t nth);
    void freeArray();

    void setTrueCount(size_t n);
    void rebuildTrueCount();
    void incrementTrueCount(size_t inc);
    void decrementTrueCount(size_t dec);

    size_t bit_count_ = 0;
    size_t true_count_ = 0;

    /* Special cases for when full or empty but we don't know the bitCount.
       This occurs when a magnet link's peers send have all / have none */
    bool have_all_hint_ = false;
    bool have_none_hint_ = false;
};
