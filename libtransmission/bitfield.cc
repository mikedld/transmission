// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::copy, std::fill_n, std::min, std::max
#include <cstddef>
#include <cstdint>
#include <vector> // std::vector

#include "libtransmission/bitfield.h"
#include "libtransmission/tr-assert.h" // TR_ASSERT, TR_ENABLE_ASSERTS
#include "libtransmission/tr-popcount.h" // tr_popcnt

// ---

namespace
{

[[nodiscard]] constexpr size_t getBytesNeeded(size_t bit_count) noexcept
{
    /* NB: If can guarantee bit_count <= SIZE_MAX - 8 then faster logic
       is ((bit_count + 7) >> 3). */
    return (bit_count >> 3) + ((bit_count & 7) != 0 ? 1 : 0);
}

/* Used only in cases where it can be guaranteed bit_count <= SIZE_MAX - 8 */
[[nodiscard]] constexpr size_t getBytesNeededSafe(size_t bit_count) noexcept
{
    return ((bit_count + 7) >> 3);
}

void setAllTrue(uint8_t* array, size_t bit_count)
{
    uint8_t constexpr Val = 0xFF;
    /* Only ever called internally with in-use bit counts. Impossible
       for bitcount > SIZE_MAX - 8. */
    size_t const n = getBytesNeededSafe(bit_count);

    if (n > 0)
    {
        std::fill_n(array, n, Val);
        /* -bit_count & 7U. Since bitcount is unsigned do ~bitcount +
           1 to replace -bitcount as linters warn about negating
           unsigned types. Any compiler will optimize ~x + 1 to -x in
           the backend. */
        uint32_t const shift = ((~bit_count) + 1) & 7U;
        array[n - 1] = Val << shift;
    }
}

/* Switch to std::popcount if project upgrades to c++20 or newer */
[[nodiscard]] uint32_t doPopcount(uint8_t flags) noexcept
{
    /* If flags are ever expanded to use machine words instead of
       uint8_t popcnt64 is also available */
    return tr_popcnt<uint8_t>::count(flags);
}

[[nodiscard]] size_t rawCountFlags(uint8_t const* flags, size_t n) noexcept
{
    auto ret = size_t{};

    for (auto const* const end = flags + n; flags != end; ++flags)
    {
        ret += doPopcount(*flags);
    }

    return ret;
}

} // namespace

// ---

size_t tr_bitfield::count_flags() const noexcept
{
    return rawCountFlags(std::data(flags_), std::size(flags_));
}

size_t tr_bitfield::count_flags(size_t begin, size_t end) const noexcept
{
    auto ret = size_t{};
    size_t const first_byte = begin >> 3U;
    size_t const last_byte = (end - 1) >> 3U;

    if (bit_count_ == 0)
    {
        return 0;
    }

    if (first_byte >= std::size(flags_))
    {
        return 0;
    }

    TR_ASSERT(begin < end);
    TR_ASSERT(!std::empty(flags_));

    if (first_byte == last_byte)
    {
        uint8_t val = flags_[first_byte];

        auto i = begin & 7U;
        val <<= i;
        i = (begin - end) & 7U;
        val >>= i;
        ret = doPopcount(val);
    }
    else
    {
        size_t const walk_end = std::min(std::size(flags_), last_byte);

        /* first byte */
        size_t const first_shift = begin & 7U;
        uint8_t val = flags_[first_byte];
        val <<= first_shift;
        /* No need to shift back val for correct popcount. */
        ret = doPopcount(val);

        /* middle bytes */

        /* Use 2x accumulators to help alleviate high latency of
           popcnt instruction on many architectures. */
        size_t tmp_accum = 0;
        for (size_t i = first_byte + 1; i < walk_end;)
        {
            tmp_accum += doPopcount(flags_[i]);
            i += 2;
            if (i > walk_end)
            {
                break;
            }
            ret += doPopcount(flags_[i - 1]);
        }
        ret += tmp_accum;

        /* last byte */
        if (last_byte < std::size(flags_))
        {
            /* -end & 7U. Since bitcount is unsigned do ~end + 1 to
               replace -end as linters warn about negating unsigned
               types. Any compiler will optimize ~x + 1 to -x in the
               backend. */
            uint32_t const last_shift = (~end + 1) & 7U;
            val = flags_[last_byte];
            val >>= last_shift;
            /* No need to shift back val for correct popcount. */
            ret += doPopcount(val);
        }
    }

    TR_ASSERT(ret <= (begin - end));
    return ret;
}

size_t tr_bitfield::count(size_t begin, size_t end) const
{
    if (has_all())
    {
        return end - begin;
    }

    if (has_none())
    {
        return 0;
    }

    return count_flags(begin, end);
}

// ---

bool tr_bitfield::is_valid() const
{
    return std::empty(flags_) || true_count_ == count_flags();
}

std::vector<uint8_t> tr_bitfield::raw() const
{
    /* Impossible for bit_count_ to exceed SIZE_MAX - 8 */
    auto const n = getBytesNeededSafe(bit_count_);

    if (!std::empty(flags_))
    {
        return flags_;
    }

    auto raw = std::vector<uint8_t>(n);

    if (has_all())
    {
        setAllTrue(std::data(raw), bit_count_);
    }

    return raw;
}

void tr_bitfield::ensure_bits_alloced(size_t n)
{
    bool const has_all = this->has_all();

    /* Can't use getBytesNeededSafe as n can be > SIZE_MAX - 8. */
    size_t const bytes_needed = has_all ? getBytesNeeded(std::max(n, true_count_)) : getBytesNeeded(n);

    if (std::size(flags_) < bytes_needed)
    {
        flags_.resize(bytes_needed);
        if (has_all)
        {
            setAllTrue(std::data(flags_), true_count_);
        }
    }
}

bool tr_bitfield::ensure_nth_bit_alloced(size_t nth)
{
    // count is zero-based, so we need to allocate nth+1 bits before setting the nth */
    if (nth == SIZE_MAX)
    {
        return false;
    }

    ensure_bits_alloced(nth + 1);
    return true;
}

void tr_bitfield::set_true_count(size_t n) noexcept
{
    TR_ASSERT(bit_count_ == 0 || n <= bit_count_);

    true_count_ = n;
    have_all_hint_ = n == bit_count_;
    have_none_hint_ = n == 0;

    if (has_all() || has_none())
    {
        free_array();
    }

    TR_ASSERT(is_valid());
}

void tr_bitfield::increment_true_count(size_t inc) noexcept
{
    TR_ASSERT(bit_count_ == 0 || inc <= bit_count_);
    TR_ASSERT(bit_count_ == 0 || true_count_ <= bit_count_ - inc);

    set_true_count(true_count_ + inc);
}

void tr_bitfield::decrement_true_count(size_t dec) noexcept
{
    TR_ASSERT(bit_count_ == 0 || dec <= bit_count_);
    TR_ASSERT(bit_count_ == 0 || true_count_ >= dec);

    set_true_count(true_count_ - dec);
}

// ---

tr_bitfield::tr_bitfield(size_t bit_count)
    : bit_count_{ bit_count }
{
    TR_ASSERT(is_valid());
}

void tr_bitfield::set_has_none() noexcept
{
    free_array();
    true_count_ = 0;
    have_all_hint_ = false;
    have_none_hint_ = true;

    TR_ASSERT(is_valid());
}

void tr_bitfield::set_has_all() noexcept
{
    free_array();
    true_count_ = bit_count_;
    have_all_hint_ = true;
    have_none_hint_ = false;

    TR_ASSERT(is_valid());
}

void tr_bitfield::set_raw(uint8_t const* raw, size_t byte_count)
{
    flags_.assign(raw, raw + byte_count);

    // ensure any excess bits at the end of the array are set to '0'.
    if (byte_count == getBytesNeededSafe(bit_count_))
    {
        auto const excess_bit_count = (byte_count * 8) - bit_count_;

        TR_ASSERT(excess_bit_count <= 7);

        if (excess_bit_count != 0)
        {
            flags_.back() &= 0xff << excess_bit_count;
        }
    }

    rebuild_true_count();
}

void tr_bitfield::set_from_bools(bool const* flags, size_t n)
{
    size_t true_count = 0;

    free_array();
    ensure_bits_alloced(n);

    for (size_t i = 0; i < n; ++i)
    {
        if (flags[i])
        {
            ++true_count;
            flags_[i >> 3U] |= (0x80 >> (i & 7U));
        }
    }

    set_true_count(true_count);
}

void tr_bitfield::set(size_t nth, bool value)
{
    if (test(nth) == value)
    {
        return;
    }

    if (!ensure_nth_bit_alloced(nth))
    {
        return;
    }

    /* Already tested that val != nth bit so just swap */
    auto& byte = flags_[nth >> 3U];
#ifdef TR_ENABLE_ASSERTS
    auto const old_byte_pop = doPopcount(byte);
#endif
    byte ^= 0x80 >> (nth & 7U);
#ifdef TR_ENABLE_ASSERTS
    auto const new_byte_pop = doPopcount(byte);
#endif

    if (value)
    {
        ++true_count_;
        TR_ASSERT(old_byte_pop + 1 == new_byte_pop);
    }
    else
    {
        --true_count_;
        TR_ASSERT(new_byte_pop + 1 == old_byte_pop);
    }
    have_all_hint_ = true_count_ == bit_count_;
    have_none_hint_ = true_count_ == 0;
}

/* Sets bit range [begin, end) to 1 */
void tr_bitfield::set_span(size_t begin, size_t end, bool value)
{
    // bounds check
    end = std::min(end, bit_count_);
    if (end == 0 || begin >= end)
    {
        return;
    }

    // NB: count(begin, end) can be quite expensive. Might be worth it
    // to fuse the count and set loop
    size_t const old_count = count(begin, end);
    size_t const new_count = value ? (end - begin) : 0;
    // did anything change?
    if (old_count == new_count)
    {
        return;
    }

    --end;
    if (!ensure_nth_bit_alloced(end))
    {
        return;
    }

    auto walk = begin >> 3;
    auto const last_byte = end >> 3;

    unsigned char first_mask = 0xff >> (begin & 7U);
    unsigned char last_mask = 0xff << ((~end) & 7U);
    if (value)
    {
        if (walk == last_byte)
        {
            flags_[walk] |= first_mask & last_mask;
        }
        else
        {
            flags_[walk] |= first_mask;
            /* last_byte is expected to be hot in cache due to earlier
               count(begin, end) */
            flags_[last_byte] |= last_mask;
            if (++walk < last_byte)
            {
                std::fill_n(std::data(flags_) + walk, last_byte - walk, 0xff);
            }
        }

        increment_true_count(new_count - old_count);
    }
    else
    {
        first_mask = ~first_mask;
        last_mask = ~last_mask;
        if (walk == last_byte)
        {
            flags_[walk] &= first_mask | last_mask;
        }
        else
        {
            flags_[walk] &= first_mask;
            /* last_byte is expected to be hot in cache due to earlier
               count(begin, end) */
            flags_[last_byte] &= last_mask;
            if (++walk < last_byte)
            {
                std::fill_n(std::data(flags_) + walk, last_byte - walk, 0);
            }
        }

        decrement_true_count(old_count);
    }
}

tr_bitfield& tr_bitfield::operator|=(tr_bitfield const& that) noexcept
{
    if (has_all() || that.has_none())
    {
        return *this;
    }

    if (that.has_all() || has_none())
    {
        *this = that;
        return *this;
    }

    flags_.resize(std::max(std::size(flags_), std::size(that.flags_)));

    for (size_t i = 0, n = std::size(that.flags_); i < n; ++i)
    {
        flags_[i] |= that.flags_[i];
    }

    rebuild_true_count();
    return *this;
}

tr_bitfield& tr_bitfield::operator&=(tr_bitfield const& that) noexcept
{
    if (has_none() || that.has_all())
    {
        return *this;
    }

    if (that.has_none() || has_all())
    {
        *this = that;
        return *this;
    }

    flags_.resize(std::min(std::size(flags_), std::size(that.flags_)));

    for (size_t i = 0, n = std::size(flags_); i < n; ++i)
    {
        flags_[i] &= that.flags_[i];
    }

    rebuild_true_count();
    return *this;
}

bool tr_bitfield::intersects(tr_bitfield const& that) const noexcept
{
    if (has_none() || that.has_none())
    {
        return false;
    }

    if (has_all() || that.has_all())
    {
        return true;
    }

    for (size_t i = 0, n = std::min(std::size(flags_), std::size(that.flags_)); i < n; ++i)
    {
        if ((flags_[i] & that.flags_[i]) != 0U)
        {
            return true;
        }
    }

    return false;
}
