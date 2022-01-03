/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include "transmission.h"

struct tr_block_info
{
    uint64_t total_size = 0;
    uint64_t piece_size = 0;
    uint64_t n_pieces = 0;

    tr_block_index_t n_blocks = 0;
    tr_block_index_t n_blocks_in_piece = 0;
    tr_block_index_t n_blocks_in_final_piece = 0;
    uint32_t block_size = 0;
    uint32_t final_block_size = 0;
    uint32_t final_piece_size = 0;

    tr_block_info() = default;
    tr_block_info(uint64_t total_size_in, uint64_t piece_size_in)
    {
        initSizes(total_size_in, piece_size_in);
    }

    void initSizes(uint64_t total_size_in, uint64_t piece_size_in);

    [[nodiscard]] constexpr auto blockCount() const
    {
        return n_blocks;
    }

    [[nodiscard]] constexpr auto blockSize() const
    {
        return block_size;
    }

    [[nodiscard]] constexpr auto blockSize(tr_block_index_t block) const
    {
        // how many bytes are in this block?
        return block + 1 == n_blocks ? final_block_size : blockSize();
    }

    [[nodiscard]] constexpr auto pieceCount() const
    {
        return n_pieces;
    }

    [[nodiscard]] constexpr tr_piece_index_t pieceForBlock(tr_block_index_t block) const
    {
        // if not initialized yet, don't divide by zero
        if (n_blocks_in_piece == 0)
        {
            return 0;
        }

        return block / n_blocks_in_piece;
    }

    [[nodiscard]] constexpr auto pieceSize() const
    {
        return piece_size;
    }

    [[nodiscard]] constexpr auto pieceSize(tr_piece_index_t piece) const
    {
        // how many bytes are in this piece?
        return piece + 1 == n_pieces ? final_piece_size : pieceSize();
    }

    [[nodiscard]] constexpr tr_piece_index_t pieceOf(uint64_t offset) const
    {
        // if not initialized yet, don't divide by zero
        if (piece_size == 0)
        {
            return 0;
        }

        // handle 0-byte files at the end of a torrent
        if (offset == total_size)
        {
            return n_pieces - 1;
        }

        return offset / piece_size;
    }

    [[nodiscard]] constexpr tr_block_index_t blockOf(uint64_t offset) const
    {
        // if not initialized yet, don't divide by zero
        if (block_size == 0)
        {
            return 0;
        }

        // handle 0-byte files at the end of a torrent
        if (offset == total_size)
        {
            return n_blocks - 1;
        }

        return offset / block_size;
    }

    [[nodiscard]] constexpr uint64_t offset(tr_piece_index_t piece, uint32_t offset, uint32_t length = 0) const
    {
        auto ret = piece_size;
        ret *= piece;
        ret += offset;
        ret += length;
        return ret;
    }

    [[nodiscard]] constexpr auto blockOf(tr_piece_index_t piece, uint32_t offset, uint32_t length = 0) const
    {
        return blockOf(this->offset(piece, offset, length));
    }

    [[nodiscard]] constexpr tr_block_span_t blockSpanForPiece(tr_piece_index_t piece) const
    {
        if (block_size == 0)
        {
            return {};
        }

        auto const begin = blockOf(offset(piece, 0));
        auto const end = 1 + blockOf(offset(piece, pieceSize(piece) - 1));
        return { begin, end };
    }

    [[nodiscard]] constexpr auto totalSize() const
    {
        return total_size;
    }

    static uint32_t bestBlockSize(uint64_t piece_size);
};
