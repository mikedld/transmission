// This file Copyright © 2010-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cerrno> // for ENOENT
#include <cmath>
#include <ctime> // time()
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/block-info.h" // tr_block_info
#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/makemeta.h"
#include "libtransmission/quark.h" // TR_KEY_length, TR_KEY_a...
#include "libtransmission/session.h" // TR_NAME
#include "libtransmission/torrent-files.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h" // tr_pathbuf
#include "libtransmission/utils.h" // for _()
#include "libtransmission/variant.h"
#include "libtransmission/version.h"

using namespace std::literals;

namespace
{

namespace find_files_helpers
{

struct TorrentFile
{
    TorrentFile(std::string_view subpath, size_t size)
        : subpath_{ subpath }
        , lowercase_{ tr_strlower(subpath) }
        , size_{ size }
    {
    }

    [[nodiscard]] auto operator<(TorrentFile const& that) const noexcept
    {
        return lowercase_ < that.lowercase_;
    }

    std::string subpath_;
    std::string lowercase_;
    uint64_t size_ = 0;
};

void walkTree(std::string_view const top, std::string_view const subpath, std::set<TorrentFile>& files)
{
    TR_ASSERT(!std::empty(top));
    TR_ASSERT(!std::empty(subpath));

    if (std::empty(top) || std::empty(subpath))
    {
        return;
    }

    auto path = tr_pathbuf{ top, '/', subpath };
    tr_sys_path_native_separators(std::data(path));
    tr_error* error = nullptr;
    auto const info = tr_sys_path_get_info(path, 0, &error);
    if (error != nullptr)
    {
        tr_logAddWarn(fmt::format(
            _("Skipping '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", error->message),
            fmt::arg("error_code", error->code)));
        tr_error_free(error);
    }
    if (!info)
    {
        return;
    }

    switch (info->type)
    {
    case TR_SYS_PATH_IS_DIRECTORY:
        for (auto const& name : tr_sys_dir_get_files(path))
        {
            if (!std::empty(subpath))
            {
                walkTree(top, tr_pathbuf{ subpath, '/', name }, files);
            }
            else
            {
                walkTree(top, name, files);
            }
        }
        break;

    case TR_SYS_PATH_IS_FILE:
        files.emplace(subpath, info->size);
        break;

    default:
        break;
    }
}

} // namespace find_files_helpers

tr_torrent_files findFiles(std::string_view const top, std::string_view const subpath)
{
    using namespace find_files_helpers;

    auto tmp = std::set<TorrentFile>{};
    walkTree(top, subpath, tmp);
    auto files = tr_torrent_files{};
    for (auto const& file : tmp)
    {
        files.add(file.subpath_, file.size_);
    }
    return files;
}

} // namespace

tr_metainfo_builder::tr_metainfo_builder(std::string_view single_file_or_parent_directory)
    : top_{ single_file_or_parent_directory }
{
    files_ = findFiles(tr_sys_path_dirname(top_), tr_sys_path_basename(top_));
    block_info_ = tr_block_info{ files_.totalSize(), default_piece_size(files_.totalSize()) };
}

bool tr_metainfo_builder::set_piece_size(uint32_t piece_size) noexcept
{
    if (!is_legal_piece_size(piece_size))
    {
        return false;
    }

    block_info_ = tr_block_info{ files_.totalSize(), piece_size };
    return true;
}

bool tr_metainfo_builder::blocking_make_checksums(tr_error** error)
{
    checksum_piece_ = 0;
    cancel_ = false;

    if (total_size() == 0U)
    {
        tr_error_set_from_errno(error, ENOENT);
        return false;
    }

    auto hashes = std::vector<std::byte>(std::size(tr_sha1_digest_t{}) * piece_count());
    auto* walk = std::data(hashes);
    auto sha = tr_sha1::create();

    auto file_index = tr_file_index_t{ 0U };
    auto piece_index = tr_piece_index_t{ 0U };
    auto total_remain = total_size();
    auto off = uint64_t{ 0U };

    auto buf = std::vector<char>(piece_size());

    auto const parent = tr_sys_path_dirname(top_);
    auto fd = tr_sys_file_open(
        tr_pathbuf{ parent, '/', path(file_index) },
        TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL,
        0,
        error);
    if (fd == TR_BAD_SYS_FILE)
    {
        return false;
    }

    while (!cancel_ && (total_remain > 0U))
    {
        checksum_piece_ = piece_index;

        TR_ASSERT(piece_index < piece_count());

        auto const piece_size = block_info_.piece_size(piece_index);
        buf.resize(piece_size);
        auto* bufptr = std::data(buf);

        auto left_in_piece = piece_size;
        while (left_in_piece > 0U)
        {
            auto const n_this_pass = std::min(file_size(file_index) - off, uint64_t{ left_in_piece });
            auto n_read = uint64_t{};

            (void)tr_sys_file_read(fd, bufptr, n_this_pass, &n_read, error);
            bufptr += n_read;
            off += n_read;
            left_in_piece -= n_read;

            if (off == file_size(file_index))
            {
                off = 0;
                tr_sys_file_close(fd);
                fd = TR_BAD_SYS_FILE;

                if (++file_index < file_count())
                {
                    fd = tr_sys_file_open(
                        tr_pathbuf{ parent, '/', path(file_index) },
                        TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL,
                        0,
                        error);
                    if (fd == TR_BAD_SYS_FILE)
                    {
                        return false;
                    }
                }
            }
        }

        TR_ASSERT(bufptr - std::data(buf) == (int)piece_size);
        TR_ASSERT(left_in_piece == 0);
        sha->add(std::data(buf), std::size(buf));
        auto const digest = sha->finish();
        walk = std::copy(std::begin(digest), std::end(digest), walk);
        sha->clear();

        total_remain -= piece_size;
        ++piece_index;
    }

    TR_ASSERT(cancel_ || size_t(walk - std::data(hashes)) == std::size(hashes));
    TR_ASSERT(cancel_ || total_remain == 0U);

    if (fd != TR_BAD_SYS_FILE)
    {
        tr_sys_file_close(fd);
    }

    if (cancel_)
    {
        tr_error_set_from_errno(error, ECANCELED);
        return false;
    }

    piece_hashes_ = std::move(hashes);
    return true;
}

std::string tr_metainfo_builder::benc(tr_error** error) const
{
    TR_ASSERT_MSG(!std::empty(piece_hashes_), "did you forget to call makeChecksums() first?");

    auto const anonymize = this->anonymize();
    auto const& comment = this->comment();
    auto const& source = this->source();
    auto const& webseeds = this->webseeds();

    if (total_size() == 0)
    {
        tr_error_set_from_errno(error, ENOENT);
        return {};
    }

    auto top = tr_variant::make_map(8U);

    // add the announce URLs
    if (!std::empty(announce_list()))
    {
        tr_variantDictAddStrView(&top, TR_KEY_announce, announce_list().at(0).announce.sv());
    }
    if (std::size(announce_list()) > 1U)
    {
        auto* const announce_list = tr_variantDictAddList(&top, TR_KEY_announce_list, 0);
        tr_variant* tier_list = nullptr;
        auto prev_tier = std::optional<tr_tracker_tier_t>{};
        for (auto const& tracker : this->announce_list())
        {
            if (!prev_tier || *prev_tier != tracker.tier)
            {
                tier_list = nullptr;
            }

            if (tier_list == nullptr)
            {
                prev_tier = tracker.tier;
                tier_list = tr_variantListAddList(announce_list, 0);
            }

            tr_variantListAddStr(tier_list, tracker.announce);
        }
    }

    // add the webseeds
    if (!std::empty(webseeds))
    {
        auto* const url_list = tr_variantDictAddList(&top, TR_KEY_url_list, std::size(webseeds));

        for (auto const& webseed : webseeds)
        {
            tr_variantListAddStr(url_list, webseed);
        }
    }

    // add the comment
    if (!std::empty(comment))
    {
        tr_variantDictAddStr(&top, TR_KEY_comment, comment);
    }

    // maybe add some optional metainfo
    if (!anonymize)
    {
        tr_variantDictAddStrView(&top, TR_KEY_created_by, TR_NAME "/" LONG_VERSION_STRING);
        tr_variantDictAddInt(&top, TR_KEY_creation_date, time(nullptr));
    }

    tr_variantDictAddStrView(&top, TR_KEY_encoding, "UTF-8");

    auto* const info_dict = tr_variantDictAddDict(&top, TR_KEY_info, 5);
    auto const base = tr_sys_path_basename(top_);

    // "There is also a key `length` or a key `files`, but not both or neither.
    // If length is present then the download represents a single file,
    // otherwise it represents a set of files which go in a directory structure."
    if (file_count() == 1U && !tr_strv_contains(path(0), '/'))
    {
        tr_variantDictAddInt(info_dict, TR_KEY_length, file_size(0));
    }
    else
    {
        auto const n_files = file_count();
        auto* const file_list = tr_variantDictAddList(info_dict, TR_KEY_files, n_files);

        for (tr_file_index_t i = 0; i < n_files; ++i)
        {
            auto* const file_dict = tr_variantListAddDict(file_list, 2);
            tr_variantDictAddInt(file_dict, TR_KEY_length, file_size(i));

            auto subpath = std::string_view{ path(i) };
            if (!std::empty(base))
            {
                subpath.remove_prefix(std::size(base) + std::size("/"sv));
            }

            auto* const path_list = tr_variantDictAddList(file_dict, TR_KEY_path, 0);
            auto token = std::string_view{};
            while (tr_strv_sep(&subpath, &token, '/'))
            {
                tr_variantListAddStr(path_list, token);
            }
        }
    }

    if (!std::empty(base))
    {
        tr_variantDictAddStr(info_dict, TR_KEY_name, base);
    }

    tr_variantDictAddInt(info_dict, TR_KEY_piece_length, piece_size());
    tr_variantDictAddRaw(info_dict, TR_KEY_pieces, std::data(piece_hashes_), std::size(piece_hashes_));

    if (is_private_)
    {
        tr_variantDictAddInt(info_dict, TR_KEY_private, 1);
    }

    if (!std::empty(source))
    {
        tr_variantDictAddStr(info_dict, TR_KEY_source, source_);
    }

    return tr_variant_serde::benc().to_string(top);
}

uint32_t tr_metainfo_builder::default_piece_size(uint64_t total_size) noexcept
{
    TR_ASSERT(total_size != 0);

    // Ideally, we want approximately 2^10 = 1024 pieces, give or take a few hundred pieces.
    // So we subtract 10 from the log2 of total size.
    // The ideal number of pieces is up for debate.
    auto exp = std::log2(total_size) - 10;

    // We want a piece size between 16KiB (2^14 bytes) and 16MiB (2^24 bytes) for maximum compatibility
    exp = std::clamp(exp, 14., 24.);

    return uint32_t{ 1U } << std::lround(exp);
}
