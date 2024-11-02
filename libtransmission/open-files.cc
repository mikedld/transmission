// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::min
#include <array>
#include <cstdint> // uint8_t, uint64_t
#include <string_view>
#include <utility>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/error-types.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/open-files.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h" // _()

namespace
{

[[nodiscard]] auto is_open(tr_sys_file_t fd) noexcept
{
    return fd != TR_BAD_SYS_FILE;
}

bool preallocate_file_sparse(tr_sys_file_t fd, uint64_t length, tr_error* error)
{
    if (length == 0U)
    {
        return true;
    }

    auto local_error = tr_error{};

    if (tr_sys_file_preallocate(fd, length, TR_SYS_FILE_PREALLOC_SPARSE, &local_error))
    {
        return true;
    }

    tr_logAddDebug(fmt::format("Fast preallocation failed: {} ({})", local_error.message(), local_error.code()));

    if (!tr_error_is_enospc(local_error.code()))
    {
        static char constexpr Zero = '\0';

        local_error = {};

        /* fallback: the old-style seek-and-write */
        if (tr_sys_file_write_at(fd, &Zero, 1, length - 1, nullptr, &local_error) &&
            tr_sys_file_truncate(fd, length, &local_error))
        {
            return true;
        }

        tr_logAddDebug(fmt::format("Fast prellocation fallback failed: {} ({})", local_error.message(), local_error.code()));
    }

    if (error != nullptr)
    {
        *error = std::move(local_error);
    }

    return false;
}

bool preallocate_file_full(tr_sys_file_t fd, uint64_t length, tr_error* error)
{
    if (length == 0U)
    {
        return true;
    }

    auto local_error = tr_error{};

    if (tr_sys_file_preallocate(fd, length, 0, &local_error))
    {
        return true;
    }

    tr_logAddDebug(fmt::format("Full preallocation failed: {} ({})", local_error.message(), local_error.code()));

    if (!tr_error_is_enospc(local_error.code()))
    {
        auto buf = std::array<uint8_t, 4096>{};
        bool success = true;

        local_error = {};

        /* fallback: the old-fashioned way */
        while (success && length > 0)
        {
            uint64_t const this_pass = std::min(length, uint64_t{ std::size(buf) });
            uint64_t bytes_written = 0;
            success = tr_sys_file_write(fd, std::data(buf), this_pass, &bytes_written, &local_error);
            length -= bytes_written;
        }

        if (success)
        {
            return true;
        }

        tr_logAddDebug(fmt::format("Full preallocation fallback failed: {} ({})", local_error.message(), local_error.code()));
    }

    if (error != nullptr)
    {
        *error = std::move(local_error);
    }

    return false;
}

} // unnamed namespace

// ---

std::optional<tr_sys_file_t> tr_open_files::get(tr_torrent_id_t tor_id, tr_file_index_t file_num, bool writable)
{
    if (auto* const found = pool_.get(make_key(tor_id, file_num)); found != nullptr)
    {
        if (writable && !found->writable_)
        {
            return {};
        }

        return found->fd_;
    }

    return {};
}

std::optional<tr_sys_file_t> tr_open_files::get(
    tr_torrent_id_t tor_id,
    tr_file_index_t file_num,
    bool writable,
    std::string_view filename_in,
    Preallocation allocation,
    uint64_t file_size)
{
    // is there already an entry
    auto key = make_key(tor_id, file_num);
    if (auto* const found = pool_.get(key); found != nullptr)
    {
        if (!writable || found->writable_)
        {
            return found->fd_;
        }

        pool_.erase(key); // close so we can re-open as writable
    }

    // create subfolders, if any
    auto const filename = tr_pathbuf{ filename_in };
    auto error = tr_error{};
    if (writable)
    {
        auto dir = tr_pathbuf{ filename.sv() };
        dir.popdir();
        if (!tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0777, &error))
        {
            tr_logAddError(fmt::format(
                _("Couldn't create '{path}': {error} ({error_code})"),
                fmt::arg("path", dir),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
            return {};
        }
    }

    auto const info = tr_sys_path_get_info(filename);
    bool const already_existed = info && info->isFile();

    // we need write permissions to resize the file
    bool const resize_needed = already_existed && (file_size < info->size);
    writable |= resize_needed;

    // open the file
    int flags = writable ? (TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE) : 0;
    flags |= TR_SYS_FILE_READ;
    auto const fd = tr_sys_file_open(filename, flags, 0666, &error);
    if (!is_open(fd))
    {
        tr_logAddError(fmt::format(
            _("Couldn't open '{path}': {error} ({error_code})"),
            fmt::arg("path", filename),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code())));
        return {};
    }

    if (writable && !already_existed && allocation != Preallocation::None)
    {
        bool success = false;
        char const* type = nullptr;

        if (allocation == Preallocation::Full)
        {
            success = preallocate_file_full(fd, file_size, &error);
            type = "full";
        }
        else if (allocation == Preallocation::Sparse)
        {
            success = preallocate_file_sparse(fd, file_size, &error);
            type = "sparse";
        }

        TR_ASSERT(type != nullptr);

        if (!success)
        {
            tr_logAddError(fmt::format(
                _("Couldn't preallocate '{path}': {error} ({error_code})"),
                fmt::arg("path", filename),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
            tr_sys_file_close(fd);
            return {};
        }

        tr_logAddDebug(fmt::format("Preallocated file '{}' ({}, size: {})", filename, type, file_size));
    }

    // If the file already exists and it's too large, truncate it.
    // This is a fringe case that happens if a torrent's been updated
    // and one of the updated torrent's files is smaller.
    // https://trac.transmissionbt.com/ticket/2228
    // https://bugs.launchpad.net/ubuntu/+source/transmission/+bug/318249
    if (resize_needed && !tr_sys_file_truncate(fd, file_size, &error))
    {
        tr_logAddWarn(fmt::format(
            _("Couldn't truncate '{path}': {error} ({error_code})"),
            fmt::arg("path", filename),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code())));
        tr_sys_file_close(fd);
        return {};
    }

    // cache it
    auto& entry = pool_.add(std::move(key));
    entry.fd_ = fd;
    entry.writable_ = writable;

    return fd;
}

void tr_open_files::close_all()
{
    pool_.clear();
}

void tr_open_files::close_torrent(tr_torrent_id_t tor_id)
{
    pool_.erase_if([&tor_id](Key const& key, Val const& /*unused*/) { return key.first == tor_id; });
}

void tr_open_files::close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num)
{
    pool_.erase(make_key(tor_id, file_num));
}

tr_open_files::Val::~Val()
{
    if (is_open(fd_))
    {
        tr_sys_file_close(fd_);
    }
}
