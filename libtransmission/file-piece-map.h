// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <algorithm> // for std::binary_search()
#include <cstdint> // for uint64_t
#include <cstddef> // for size_t
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/bitfield.h"
#include "libtransmission/tr-macros.h" // TR_CONSTEXPR20

struct tr_block_info;
struct tr_torrent_metainfo;

class tr_file_piece_map
{
public:
    template<typename T>
    struct index_span_t
    {
        T begin;
        T end;
    };
    using file_span_t = index_span_t<tr_file_index_t>;
    using piece_span_t = index_span_t<tr_piece_index_t>;

    template<typename T>
    struct offset_t
    {
        T index;
        uint64_t offset;
    };

    using file_offset_t = offset_t<tr_file_index_t>;
    explicit tr_file_piece_map(tr_torrent_metainfo const& tm);
    tr_file_piece_map(tr_block_info const& block_info, uint64_t const* file_sizes, size_t n_files);

    [[nodiscard]] TR_CONSTEXPR20 piece_span_t piece_span_for_file(tr_file_index_t const file) const noexcept
    {
        return file_pieces_[file];
    }

    [[nodiscard]] file_span_t file_span_for_piece(tr_piece_index_t piece) const;

    [[nodiscard]] file_offset_t file_offset(uint64_t offset) const;

    [[nodiscard]] TR_CONSTEXPR20 size_t file_count() const
    {
        return std::size(file_pieces_);
    }

    [[nodiscard]] TR_CONSTEXPR20 auto byte_span_for_file(tr_file_index_t const file) const
    {
        auto const& span = file_bytes_[file];
        return tr_byte_span_t{ span.begin, span.end };
    }

    [[nodiscard]] TR_CONSTEXPR20 bool is_edge_piece(tr_piece_index_t const piece) const
    {
        return std::binary_search(std::begin(edge_pieces_), std::end(edge_pieces_), piece);
    }

private:
    using byte_span_t = index_span_t<uint64_t>;

    void reset(tr_torrent_metainfo const& tm);
    void reset(tr_block_info const& block_info, uint64_t const* file_sizes, size_t n_files);

    std::vector<byte_span_t> file_bytes_;
    std::vector<piece_span_t> file_pieces_;
    std::vector<tr_piece_index_t> edge_pieces_;
};

class tr_file_priorities
{
public:
    explicit tr_file_priorities(tr_file_piece_map const* fpm) noexcept
        : fpm_{ fpm }
    {
    }

    void set(tr_file_index_t file, tr_priority_t priority);
    void set(tr_file_index_t const* files, size_t n, tr_priority_t priority);

    [[nodiscard]] tr_priority_t file_priority(tr_file_index_t file) const;
    [[nodiscard]] tr_priority_t piece_priority(tr_piece_index_t piece) const;

private:
    tr_file_piece_map const* fpm_;
    std::vector<tr_priority_t> priorities_;
};

class tr_files_wanted
{
public:
    explicit tr_files_wanted(tr_file_piece_map const* fpm);

    void set(tr_file_index_t file, bool wanted);
    void set(tr_file_index_t const* files, size_t n, bool wanted);

    [[nodiscard]] TR_CONSTEXPR20 bool file_wanted(tr_file_index_t file) const
    {
        return wanted_.test(file);
    }

    [[nodiscard]] bool piece_wanted(tr_piece_index_t piece) const;

private:
    tr_file_piece_map const* fpm_;
    tr_bitfield wanted_;
};
