// This file Copyright (C) 2021-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cstddef> // size_t
#include <cstdint>
#include <set>

#include <libtransmission/transmission.h>

#include <libtransmission/bitfield.h>
#include <libtransmission/block-info.h>
#include <libtransmission/crypto-utils.h> // for tr_rand_obj()
#include <libtransmission/completion.h>

#include "gtest/gtest.h"

using CompletionTest = ::testing::Test;

namespace
{

struct TestTorrent
{
    std::set<tr_piece_index_t> dnd_pieces;

    [[nodiscard]] tr_completion makeCompletion(tr_block_info const& block_info) const
    {
        return { [this](tr_piece_index_t const piece) { return dnd_pieces.count(piece) == 0; }, &block_info };
    }
};

auto constexpr BlockSize = uint64_t{ 16 } * 1024U;

} // namespace

TEST_F(CompletionTest, MagnetLink)
{
    auto torrent = TestTorrent{};
    auto block_info = tr_block_info{};
    auto completion = torrent.makeCompletion(block_info);

    EXPECT_FALSE(completion.has_all());
    EXPECT_TRUE(completion.has_none());
    EXPECT_FALSE(completion.has_blocks({ 0, 1 }));
    EXPECT_FALSE(completion.has_blocks({ 0, 1000 }));
    EXPECT_FALSE(completion.has_piece(0));
    EXPECT_DOUBLE_EQ(0.0, completion.percent_done());
    EXPECT_DOUBLE_EQ(0.0, completion.percent_complete());
    EXPECT_EQ(TR_LEECH, completion.status());
    EXPECT_EQ(0, completion.has_total());
    EXPECT_EQ(0, completion.has_valid());
    EXPECT_EQ(0, completion.left_until_done());
    EXPECT_EQ(0, completion.size_when_done());
}

TEST_F(CompletionTest, setBlocks)
{
    auto constexpr TotalSize = uint64_t{ BlockSize * 64 * 50000 }; // 50GB
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };

    auto torrent = TestTorrent{};
    auto const block_info = tr_block_info{ TotalSize, PieceSize };
    auto completion = torrent.makeCompletion(block_info);
    EXPECT_FALSE(completion.blocks().has_all());
    EXPECT_FALSE(completion.has_all());
    EXPECT_EQ(0, completion.has_total());

    auto bitfield = tr_bitfield{ block_info.block_count() };
    bitfield.set_has_all();

    // test that the bitfield did get replaced
    completion.set_blocks(bitfield);
    EXPECT_TRUE(completion.blocks().has_all());
    EXPECT_TRUE(completion.has_all());
    EXPECT_EQ(block_info.total_size(), completion.has_total());
}

TEST_F(CompletionTest, hasBlock)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 };
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };
    auto completion = torrent.makeCompletion(block_info);

    EXPECT_FALSE(completion.has_block(0));
    EXPECT_FALSE(completion.has_block(1));

    completion.add_block(0);
    EXPECT_TRUE(completion.has_block(0));
    EXPECT_FALSE(completion.has_block(1));

    completion.add_piece(0);
    EXPECT_TRUE(completion.has_block(0));
    EXPECT_TRUE(completion.has_block(1));
}

TEST_F(CompletionTest, hasBlocks)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 };
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };

    auto completion = torrent.makeCompletion(block_info);
    EXPECT_FALSE(completion.has_blocks({ 0, 1 }));
    EXPECT_FALSE(completion.has_blocks({ 0, 2 }));

    completion.add_block(0);
    EXPECT_TRUE(completion.has_blocks({ 0, 1 }));
    EXPECT_FALSE(completion.has_blocks({ 0, 2 }));
}

TEST_F(CompletionTest, hasNone)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 };
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };

    auto completion = torrent.makeCompletion(block_info);
    EXPECT_TRUE(completion.has_none());

    completion.add_block(0);
    EXPECT_FALSE(completion.has_none());
}

TEST_F(CompletionTest, hasPiece)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 };
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };

    // check that the initial state does not have it
    auto completion = torrent.makeCompletion(block_info);
    EXPECT_FALSE(completion.has_piece(0));
    EXPECT_FALSE(completion.has_piece(1));
    EXPECT_EQ(0, completion.has_valid());

    // check that adding a piece means we have it
    completion.add_piece(0);
    EXPECT_TRUE(completion.has_piece(0));
    EXPECT_FALSE(completion.has_piece(1));
    EXPECT_EQ(PieceSize, completion.has_valid());

    // check that removing a piece means we don't have it
    completion.remove_piece(0);
    EXPECT_FALSE(completion.has_piece(0));
    EXPECT_FALSE(completion.has_piece(1));
    EXPECT_EQ(0, completion.has_valid());

    // check that adding all the blocks in a piece means we have it
    for (tr_block_index_t i = 1, n = block_info.piece_loc(1).block; i < n; ++i)
    {
        completion.add_block(i);
    }
    EXPECT_FALSE(completion.has_piece(0));
    EXPECT_EQ(0, completion.has_valid());
    completion.add_block(0);
    EXPECT_TRUE(completion.has_piece(0));
    EXPECT_EQ(PieceSize, completion.has_valid());
}

TEST_F(CompletionTest, percentCompleteAndDone)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 };
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };

    // check that in blank-slate initial state, isDone() is false
    auto completion = torrent.makeCompletion(block_info);
    EXPECT_DOUBLE_EQ(0.0, completion.percent_complete());
    EXPECT_DOUBLE_EQ(0.0, completion.percent_done());

    // add half the pieces
    for (size_t i = 0; i < 32; ++i)
    {
        completion.add_piece(i);
    }
    EXPECT_DOUBLE_EQ(0.5, completion.percent_complete());
    EXPECT_DOUBLE_EQ(0.5, completion.percent_done());

    // but marking some of the pieces we have as unwanted
    // should not change percent_done
    for (size_t i = 0; i < 16; ++i)
    {
        torrent.dnd_pieces.insert(i);
    }
    completion.invalidate_size_when_done();
    EXPECT_DOUBLE_EQ(0.5, completion.percent_complete());
    EXPECT_DOUBLE_EQ(0.5, completion.percent_done());

    // but marking some of the pieces we DON'T have as unwanted
    // SHOULD change percent_done
    for (size_t i = 32; i < 48; ++i)
    {
        torrent.dnd_pieces.insert(i);
    }
    completion.invalidate_size_when_done();
    EXPECT_DOUBLE_EQ(0.5, completion.percent_complete());
    EXPECT_DOUBLE_EQ(2.0 / 3.0, completion.percent_done());
}

TEST_F(CompletionTest, hasTotalAndValid)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 } + 1;
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };

    // check that the initial blank-slate state has nothing
    auto completion = torrent.makeCompletion(block_info);
    EXPECT_EQ(0, completion.has_total());
    EXPECT_EQ(completion.has_valid(), completion.has_total());

    // check that adding the final piece adjusts by block_info.final_piece_size
    completion.set_has_piece(block_info.piece_count() - 1, true);
    EXPECT_EQ(block_info.piece_size(block_info.piece_count() - 1), completion.has_total());
    EXPECT_EQ(completion.has_valid(), completion.has_total());

    // check that adding a non-final piece adjusts by block_info.pieceSize()
    completion.set_has_piece(0, true);
    EXPECT_EQ(block_info.piece_size(block_info.piece_count() - 1) + block_info.piece_size(), completion.has_total());
    EXPECT_EQ(completion.has_valid(), completion.has_total());

    // check that removing the final piece adjusts by block_info.final_piece_size
    completion.set_has_piece(block_info.piece_count() - 1, false);
    EXPECT_EQ(block_info.piece_size(), completion.has_valid());
    EXPECT_EQ(completion.has_valid(), completion.has_total());

    // check that removing a non-final piece adjusts by block_info.pieceSize()
    completion.set_has_piece(0, false);
    EXPECT_EQ(0, completion.has_valid());
    EXPECT_EQ(completion.has_valid(), completion.has_total());

    // check that adding an incomplete piece adjusts hasTotal but not hasValid
    completion.add_block(0);
    EXPECT_EQ(0, completion.has_valid());
    EXPECT_EQ(BlockSize, completion.has_total());
}

TEST_F(CompletionTest, leftUntilDone)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 } + 1;
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };

    // check that the initial blank-slate state has nothing
    auto completion = torrent.makeCompletion(block_info);
    EXPECT_EQ(block_info.total_size(), completion.left_until_done());

    // check that adding the final piece adjusts by block_info.final_piece_size
    completion.add_piece(block_info.piece_count() - 1);
    EXPECT_EQ(block_info.total_size() - block_info.piece_size(block_info.piece_count() - 1), completion.left_until_done());

    // check that adding a non-final piece adjusts by block_info.pieceSize()
    completion.add_piece(0);
    EXPECT_EQ(
        block_info.total_size() - block_info.piece_size(block_info.piece_count() - 1) - block_info.piece_size(),
        completion.left_until_done());

    // check that removing the final piece adjusts by block_info.final_piece_size
    completion.remove_piece(block_info.piece_count() - 1);
    EXPECT_EQ(block_info.total_size() - block_info.piece_size(), completion.left_until_done());

    // check that dnd-flagging a piece we already have affects nothing
    torrent.dnd_pieces.insert(0);
    completion.invalidate_size_when_done();
    EXPECT_EQ(block_info.total_size() - block_info.piece_size(), completion.left_until_done());
    torrent.dnd_pieces.clear();
    completion.invalidate_size_when_done();

    // check that dnd-flagging a piece we DON'T already have adjusts by block_info.pieceSize()
    torrent.dnd_pieces.insert(1);
    completion.invalidate_size_when_done();
    EXPECT_EQ(block_info.total_size() - block_info.piece_size() * uint64_t{ 2U }, completion.left_until_done());
    torrent.dnd_pieces.clear();
    completion.invalidate_size_when_done();

    // check that removing a non-final piece adjusts by block_info.pieceSize()
    completion.remove_piece(0);
    EXPECT_EQ(block_info.total_size(), completion.left_until_done());

    // check that adding a block adjusts by block_info.block_size
    completion.add_block(0);
    EXPECT_EQ(block_info.total_size() - tr_block_info::BlockSize, completion.left_until_done());
}

TEST_F(CompletionTest, sizeWhenDone)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 64 * 50000 }; // 50GB
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };

    // check that adding or removing blocks or pieces does not affect size_when_done
    auto completion = torrent.makeCompletion(block_info);
    EXPECT_EQ(block_info.total_size(), completion.size_when_done());
    completion.add_block(0);
    EXPECT_EQ(block_info.total_size(), completion.size_when_done());
    completion.add_piece(0);
    EXPECT_EQ(block_info.total_size(), completion.size_when_done());
    completion.remove_piece(0);
    EXPECT_EQ(block_info.total_size(), completion.size_when_done());

    // check that flagging complete pieces as dnd does not affect size_when_done
    for (size_t i = 0; i < 32; ++i)
    {
        completion.add_piece(i);
        torrent.dnd_pieces.insert(i);
    }
    completion.invalidate_size_when_done();
    EXPECT_EQ(block_info.total_size(), completion.size_when_done());

    // check that flagging missing pieces as dnd does not affect size_when_done
    for (size_t i = 32; i < 48; ++i)
    {
        torrent.dnd_pieces.insert(i);
    }
    completion.invalidate_size_when_done();
    EXPECT_EQ(block_info.total_size() - uint64_t{ 16U } * block_info.piece_size(), completion.size_when_done());
}

TEST_F(CompletionTest, createPieceBitfield)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 } + 1;
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };

    // make a completion object that has a random assortment of pieces
    auto completion = torrent.makeCompletion(block_info);
    auto buf = tr_rand_obj<std::array<char, 65>>();
    ASSERT_EQ(std::size(buf), block_info.piece_count());
    for (uint64_t i = 0; i < block_info.piece_count(); ++i)
    {
        if ((buf[i] % 2) != 0)
        {
            completion.add_piece(i);
        }
    }

    // serialize it to a raw bitfield, read it back into a bitfield,
    // and test that the new bitfield matches
    auto const pieces_raw_bitfield = completion.create_piece_bitfield();
    tr_bitfield pieces{ size_t{ block_info.piece_count() } };
    pieces.set_raw(std::data(pieces_raw_bitfield), std::size(pieces_raw_bitfield));
    for (uint64_t i = 0; i < block_info.piece_count(); ++i)
    {
        EXPECT_EQ(completion.has_piece(i), pieces.test(i));
    }
}

TEST_F(CompletionTest, setHasPiece)
{
}

TEST_F(CompletionTest, countMissingBytesInPiece)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 } + 1;
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };
    auto completion = torrent.makeCompletion(block_info);

    EXPECT_EQ(block_info.piece_size(0), completion.count_missing_bytes_in_piece(0));
    completion.add_block(0);
    EXPECT_EQ(block_info.piece_size(0) - tr_block_info::BlockSize, completion.count_missing_bytes_in_piece(0));
    completion.add_piece(0);
    EXPECT_EQ(0U, completion.count_missing_bytes_in_piece(0));

    auto const final_piece = block_info.piece_count() - 1;
    auto const final_block = block_info.block_count() - 1;
    EXPECT_EQ(block_info.piece_size(final_piece), completion.count_missing_bytes_in_piece(final_piece));
    completion.add_block(final_block);
    EXPECT_EQ(1U, block_info.piece_size(block_info.piece_count() - 1));
    EXPECT_TRUE(completion.has_piece(final_piece));
    EXPECT_EQ(0U, completion.count_missing_bytes_in_piece(final_piece));
}

TEST_F(CompletionTest, amountDone)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 } + 1;
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };
    auto completion = torrent.makeCompletion(block_info);

    // make bins s.t. each bin is a single piece
    auto bins = std::array<float, TotalSize / PieceSize>{};

    for (tr_piece_index_t piece = 0; piece < block_info.piece_count(); ++piece)
    {
        completion.remove_piece(piece);
    }
    completion.amount_done(std::data(bins), std::size(bins));
    std::for_each(std::begin(bins), std::end(bins), [](float bin) { EXPECT_DOUBLE_EQ(0.0, bin); });

    // one block
    completion.add_block(0);
    completion.amount_done(std::data(bins), std::size(bins));
    EXPECT_DOUBLE_EQ(0.0, bins[1]);

    // one piece
    completion.add_piece(0);
    completion.amount_done(std::data(bins), std::size(bins));
    EXPECT_DOUBLE_EQ(1.0, bins[0]);
    EXPECT_DOUBLE_EQ(0.0, bins[1]);

    // all pieces
    for (tr_piece_index_t piece = 0; piece < block_info.piece_count(); ++piece)
    {
        completion.add_piece(piece);
    }
    completion.amount_done(std::data(bins), std::size(bins));
    std::for_each(std::begin(bins), std::end(bins), [](float bin) { EXPECT_DOUBLE_EQ(1.0, bin); });

    // don't do anything if fed bad input
    auto const backup = bins;
    completion.amount_done(std::data(bins), 0);
    EXPECT_EQ(backup, bins);
}

TEST_F(CompletionTest, countHasBytesInSpan)
{
    // set up a fake torrent
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 } + 1;
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };
    auto completion = torrent.makeCompletion(block_info);

    // torrent is complete
    auto blocks = tr_bitfield{ block_info.block_count() };
    blocks.set_has_all();
    completion.set_blocks(blocks);

    EXPECT_EQ(TotalSize, completion.count_has_bytes_in_span({ 0, TotalSize }));
    EXPECT_EQ(TotalSize, completion.count_has_bytes_in_span({ 0, TotalSize + 1 }));
    // test span that's entirely in a single block
    EXPECT_EQ(1, completion.count_has_bytes_in_span({ 16, 17 }));
    EXPECT_EQ(16, completion.count_has_bytes_in_span({ 16, 32 }));
    // test edge cases on block boundary
    EXPECT_EQ(1, completion.count_has_bytes_in_span({ BlockSize - 1, BlockSize }));
    EXPECT_EQ(1, completion.count_has_bytes_in_span({ BlockSize, BlockSize + 1 }));
    EXPECT_EQ(2, completion.count_has_bytes_in_span({ BlockSize - 1, BlockSize + 1 }));
    // test edge cases on piece boundary
    EXPECT_EQ(1, completion.count_has_bytes_in_span({ PieceSize - 1, PieceSize }));
    EXPECT_EQ(1, completion.count_has_bytes_in_span({ PieceSize, PieceSize + 1 }));
    EXPECT_EQ(2, completion.count_has_bytes_in_span({ PieceSize - 1, PieceSize + 1 }));

    // test span that has a middle block
    EXPECT_EQ(BlockSize * 3, completion.count_has_bytes_in_span({ 0, BlockSize * 3 }));
    EXPECT_EQ(BlockSize * 2, completion.count_has_bytes_in_span({ BlockSize / 2, BlockSize * 2 + BlockSize / 2 }));

    // test span where first block is missing
    blocks.unset(0);
    completion.set_blocks(blocks);
    EXPECT_EQ(BlockSize * 2, completion.count_has_bytes_in_span({ 0, BlockSize * 3 }));
    EXPECT_EQ(BlockSize * 1.5, completion.count_has_bytes_in_span({ BlockSize / 2, BlockSize * 2 + BlockSize / 2 }));
    // test span where final block is missing
    blocks.set_has_all();
    blocks.unset(2);
    completion.set_blocks(blocks);
    EXPECT_EQ(BlockSize * 2, completion.count_has_bytes_in_span({ 0, BlockSize * 3 }));
    EXPECT_EQ(BlockSize * 1.5, completion.count_has_bytes_in_span({ BlockSize / 2, BlockSize * 2 + BlockSize / 2 }));
}

TEST_F(CompletionTest, wantNone)
{
    auto torrent = TestTorrent{};
    auto constexpr TotalSize = uint64_t{ BlockSize * 4096 };
    auto constexpr PieceSize = uint64_t{ BlockSize * 64 };
    auto const block_info = tr_block_info{ TotalSize, PieceSize };
    auto completion = torrent.makeCompletion(block_info);

    // we have some data
    completion.add_block(0);

    // and want nothing
    for (tr_piece_index_t i = 0, n = block_info.block_count(); i < n; ++i)
    {
        torrent.dnd_pieces.insert(i);
    }
    completion.invalidate_size_when_done();

    EXPECT_LE(completion.has_total(), completion.size_when_done());
    EXPECT_EQ(completion.has_total(), block_info.BlockSize);
    EXPECT_EQ(completion.size_when_done(), block_info.BlockSize);
    EXPECT_LE(completion.left_until_done(), completion.size_when_done());
    EXPECT_EQ(completion.left_until_done(), 0);

    // but we magically get a block anyway
    completion.add_block(1);

    EXPECT_LE(completion.has_total(), completion.size_when_done());
    EXPECT_EQ(completion.has_total(), 2 * block_info.BlockSize);
    EXPECT_EQ(completion.size_when_done(), 2 * block_info.BlockSize);
    EXPECT_LE(completion.left_until_done(), completion.size_when_done());
    EXPECT_EQ(completion.left_until_done(), 0);
}
