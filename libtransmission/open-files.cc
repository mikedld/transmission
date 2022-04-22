// This file Copyright © 2005-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint> // uint8_t, uint64_t

#include <iostream>

#include <fmt/core.h>

#include "transmission.h"

#include "error-types.h"
#include "error.h"
#include "file.h"
#include "log.h"
#include "open-files.h"
#include "tr-assert.h"
#include "tr-strbuf.h"
#include "utils.h" // _()

namespace
{

bool isOpen(tr_sys_file_t fd)
{
    return fd != TR_BAD_SYS_FILE;
}

bool preallocate_file_sparse(tr_sys_file_t fd, uint64_t length, tr_error** error)
{
    tr_error* my_error = nullptr;

    if (length == 0)
    {
        return true;
    }

    if (tr_sys_file_preallocate(fd, length, TR_SYS_FILE_PREALLOC_SPARSE, &my_error))
    {
        return true;
    }

    tr_logAddDebug(fmt::format("Fast preallocation failed: {} ({})", my_error->message, my_error->code));

    if (!TR_ERROR_IS_ENOSPC(my_error->code))
    {
        char const zero = '\0';

        tr_error_clear(&my_error);

        /* fallback: the old-style seek-and-write */
        if (tr_sys_file_write_at(fd, &zero, 1, length - 1, nullptr, &my_error) && tr_sys_file_truncate(fd, length, &my_error))
        {
            return true;
        }

        tr_logAddDebug(fmt::format("Fast prellocation fallback failed: {} ({})", my_error->message, my_error->code));
    }

    tr_error_propagate(error, &my_error);
    return false;
}

bool preallocate_file_full(tr_sys_file_t fd, uint64_t length, tr_error** error)
{
    tr_error* my_error = nullptr;

    if (length == 0)
    {
        return true;
    }

    if (tr_sys_file_preallocate(fd, length, 0, &my_error))
    {
        return true;
    }

    tr_logAddDebug(fmt::format("Full preallocation failed: {} ({})", my_error->message, my_error->code));

    if (!TR_ERROR_IS_ENOSPC(my_error->code))
    {
        auto buf = std::array<uint8_t, 4096>{};
        bool success = true;

        tr_error_clear(&my_error);

        /* fallback: the old-fashioned way */
        while (success && length > 0)
        {
            uint64_t const thisPass = std::min(length, uint64_t{ std::size(buf) });
            uint64_t bytes_written = 0;
            success = tr_sys_file_write(fd, std::data(buf), thisPass, &bytes_written, &my_error);
            length -= bytes_written;
        }

        if (success)
        {
            return true;
        }

        tr_logAddDebug(fmt::format("Full preallocation fallback failed: {} ({})", my_error->message, my_error->code));
    }

    tr_error_propagate(error, &my_error);
    return false;
}

uint64_t next_sequence_ = 1;

} // unnamed namespace

///

std::optional<tr_sys_file_t> tr_open_files::get(tr_torrent_id_t tor_id, tr_file_index_t file_num, bool writable)
{
    std::cerr << __FILE__ << ':' << __LINE__ << std::endl;
    if (auto const it = find(makeKey(tor_id, file_num));
        (it != std::end(files_)) && isOpen(it->fd_) && (!writable || it->writable_))
    {
        std::cerr << __FILE__ << ':' << __LINE__ << std::endl;
        return it->fd_;
    }

    std::cerr << __FILE__ << ':' << __LINE__ << std::endl;
    return {};
}

std::optional<tr_sys_file_t> tr_open_files::get(
    tr_torrent_id_t tor_id,
    tr_file_index_t file_num,
    bool writable,
    std::string_view filename_in,
    tr_preallocation_mode allocation,
    uint64_t file_size)
{
    // is there already an entry
    auto const key = makeKey(tor_id, file_num);
    if (auto it = find(key); it != std::end(files_) && isOpen(it->fd_))
    {
        if (!writable || it->writable_)
        {
            return it->fd_;
        }

        it->close(); // close so we can re-open as writable
    }

    // create subfolders, if any
    auto const filename = tr_pathbuf{ filename_in };
    tr_error* error = nullptr;
    if (writable)
    {
        auto const dir = tr_sys_path_dirname(filename, &error);

        if (std::empty(dir))
        {
            tr_logAddError(fmt::format(
                _("Couldn't create '{path}': {error} ({error_code})"),
                fmt::arg("path", filename),
                fmt::arg("error", error->message),
                fmt::arg("error_code", error->code)));
            tr_error_free(error);
            std::cerr << __FILE__ << ':' << __LINE__ << ' ' << error->message << std::endl;
            return {};
        }

        if (!tr_sys_dir_create(dir.c_str(), TR_SYS_DIR_CREATE_PARENTS, 0777, &error))
        {
            tr_logAddError(fmt::format(
                _("Couldn't create '{path}': {error} ({error_code})"),
                fmt::arg("path", dir),
                fmt::arg("error", error->message),
                fmt::arg("error_code", error->code)));
            tr_error_free(error);
            std::cerr << __FILE__ << ':' << __LINE__ << ' ' << error->message << std::endl;
            return {};
        }
    }

    auto info = tr_sys_path_info{};
    bool const already_existed = tr_sys_path_get_info(filename, 0, &info) && info.type == TR_SYS_PATH_IS_FILE;

    // we need write permissions to resize the file
    bool const resize_needed = already_existed && (file_size < info.size);
    writable |= resize_needed;

    // open the file
    int flags = writable ? (TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE) : 0;
    flags |= TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL;
    std::cerr << __FILE__ << ':' << __LINE__ << " filename [" << filename << ']' << std::endl;
    auto const fd = tr_sys_file_open(filename, flags, 0666, &error);
    std::cerr << __FILE__ << ':' << __LINE__ << " fd " << fd << std::endl;
    if (!isOpen(fd))
    {
        tr_logAddError(fmt::format(
            _("Couldn't open '{path}': {error} ({error_code})"),
            fmt::arg("path", filename),
            fmt::arg("error", error->message),
            fmt::arg("error_code", error->code)));
        tr_error_free(error);
        std::cerr << __FILE__ << ':' << __LINE__ << ' ' << error->message << std::endl;
        return {};
    }

    if (writable && !already_existed && allocation != TR_PREALLOCATE_NONE)
    {
        bool success = false;
        char const* type = nullptr;

        if (allocation == TR_PREALLOCATE_FULL)
        {
            success = preallocate_file_full(fd, file_size, &error);
            type = "full";
        }
        else if (allocation == TR_PREALLOCATE_SPARSE)
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
                fmt::arg("error", error->message),
                fmt::arg("error_code", error->code)));
            tr_error_free(error);
            tr_sys_file_close(fd);
            std::cerr << __FILE__ << ':' << __LINE__ << ' ' << error->message << std::endl;
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
            fmt::arg("error", error->message),
            fmt::arg("error_code", error->code)));
        tr_error_free(error);
        tr_sys_file_close(fd);
        std::cerr << __FILE__ << ':' << __LINE__ << ' ' << error->message << std::endl;
        return {};
    }

    // cache it
    auto& entry = getFreeSlot();
    entry.key_ = key;
    entry.fd_ = fd;
    entry.writable_ = writable;
    entry.sequence_ = next_sequence_++;

    std::cerr << __FILE__ << ':' << __LINE__ << ' ' << fd << std::endl;
    return fd;
}

tr_open_files::Entry& tr_open_files::getFreeSlot()
{
    auto const it = std::min_element(
        std::begin(files_),
        std::end(files_),
        [](auto const& a, auto const& b) { return a.sequence_ < b.sequence_; });
    it->close();
    return *it;
}

tr_open_files::Files::iterator tr_open_files::find(Key const& key)
{
    return std::find_if(std::begin(files_), std::end(files_), [&key](auto const& item) { return item.key_ == key; });
}

void tr_open_files::closeAll()
{
    std::for_each(std::begin(files_), std::end(files_), [](auto& file) { file.close(); });
}

void tr_open_files::closeTorrent(tr_torrent_id_t tor_id)
{
    for (auto& file : files_)
    {
        if (file.key_.first == tor_id)
        {
            file.close();
        }
    }
}

void tr_open_files::closeFile(tr_torrent_id_t tor_id, tr_file_index_t file_num)
{
    if (auto const it = find(makeKey(tor_id, file_num)); it != std::end(files_))
    {
        it->close();
    }
}

tr_open_files::Entry::~Entry()
{
    close();
}

void tr_open_files::Entry::close()
{
    if (isOpen(fd_))
    {
        tr_sys_file_close(fd_);
    }

    key_ = {};
    fd_ = TR_BAD_SYS_FILE;
    writable_ = false;
    sequence_ = 0;
}
