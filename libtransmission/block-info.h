// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstdint> // uint32_t, uint64_t

#include "libtransmission/transmission.h"

struct tr_block_info
{
public:
    static auto constexpr BlockSize = uint32_t{ 1024U * 16U };

    tr_block_info() noexcept
    {
    }

    tr_block_info(uint64_t const total_size_in, uint32_t const piece_size_in) noexcept
    {
        init_sizes(total_size_in, piece_size_in);
    }

    [[nodiscard]] constexpr auto block_count() const noexcept
    {
        return n_blocks_;
    }

    [[nodiscard]] constexpr auto block_size(tr_block_index_t const block) const noexcept
    {
        return block + 1U == n_blocks_ ? final_block_size_ : BlockSize;
    }

    [[nodiscard]] constexpr auto piece_count() const noexcept
    {
        return n_pieces_;
    }

    [[nodiscard]] constexpr auto piece_size() const noexcept
    {
        return piece_size_;
    }

    [[nodiscard]] constexpr auto piece_size(tr_piece_index_t const piece) const noexcept
    {
        return piece + 1U == n_pieces_ ? final_piece_size_ : piece_size();
    }

    [[nodiscard]] constexpr auto total_size() const noexcept
    {
        return total_size_;
    }

    struct Location
    {
        [[nodiscard]] constexpr bool operator==(Location const& that) const noexcept
        {
            return this->byte == that.byte;
        }

        [[nodiscard]] constexpr bool operator<(Location const& that) const noexcept
        {
            return this->byte < that.byte;
        }

        uint64_t byte = {};

        tr_piece_index_t piece = {};
        uint32_t piece_offset = {};

        tr_block_index_t block = {};
        uint32_t block_offset = {};
    };

    // Location of the torrent's nth byte
    [[nodiscard]] constexpr auto byte_loc(uint64_t const byte_idx) const noexcept
    {
        auto loc = Location{};

        if (is_initialized())
        {
            loc.byte = byte_idx;

            loc.block = static_cast<tr_block_index_t>(byte_idx / BlockSize);
            loc.piece = static_cast<tr_piece_index_t>(byte_idx / piece_size());

            loc.block_offset = static_cast<uint32_t>(loc.byte - (uint64_t{ loc.block } * BlockSize));
            loc.piece_offset = static_cast<uint32_t>(loc.byte - (uint64_t{ loc.piece } * piece_size()));
        }

        return loc;
    }

    // Location of the first byte in `block`.
    [[nodiscard]] constexpr auto block_loc(tr_block_index_t const block) const noexcept
    {
        return byte_loc(uint64_t{ block } * BlockSize);
    }

    // Location of the first byte (+ optional offset and length) in `piece`
    [[nodiscard]] constexpr auto piece_loc(tr_piece_index_t piece, uint32_t offset = {}, uint32_t length = {}) const noexcept
    {
        return byte_loc(uint64_t{ piece } * piece_size() + offset + length);
    }

    [[nodiscard]] constexpr tr_block_span_t block_span_for_piece(tr_piece_index_t const piece) const noexcept
    {
        if (!is_initialized())
        {
            return { 0U, 0U };
        }

        return { piece_loc(piece).block, piece_last_loc(piece).block + 1U };
    }

    [[nodiscard]] constexpr tr_byte_span_t byte_span_for_piece(tr_piece_index_t const piece) const noexcept
    {
        if (!is_initialized())
        {
            return { 0U, 0U };
        }

        auto const offset = piece_loc(piece).byte;
        return { offset, offset + piece_size(piece) };
    }

private:
    void init_sizes(uint64_t total_size_in, uint32_t piece_size_in) noexcept;

    // Location of the last byte in `piece`.
    [[nodiscard]] constexpr Location piece_last_loc(tr_piece_index_t const piece) const noexcept
    {
        return byte_loc(static_cast<uint64_t>(piece) * piece_size() + piece_size(piece) - 1);
    }

    [[nodiscard]] constexpr bool is_initialized() const noexcept
    {
        return piece_size_ != 0U;
    }

    uint64_t total_size_ = {};

    tr_block_index_t n_blocks_ = {};
    tr_piece_index_t n_pieces_ = {};

    uint32_t final_block_size_ = {};

    uint32_t piece_size_ = {};
    uint32_t final_piece_size_ = {};
};
