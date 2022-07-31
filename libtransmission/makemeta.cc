// This file Copyright © 2010-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <event2/util.h> /* evutil_ascii_strcasecmp() */

#include <fmt/format.h>

#include "transmission.h"

#include "crypto-utils.h"
#include "error.h"
#include "file.h"
#include "log.h"
#include "makemeta.h"
#include "session.h" // TR_NAME
#include "tr-assert.h"
#include "utils.h"
#include "variant.h"
#include "version.h"

using namespace std::literals;

namespace
{

void walkTree(std::string_view top, std::string_view subpath, tr_torrent_files& files)
{
    if (std::empty(top) && std::empty(subpath))
    {
        return;
    }

    auto path = tr_pathbuf{ top };
    if (!std::empty(subpath))
    {
        path.append('/', subpath);
    }
    tr_sys_path_native_separators(std::data(path));

    auto info = tr_sys_path_info{};
    if (tr_error* error = nullptr; !tr_sys_path_get_info(path, 0, &info, &error))
    {
        tr_logAddWarn(fmt::format(
            _("Skipping '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", error->message),
            fmt::arg("error_code", error->code)));
        tr_error_free(error);
    }

    switch (info.type)
    {
    case TR_SYS_PATH_IS_DIRECTORY:
        if (tr_sys_dir_t odir = tr_sys_dir_open(path.c_str()); odir != TR_BAD_SYS_DIR)
        {
            char const* name = nullptr;
            while ((name = tr_sys_dir_read_name(odir)) != nullptr)
            {
                if (name[0] == '.') // skip dotfiles
                {
                    continue;
                }

                if (!std::empty(subpath))
                {
                    walkTree(top, tr_pathbuf{ subpath, '/', name }, files);
                }
                else
                {
                    walkTree(top, name, files);
                }
            }

            tr_sys_dir_close(odir);
        }
        break;

    case TR_SYS_PATH_IS_FILE:
        files.add(subpath, info.size);
        break;

    default:
        break;
    }
}

} // namespace

tr_metainfo_builder::tr_metainfo_builder(std::string_view top)
    : top_{ top }
{
    walkTree(top, {}, files_);
    block_info_ = tr_block_info{ files().totalSize(), defaultPieceSize(files_.totalSize()) };
}

uint32_t tr_metainfo_builder::defaultPieceSize(uint64_t totalSize)
{
    uint32_t const KiB = 1024;
    uint32_t const MiB = 1048576;
    uint32_t const GiB = 1073741824;

    if (totalSize >= 2 * GiB)
    {
        return 2 * MiB;
    }

    if (totalSize >= 1 * GiB)
    {
        return 1 * MiB;
    }

    if (totalSize >= 512 * MiB)
    {
        return 512 * KiB;
    }

    if (totalSize >= 350 * MiB)
    {
        return 256 * KiB;
    }

    if (totalSize >= 150 * MiB)
    {
        return 128 * KiB;
    }

    if (totalSize >= 50 * MiB)
    {
        return 64 * KiB;
    }

    return 32 * KiB; /* less than 50 meg */
}

bool tr_metainfo_builder::isLegalPieceSize(uint32_t x)
{
    // It must be a power of two and at least 16KiB
    static auto constexpr MinSize = uint32_t{ 1024U * 16U };
    auto const is_power_of_two = (x & (x - 1)) == 0;
    return x >= MinSize && is_power_of_two;
}

bool tr_metainfo_builder::setPieceSize(uint32_t piece_size)
{
    if (!isLegalPieceSize(piece_size))
    {
        return false;
    }

    block_info_ = tr_block_info{ files().totalSize(), piece_size };
    return true;
}

bool tr_metainfo_builder::makeChecksums(tr_error** error)
{
    checksum_piece_ = 0;
    cancel_ = false;

    auto const& files = this->files();
    auto const& block_info = this->blockInfo();

    if (files.totalSize() == 0U)
    {
        tr_error_set(error, ENOENT, tr_strerror(ENOENT));
        return false;
    }

    auto hashes = std::vector<std::byte>(std::size(tr_sha1_digest_t{}) * block_info.pieceCount());
    auto* walk = std::data(hashes);
    auto sha = tr_sha1::create();

    auto file_index = tr_file_index_t{ 0U };
    auto piece_index = tr_piece_index_t{ 0U };
    auto total_remain = files.totalSize();
    auto off = uint64_t{ 0U };

    auto buf = std::vector<char>(block_info.pieceSize());

    auto fd = tr_sys_file_open(
        tr_pathbuf{ top_, '/', files.path(file_index) },
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

        TR_ASSERT(piece_index < block_info.pieceCount());

        uint32_t const piece_size = block_info.pieceSize(piece_index);
        buf.resize(piece_size);
        auto* bufptr = std::data(buf);

        auto left_in_piece = piece_size;
        while (left_in_piece > 0U)
        {
            auto const n_this_pass = std::min(files.fileSize(file_index) - off, uint64_t{ left_in_piece });
            auto n_read = uint64_t{};

            (void)tr_sys_file_read(fd, bufptr, n_this_pass, &n_read, error);
            bufptr += n_read;
            off += n_read;
            left_in_piece -= n_read;

            if (off == files.fileSize(file_index))
            {
                off = 0;
                tr_sys_file_close(fd);
                fd = TR_BAD_SYS_FILE;

                if (++file_index < files.fileCount())
                {
                    fd = tr_sys_file_open(
                        tr_pathbuf{ top_, '/', files.path(file_index) },
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
        auto const digest = sha->final();
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
        tr_error_set(error, ECANCELED, tr_strerror(ECANCELED));
        return false;
    }

    piece_hashes_ = std::move(hashes);
    return true;
}

std::future<tr_error*> tr_metainfo_builder::makeChecksumsAsync()
{
    auto promise = std::promise<tr_error*>{};
    auto future = promise.get_future();
    std::thread work_thread(
        [this, promise = std::move(promise)]() mutable
        {
            tr_error* error = nullptr;
            makeChecksums(&error);
            promise.set_value(error);
        });
    work_thread.detach();
    return future;
}

std::string tr_metainfo_builder::benc(tr_error** error) const
{
    auto const anonymize = this->anonymize();
    auto const& block_info = this->blockInfo();
    auto const& comment = this->comment();
    auto const& files = this->files();
    auto const& source = this->source();
    auto const& webseeds = this->webseeds();

    if (block_info.totalSize() == 0)
    {
        tr_error_set(error, ENOENT, tr_strerror(ENOENT));
        return {};
    }

    auto top = tr_variant{};
    tr_variantInitDict(&top, 8);

    // add the announce-list trackers
    if (!std::empty(announceList()))
    {
        auto* const announce_list = tr_variantDictAddList(&top, TR_KEY_announce_list, 0);
        tr_variant* tier_list = nullptr;
        auto prev_tier = std::optional<tr_tracker_tier_t>{};
        for (auto const& tracker : announceList())
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

    if (is_private_)
    {
        tr_variantDictAddInt(&top, TR_KEY_private, 1);
    }

    if (!std::empty(source))
    {
        tr_variantDictAddStr(&top, TR_KEY_source, source_);
    }

    auto* const info_dict = tr_variantDictAddDict(&top, TR_KEY_info, 5);

    // "There is also a key `length` or a key `files`, but not both or neither.
    // If length is present then the download represents a single file,
    // otherwise it represents a set of files which go in a directory structure."
    if (files.fileCount() == 1U)
    {
        tr_variantDictAddInt(info_dict, TR_KEY_length, files.fileSize(0));
    }
    else
    {
        auto const n_files = files.fileCount();
        auto* const file_list = tr_variantDictAddList(info_dict, TR_KEY_files, n_files);

        for (tr_file_index_t i = 0; i < n_files; ++i)
        {
            auto* file_dict = tr_variantListAddDict(file_list, 2);
            tr_variantDictAddInt(file_dict, TR_KEY_length, files.fileSize(i));
            auto subpath = std::string_view{ files.path(i) };
            auto* path_list = tr_variantDictAddList(file_dict, TR_KEY_path, 0);
            auto token = std::string_view{};
            while (tr_strvSep(&subpath, &token, '/'))
            {
                tr_variantListAddStr(path_list, token);
            }
        }
    }

    if (auto const base = tr_sys_path_basename(top_); !std::empty(base))
    {
        tr_variantDictAddStr(info_dict, TR_KEY_name, base);
    }

    tr_variantDictAddInt(info_dict, TR_KEY_piece_length, block_info.pieceSize());
    tr_variantDictAddRaw(info_dict, TR_KEY_pieces, std::data(piece_hashes_), std::size(piece_hashes_));
    auto ret = tr_variantToStr(&top, TR_VARIANT_FMT_BENC);
    tr_variantFree(&top);
    return ret;
}

bool tr_metainfo_builder::save(std::string_view filename, tr_error** error) const
{
    return tr_saveFile(filename, benc(), error);
}
