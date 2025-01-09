// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstdint> // uint32_t, uint64_t
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/block-info.h"
#include "libtransmission/magnet-metainfo.h"
#include "libtransmission/torrent-files.h"
#include "libtransmission/tr-macros.h"

struct tr_error;

struct tr_torrent_metainfo : public tr_magnet_metainfo
{
public:
    [[nodiscard]] TR_CONSTEXPR20 auto empty() const noexcept
    {
        return std::empty(files_);
    }

    bool parse_benc(std::string_view benc, tr_error* error = nullptr);

    // Helper function wrapper around parseBenc().
    // If you're looping through several files, passing in a non-nullptr
    // `contents` can reduce the number of memory allocations needed to
    // load multiple files.
    bool parse_torrent_file(std::string_view benc_filename, std::vector<char>* contents = nullptr, tr_error* error = nullptr);

    // FILES

    [[nodiscard]] constexpr auto const& files() const noexcept
    {
        return files_;
    }
    [[nodiscard]] TR_CONSTEXPR20 auto file_count() const noexcept
    {
        return files().file_count();
    }
    [[nodiscard]] TR_CONSTEXPR20 auto file_size(tr_file_index_t i) const
    {
        return files().file_size(i);
    }
    [[nodiscard]] TR_CONSTEXPR20 auto const& file_subpath(tr_file_index_t i) const
    {
        return files().path(i);
    }

    void set_file_subpath(tr_file_index_t i, std::string_view subpath)
    {
        files_.set_path(i, subpath);
    }

    /// BLOCK INFO

    [[nodiscard]] constexpr auto const& block_info() const noexcept
    {
        return block_info_;
    }

    [[nodiscard]] constexpr auto block_count() const noexcept
    {
        return block_info().block_count();
    }
    [[nodiscard]] constexpr auto byte_loc(uint64_t nth_byte) const noexcept
    {
        return block_info().byte_loc(nth_byte);
    }
    [[nodiscard]] constexpr auto block_loc(tr_block_index_t block) const noexcept
    {
        return block_info().block_loc(block);
    }
    [[nodiscard]] constexpr auto piece_loc(tr_piece_index_t piece, uint32_t offset = 0, uint32_t length = 0) const noexcept
    {
        return block_info().piece_loc(piece, offset, length);
    }
    [[nodiscard]] constexpr auto block_size(tr_block_index_t block) const noexcept
    {
        return block_info().block_size(block);
    }
    [[nodiscard]] constexpr auto block_span_for_piece(tr_piece_index_t piece) const noexcept
    {
        return block_info().block_span_for_piece(piece);
    }
    [[nodiscard]] constexpr auto piece_count() const noexcept
    {
        return block_info().piece_count();
    }
    [[nodiscard]] constexpr auto piece_size() const noexcept
    {
        return block_info().piece_size();
    }
    [[nodiscard]] constexpr auto piece_size(tr_piece_index_t piece) const noexcept
    {
        return block_info().piece_size(piece);
    }
    [[nodiscard]] constexpr auto total_size() const noexcept
    {
        return block_info().total_size();
    }

    // OTHER PROPERTIES

    [[nodiscard]] constexpr auto const& comment() const noexcept
    {
        return comment_;
    }
    [[nodiscard]] constexpr auto const& creator() const noexcept
    {
        return creator_;
    }
    [[nodiscard]] constexpr auto const& source() const noexcept
    {
        return source_;
    }

    [[nodiscard]] constexpr auto is_private() const noexcept
    {
        return is_private_;
    }

    [[nodiscard]] TR_CONSTEXPR20 tr_sha1_digest_t const& piece_hash(tr_piece_index_t piece) const
    {
        return pieces_[piece];
    }

    [[nodiscard]] TR_CONSTEXPR20 bool has_v1_metadata() const noexcept
    {
        // need 'pieces' field and 'files' or 'length'
        // TODO check for 'files' or 'length'
        return !std::empty(pieces_);
    }

    [[nodiscard]] constexpr bool has_v2_metadata() const noexcept
    {
        return is_v2_;
    }

    [[nodiscard]] constexpr auto const& date_created() const noexcept
    {
        return date_created_;
    }

    [[nodiscard]] constexpr auto info_dict_size() const noexcept
    {
        return info_dict_size_;
    }

    [[nodiscard]] constexpr auto info_dict_offset() const noexcept
    {
        return info_dict_offset_;
    }

    [[nodiscard]] constexpr auto pieces_offset() const noexcept
    {
        return pieces_offset_;
    }

    // UTILS

    [[nodiscard]] auto torrent_file(std::string_view torrent_dir = {}) const
    {
        return make_filename(torrent_dir, name(), info_hash_string(), BasenameFormat::Hash, ".torrent");
    }

    [[nodiscard]] auto magnet_file(std::string_view torrent_dir = {}) const
    {
        return make_filename(torrent_dir, name(), info_hash_string(), BasenameFormat::Hash, ".magnet");
    }

    [[nodiscard]] auto resume_file(std::string_view resume_dir = {}) const
    {
        return make_filename(resume_dir, name(), info_hash_string(), BasenameFormat::Hash, ".resume");
    }

    static bool migrate_file(
        std::string_view dirname,
        std::string_view name,
        std::string_view info_hash_string,
        std::string_view suffix);

    static void remove_file(
        std::string_view dirname,
        std::string_view name,
        std::string_view info_hash_string,
        std::string_view suffix);

private:
    friend struct MetainfoHandler;
    static bool parse_impl(tr_torrent_metainfo& setme, std::string_view benc, tr_error* error);
    static std::string fix_webseed_url(tr_torrent_metainfo const& tm, std::string_view url);

    enum class BasenameFormat : uint8_t
    {
        Hash,
        NameAndPartialHash
    };

    [[nodiscard]] static std::string make_filename(
        std::string_view dirname,
        std::string_view name,
        std::string_view info_hash_string,
        BasenameFormat format,
        std::string_view suffix);

    [[nodiscard]] auto make_filename(std::string_view dirname, BasenameFormat format, std::string_view suffix) const
    {
        return make_filename(dirname, name(), info_hash_string(), format, suffix);
    }

    tr_block_info block_info_ = tr_block_info{ 0, 0 };

    tr_torrent_files files_;

    std::vector<tr_sha1_digest_t> pieces_;

    std::string comment_;
    std::string creator_;
    std::string source_;

    time_t date_created_ = 0;

    // Offset + size of the bencoded info dict subset of the bencoded data.
    // Used when loading pieces of it to sent to magnet peers.
    // See https://www.bittorrent.org/beps/bep_0009.html
    uint64_t info_dict_size_ = 0;
    uint64_t info_dict_offset_ = 0;

    // Offset of the bencoded 'pieces' checksums subset of the bencoded data.
    // Used when loading piece checksums on demand.
    uint64_t pieces_offset_ = 0;

    bool has_magnet_info_hash_ = false;
    bool is_private_ = false;
    bool is_v2_ = false;
};
