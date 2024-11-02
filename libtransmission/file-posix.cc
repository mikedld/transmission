// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#undef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits> /* PATH_MAX */
#include <cstdint> /* SIZE_MAX */
#include <cstdio> // remove, rename
#include <cstdlib> // mkdtemp, mkstemp, realpath
#include <optional>
#include <string_view>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h> /* O_LARGEFILE, posix_fadvise(), [posix_]fallocate(), fcntl() */
#include <sys/stat.h>
#include <unistd.h> /* lseek(), write(), ftruncate(), pread(), pwrite(), pathconf(), etc */

#ifdef HAVE_FLOCK
#include <sys/file.h> /* flock() */
#endif

#ifdef HAVE_XFS_XFS_H
#include <xfs/xfs.h>
#endif

/* OS-specific file copy (copy_file_range, sendfile64, or copyfile). */
#if defined(__linux__)
#include <linux/version.h>
/* Linux's copy_file_range(2) is buggy prior to 5.3. */
#if defined(HAVE_COPY_FILE_RANGE) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
#define USE_COPY_FILE_RANGE
#elif defined(HAVE_SENDFILE64)
#include <sys/sendfile.h>
#define USE_SENDFILE64
#endif
#elif defined(__APPLE__) && defined(HAVE_COPYFILE)
#include <copyfile.h>
#ifndef COPYFILE_CLONE /* macos < 10.12 */
#define COPYFILE_CLONE 0
#endif
#define USE_COPYFILE
#elif defined(HAVE_COPY_FILE_RANGE)
/* Presently this is only FreeBSD 13+. */
#define USE_COPY_FILE_RANGE
#endif /* __linux__ */

#include <fmt/core.h>

#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h" // TR_UCLIBC_CHECK_VERSION
#include "libtransmission/tr-strbuf.h"

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_SEQUENTIAL
#define O_SEQUENTIAL 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* don't use pread/pwrite on old versions of uClibc because they're buggy.
 * https://trac.transmissionbt.com/ticket/3826 */
#if defined(__UCLIBC__) && !TR_UCLIBC_CHECK_VERSION(0, 9, 28)
#undef HAVE_PREAD
#undef HAVE_PWRITE
#endif

#ifdef __APPLE__
#ifndef HAVE_PREAD
#define HAVE_PREAD
#endif
#ifndef HAVE_PWRITE
#define HAVE_PWRITE
#endif
#ifndef HAVE_MKDTEMP
#define HAVE_MKDTEMP
#endif
#endif

using namespace std::literals;

namespace
{
void set_file_for_single_pass(tr_sys_file_t handle)
{
    /* Set hints about the lookahead buffer and caching. It's okay
       for these to fail silently, so don't let them affect errno */

    int const err = errno;

    if (handle == TR_BAD_SYS_FILE)
    {
        return;
    }

#ifdef HAVE_POSIX_FADVISE

    (void)posix_fadvise(handle, 0, 0, POSIX_FADV_SEQUENTIAL);

#endif

#ifdef __APPLE__

    (void)fcntl(handle, F_RDAHEAD, 1);
    (void)fcntl(handle, F_NOCACHE, 1);

#endif

    errno = err;
}
} // namespace

bool tr_sys_path_exists(char const* path, tr_error* error)
{
    TR_ASSERT(path != nullptr);

    bool const ret = access(path, F_OK) != -1;

    if (error != nullptr && !ret && errno != ENOENT)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

std::optional<tr_sys_path_info> tr_sys_path_get_info(std::string_view path, int flags, tr_error* error)
{
    struct stat sb = {};

    bool ok = false;
    auto const szpath = tr_pathbuf{ path };

    if ((flags & TR_SYS_PATH_NO_FOLLOW) == 0)
    {
        ok = stat(szpath, &sb) != -1;
    }
    else
    {
        ok = lstat(szpath, &sb) != -1;
    }

    if (!ok)
    {
        if (error != nullptr)
        {
            error->set_from_errno(errno);
        }

        return {};
    }

    auto info = tr_sys_path_info{};

    if (S_ISREG(sb.st_mode))
    {
        info.type = TR_SYS_PATH_IS_FILE;
    }
    else if (S_ISDIR(sb.st_mode))
    {
        info.type = TR_SYS_PATH_IS_DIRECTORY;
    }
    else
    {
        info.type = TR_SYS_PATH_IS_OTHER;
    }

    info.size = static_cast<uint64_t>(sb.st_size);
    info.last_modified_at = sb.st_mtime;

    return info;
}

bool tr_sys_path_is_relative(std::string_view path)
{
    return std::empty(path) || path.front() != '/';
}

bool tr_sys_path_is_same(char const* path1, char const* path2, tr_error* error)
{
    TR_ASSERT(path1 != nullptr);
    TR_ASSERT(path2 != nullptr);

    bool ret = false;
    struct stat sb1 = {};
    struct stat sb2 = {};

    if (stat(path1, &sb1) != -1 && stat(path2, &sb2) != -1)
    {
        ret = sb1.st_dev == sb2.st_dev && sb1.st_ino == sb2.st_ino;
    }
    else if (error != nullptr && errno != ENOENT)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

std::string tr_sys_path_resolve(std::string_view path, tr_error* error)
{
    auto const szpath = tr_pathbuf{ path };
    auto buf = std::array<char, PATH_MAX>{};

    if (auto const* const ret = realpath(szpath, std::data(buf)); ret != nullptr)
    {
        return ret;
    }

    if (error != nullptr)
    {
        error->set_from_errno(errno);
    }

    return {};
}

std::string_view tr_sys_path_basename(std::string_view path, tr_error* /*error*/)
{
    // As per the basename() manpage:
    // If path [is] an empty string, then basename() return[s] the string "."
    if (std::empty(path))
    {
        return "."sv;
    }

    // Remove all trailing slashes.
    // If nothing is left, return "/"
    if (auto pos = path.find_last_not_of('/'); pos != std::string_view::npos)
    {
        path = path.substr(0, pos + 1);
    }
    else // all slashes
    {
        return "/"sv;
    }

    if (auto pos = path.find_last_of('/'); pos != std::string_view::npos)
    {
        path.remove_prefix(pos + 1);
    }

    return std::empty(path) ? "/"sv : path;
}

// This function is adapted from Node.js's path.posix.dirname() function,
// which is copyrighted by Joyent, Inc. and other Node contributors
// and is distributed under MIT (SPDX:MIT) license.
std::string_view tr_sys_path_dirname(std::string_view path)
{
    auto const len = std::size(path);

    if (len == 0U)
    {
        return "."sv;
    }

    auto const has_root = path[0] == '/';
    auto end = std::string_view::npos;
    auto matched_slash = true;

    for (auto i = len - 1; i >= 1U; --i)
    {
        if (path[i] == '/')
        {
            if (!matched_slash)
            {
                end = i;
                break;
            }
        }
        else
        {
            // We saw the first non-path separator
            matched_slash = false;
        }
    }

    if (end == std::string_view::npos)
    {
        return has_root ? "/"sv : "."sv;
    }

    if (has_root && end == 1)
    {
        return "//"sv;
    }

    return path.substr(0, end);
}

bool tr_sys_path_rename(char const* src_path, char const* dst_path, tr_error* error)
{
    TR_ASSERT(src_path != nullptr);
    TR_ASSERT(dst_path != nullptr);

    bool const ret = rename(src_path, dst_path) != -1;

    if (error != nullptr && !ret)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

/* We try to do a fast (in-kernel) copy using a variety of non-portable system
 * calls. If the current implementation does not support in-kernel copying, we
 * use a user-space fallback instead. */
bool tr_sys_path_copy(char const* src_path, char const* dst_path, tr_error* error)
{
    TR_ASSERT(src_path != nullptr);
    TR_ASSERT(dst_path != nullptr);

    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

#if defined(USE_COPYFILE)
    if (copyfile(src_path, dst_path, nullptr, COPYFILE_CLONE | COPYFILE_ALL) < 0)
    {
        error->set_from_errno(errno);
        return false;
    }

    return true;

#else /* USE_COPYFILE */

    auto const info = tr_sys_path_get_info(src_path, 0, error);
    if (!info)
    {
        error->prefix_message("Unable to get information on source file: ");
        return false;
    }

    /* Other OSes require us to copy between file descriptors, so open them. */
    tr_sys_file_t const in = tr_sys_file_open(src_path, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
    if (in == TR_BAD_SYS_FILE)
    {
        error->prefix_message("Unable to open source file: ");
        return false;
    }

    tr_sys_file_t const out = tr_sys_file_open(
        dst_path,
        TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE,
        0666,
        error);
    if (out == TR_BAD_SYS_FILE)
    {
        tr_sys_file_close(in);
        error->prefix_message("Unable to open destination file: ");
        return false;
    }

    uint64_t file_size = info->size;
    int errno_cpy = 0; /* keep errno intact across copy attempts */

#if defined(USE_COPY_FILE_RANGE)

    /* Kernel copy by copy_file_range */
    /* try this first if available, no need to check previous copy attempts */
    /* might throw EXDEV when used between filesystems in most kernels */

    while (file_size > 0U)
    {
        size_t const chunk_size = std::min({ file_size, uint64_t{ SSIZE_MAX }, uint64_t{ INT32_MAX } });
        auto const copied = copy_file_range(in, nullptr, out, nullptr, chunk_size, 0);

        TR_ASSERT(copied == -1 || copied >= 0); /* -1 for error; some non-negative value otherwise. */

        if (copied == -1)
        {
            errno_cpy = errno; /* remember me for later */
            if (errno != EXDEV) /* EXDEV is expected, don't log error */
            {
                error->set_from_errno(errno);
            }
            if (file_size > 0U)
            {
                file_size = info->size; /* restore file_size for next fallback */
            }
            break; /* break copy */
        }

        TR_ASSERT(copied >= 0 && ((uint64_t)copied) <= file_size);
        TR_ASSERT(copied >= 0 && ((uint64_t)copied) <= chunk_size);
        file_size -= copied;
    } /* end file_size loop */
    /* at this point errno_cpy is either set or file_size is 0 due to while condition */

#endif /* USE_COPY_FILE_RANGE */

#if defined(USE_SENDFILE64)

    /* Kernel copy by sendfile64 */
    /* if file_size>0 and errno_cpy==0, we probably never entered any previous copy attempt, also: */
    /* if we (still) got something to copy and we encountered certain error in copy_file_range */

    /* duplicated code, this could be refactored in a function */
    /* keeping the copy paths in blocks helps with error tracing though */
    /* trying sendfile after EXDEV is more efficient than falling to user-space straight away */

    if (file_size > 0U && (errno_cpy == 0 || errno_cpy == EXDEV))
    {
        /* set file offsets to 0 in case previous copy did move them */
        /* TR_BAD_SYS_FILE has previously been checked, we can directly lseek */
        /* in case we have no pending errno, be kind enough to report any error */
        if (lseek(in, 0, SEEK_SET) == -1 || lseek(out, 0, SEEK_SET) == -1)
        {
            if (errno_cpy == 0)
            {
                error->set_from_errno(errno);
            }
        }
        else
        {
            while (file_size > 0U)
            {
                size_t const chunk_size = std::min({ file_size, uint64_t{ SSIZE_MAX }, uint64_t{ INT32_MAX } });
                auto const copied = sendfile64(out, in, nullptr, chunk_size);
                TR_ASSERT(copied == -1 || copied >= 0); /* -1 for error; some non-negative value otherwise. */

                if (copied == -1)
                {
                    errno_cpy = errno; /* remember me for later */
                    /* use sendfile with EINVAL (invalid argument) for falling to user-space copy */
                    if (errno != EINVAL) /* EINVAL is expected on some 32-bit devices (of Synology), don't log error */
                    {
                        error->set_from_errno(errno);
                    }
                    if (file_size > 0U)
                    {
                        file_size = info->size; /* restore file_size for next fallback */
                    }
                    break; /* break copy */
                }

                TR_ASSERT(copied >= 0 && ((uint64_t)copied) <= file_size);
                TR_ASSERT(copied >= 0 && ((uint64_t)copied) <= chunk_size);
                file_size -= copied;
            } /* end file_size loop */
        } /* end lseek error */
    } /* end fallback check */
    /* at this point errno_cpy is either set or file_size is 0 due to while condition */

#endif /* USE_SENDFILE64 */

    /* Fallback to user-space copy. */
    /* if file_size>0 and errno_cpy==0, we probably never entered any copy attempt, also: */
    /* if we (still) got something to copy and we encountered certain error in previous attempts */
    if (file_size > 0U && (errno_cpy == 0 || errno_cpy == EXDEV || errno_cpy == EINVAL))
    {
        static auto constexpr Buflen = size_t{ 1024U * 1024U }; /* 1024 KiB buffer */
        auto buf = std::vector<char>{};
        buf.resize(Buflen);

        /* set file offsets to 0 in case previous copy did move them */
        /* TR_BAD_SYS_FILE has previously been checked, we can directly lseek */
        /* in case we have no pending errno, be kind enough to report any error */
        if (lseek(in, 0, SEEK_SET) == -1 || lseek(out, 0, SEEK_SET) == -1)
        {
            if (errno_cpy == 0)
            {
                error->set_from_errno(errno);
            }
        }
        else
        {
            while (file_size > 0U)
            {
                uint64_t const chunk_size = std::min(file_size, uint64_t{ Buflen });
                uint64_t bytes_read = 0;
                uint64_t bytes_written = 0;

                if (!tr_sys_file_read(in, std::data(buf), chunk_size, &bytes_read, error))
                {
                    break;
                }

                if (!tr_sys_file_write(out, std::data(buf), bytes_read, &bytes_written, error))
                {
                    break;
                }

                TR_ASSERT(bytes_read == bytes_written);
                TR_ASSERT(bytes_written <= file_size);
                file_size -= bytes_written;
            } /* end file_size loop */
        } /* end lseek error */
    } /* end fallback check */

    /* all copy paths will end here, file_size>0 signifies error */

    /* cleanup */
    tr_sys_file_close(out);
    tr_sys_file_close(in);

    if (file_size != 0)
    {
        error->prefix_message("Unable to read/write: ");
        return false;
    }

    return true;

#endif /* USE_COPYFILE */
}

bool tr_sys_path_remove(char const* path, tr_error* error)
{
    TR_ASSERT(path != nullptr);

    bool const ret = remove(path) != -1;

    if (error != nullptr && !ret)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

char* tr_sys_path_native_separators(char* path)
{
    return path;
}

tr_sys_file_t tr_sys_file_open(char const* path, int flags, int permissions, tr_error* error)
{
    TR_ASSERT(path != nullptr);
    TR_ASSERT((flags & (TR_SYS_FILE_READ | TR_SYS_FILE_WRITE)) != 0);

    struct native_map_item
    {
        int symbolic_mask;
        int symbolic_value;
        int native_value;
    };

    auto constexpr NativeMap = std::array<native_map_item, 7U>{
        { { TR_SYS_FILE_READ | TR_SYS_FILE_WRITE, TR_SYS_FILE_READ | TR_SYS_FILE_WRITE, O_RDWR },
          { TR_SYS_FILE_READ | TR_SYS_FILE_WRITE, TR_SYS_FILE_READ, O_RDONLY },
          { TR_SYS_FILE_READ | TR_SYS_FILE_WRITE, TR_SYS_FILE_WRITE, O_WRONLY },
          { TR_SYS_FILE_CREATE, TR_SYS_FILE_CREATE, O_CREAT },
          { TR_SYS_FILE_TRUNCATE, TR_SYS_FILE_TRUNCATE, O_TRUNC },
          { TR_SYS_FILE_SEQUENTIAL, TR_SYS_FILE_SEQUENTIAL, O_SEQUENTIAL } }
    };

    int native_flags = O_BINARY | O_LARGEFILE | O_CLOEXEC;

    for (auto const& item : NativeMap)
    {
        if ((flags & item.symbolic_mask) == item.symbolic_value)
        {
            native_flags |= item.native_value;
        }
    }

    tr_sys_file_t const ret = open(path, native_flags, permissions);

    if (ret != TR_BAD_SYS_FILE)
    {
        if ((flags & TR_SYS_FILE_SEQUENTIAL) != 0)
        {
            set_file_for_single_pass(ret);
        }
    }
    else if (error != nullptr)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

tr_sys_file_t tr_sys_file_open_temp(char* path_template, tr_error* error)
{
    TR_ASSERT(path_template != nullptr);

    tr_sys_file_t const ret = mkstemp(path_template);

    if (error != nullptr && ret == TR_BAD_SYS_FILE)
    {
        error->set_from_errno(errno);
    }

    set_file_for_single_pass(ret);

    return ret;
}

bool tr_sys_file_close(tr_sys_file_t handle, tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_FILE);

    bool const ret = close(handle) != -1;

    if (error != nullptr && !ret)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

bool tr_sys_file_read(tr_sys_file_t handle, void* buffer, uint64_t size, uint64_t* bytes_read, tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_FILE);
    TR_ASSERT(buffer != nullptr || size == 0);

    bool ret = false;

    auto const my_bytes_read = read(handle, buffer, size);
    static_assert(sizeof(*bytes_read) >= sizeof(my_bytes_read));

    if (my_bytes_read != -1)
    {
        if (bytes_read != nullptr)
        {
            *bytes_read = my_bytes_read;
        }

        ret = true;
    }
    else if (error != nullptr)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

bool tr_sys_file_read_at(
    tr_sys_file_t handle,
    void* buffer,
    uint64_t size,
    uint64_t offset,
    uint64_t* bytes_read,
    tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_FILE);
    TR_ASSERT(buffer != nullptr || size == 0);
    /* seek requires signed offset, so it should be in mod range */
    TR_ASSERT(offset < UINT64_MAX / 2);

    bool ret = false;

#ifdef HAVE_PREAD

    auto const my_bytes_read = pread(handle, buffer, size, offset);

#else

    ssize_t const my_bytes_read = lseek(handle, offset, SEEK_SET) == -1 ? -1 : read(handle, buffer, size);

#endif

    static_assert(sizeof(*bytes_read) >= sizeof(my_bytes_read));

    if (my_bytes_read > 0)
    {
        if (bytes_read != nullptr)
        {
            *bytes_read = my_bytes_read;
        }

        ret = true;
    }
    else if (error != nullptr && my_bytes_read == -1)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

bool tr_sys_file_write(tr_sys_file_t handle, void const* buffer, uint64_t size, uint64_t* bytes_written, tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_FILE);
    TR_ASSERT(buffer != nullptr || size == 0);

    bool ret = false;

    auto const my_bytes_written = write(handle, buffer, size);
    static_assert(sizeof(*bytes_written) >= sizeof(my_bytes_written));

    if (my_bytes_written != -1)
    {
        if (bytes_written != nullptr)
        {
            *bytes_written = my_bytes_written;
        }

        ret = true;
    }
    else if (error != nullptr)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

bool tr_sys_file_write_at(
    tr_sys_file_t handle,
    void const* buffer,
    uint64_t size,
    uint64_t offset,
    uint64_t* bytes_written,
    tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_FILE);
    TR_ASSERT(buffer != nullptr || size == 0);
    /* seek requires signed offset, so it should be in mod range */
    TR_ASSERT(offset < UINT64_MAX / 2);

    bool ret = false;

#ifdef HAVE_PWRITE

    auto const my_bytes_written = pwrite(handle, buffer, size, offset);

#else

    ssize_t const my_bytes_written = lseek(handle, offset, SEEK_SET) == -1 ? -1 : write(handle, buffer, size);

#endif

    static_assert(sizeof(*bytes_written) >= sizeof(my_bytes_written));

    if (my_bytes_written != -1)
    {
        if (bytes_written != nullptr)
        {
            *bytes_written = my_bytes_written;
        }

        ret = true;
    }
    else if (error != nullptr)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

bool tr_sys_file_truncate(tr_sys_file_t handle, uint64_t size, tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_FILE);

    bool const ret = ftruncate(handle, size) != -1;

    if (error != nullptr && !ret)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

namespace
{
namespace preallocate_helpers
{
#ifdef HAVE_FALLOCATE64
bool preallocate_fallocate64(tr_sys_file_t handle, uint64_t size)
{
    return fallocate64(handle, 0, 0, size) == 0;
}
#endif

#ifdef HAVE_XFS_XFS_H
bool full_preallocate_xfs(tr_sys_file_t handle, uint64_t size)
{
    if (platform_test_xfs_fd(handle) == 0) // true if on xfs filesystem
    {
        return false;
    }

    xfs_flock64_t fl;
    fl.l_whence = 0;
    fl.l_start = 0;
    fl.l_len = size;

    // The blocks are allocated, but not zeroed, and the file size does not change
    bool ok = xfsctl(nullptr, handle, XFS_IOC_RESVSP64, &fl) != -1;

    if (ok)
    {
        ok = ftruncate(handle, size) == 0;
    }

    return ok;
}
#endif

#ifdef __APPLE__
bool full_preallocate_apple(tr_sys_file_t handle, uint64_t size)
{
    fstore_t fst;

    fst.fst_flags = F_ALLOCATEALL;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = size;
    fst.fst_bytesalloc = 0;

    bool ok = fcntl(handle, F_PREALLOCATE, &fst) != -1;

    if (ok)
    {
        ok = ftruncate(handle, size) == 0;
    }

    return ok;
}
#endif

#ifdef HAVE_POSIX_FALLOCATE
bool full_preallocate_posix(tr_sys_file_t handle, uint64_t size)
{
    return posix_fallocate(handle, 0, size) == 0;
}
#endif
} // namespace preallocate_helpers
} // unnamed namespace

bool tr_sys_file_preallocate(tr_sys_file_t handle, uint64_t size, int flags, tr_error* error)
{
    using namespace preallocate_helpers;

    TR_ASSERT(handle != TR_BAD_SYS_FILE);

    using prealloc_func = bool (*)(tr_sys_file_t, uint64_t);

    // these approaches are fast and should be tried first
    auto approaches = std::vector<prealloc_func>{
#ifdef HAVE_FALLOCATE64
        preallocate_fallocate64
#endif
    };

    // these approaches are sometimes slower in some settings (e.g.
    // a slow zeroing of all the preallocated space) so only use them
    // if specified by `flags`
    if ((flags & TR_SYS_FILE_PREALLOC_SPARSE) == 0)
    {
        // TODO: these functions haven't been reviewed in awhile.
        // It's possible that some are faster now & should be promoted
        // to 'always try' and/or replaced with fresher platform API.
        approaches.insert(
            std::end(approaches),
            {
#ifdef HAVE_XFS_XFS_H
                full_preallocate_xfs,
#endif
#ifdef __APPLE__
                full_preallocate_apple,
#endif
#ifdef HAVE_POSIX_FALLOCATE
                full_preallocate_posix,
#endif
            });
    }

    for (auto& approach : approaches) // try until one of them works
    {
        errno = 0;

        if (auto const success = approach(handle, size); success)
        {
            return success;
        }

        if (errno == ENOSPC) // disk full, so subsequent approaches will fail too
        {
            break;
        }
    }

    if (error != nullptr)
    {
        error->set_from_errno(errno ? errno : ENOSYS);
    }

    return false;
}

bool tr_sys_file_lock([[maybe_unused]] tr_sys_file_t handle, [[maybe_unused]] int operation, tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_FILE);
    TR_ASSERT((operation & ~(TR_SYS_FILE_LOCK_SH | TR_SYS_FILE_LOCK_EX | TR_SYS_FILE_LOCK_NB)) == 0);
    TR_ASSERT(!!(operation & TR_SYS_FILE_LOCK_SH) + !!(operation & TR_SYS_FILE_LOCK_EX) == 1);

#if defined(F_OFD_SETLK)

    struct flock fl = {};

    switch (operation & (TR_SYS_FILE_LOCK_SH | TR_SYS_FILE_LOCK_EX))
    {
    case TR_SYS_FILE_LOCK_SH:
        fl.l_type = F_RDLCK;
        break;

    case TR_SYS_FILE_LOCK_EX:
        fl.l_type = F_WRLCK;
        break;

    default:
        errno = EINVAL;
        break;
    }

    fl.l_whence = SEEK_SET;

    int const native_operation = (operation & TR_SYS_FILE_LOCK_NB) != 0 ? F_OFD_SETLK : F_OFD_SETLKW;

    auto result = std::optional<bool>{};
    while (!result)
    {
        if (fcntl(handle, native_operation, &fl) != -1)
        {
            result = true;
        }
        else if (errno != EINTR)
        {
            result = false;
        }
    }

#elif defined(HAVE_FLOCK)

    int const native_operation = //
        (((operation & TR_SYS_FILE_LOCK_SH) != 0) ? LOCK_SH : 0) | //
        (((operation & TR_SYS_FILE_LOCK_EX) != 0) ? LOCK_EX : 0) | //
        (((operation & TR_SYS_FILE_LOCK_NB) != 0) ? LOCK_NB : 0);

    auto result = std::optional<bool>{};
    while (!result)
    {
        if (flock(handle, native_operation) != -1)
        {
            result = true;
        }
        else if (errno != EINTR)
        {
            result = false;
        }
    }

#else

    errno = ENOSYS;
    auto const result = std::optional<bool>{ false };

#endif

    TR_ASSERT(result);
    if (!*result && errno == EAGAIN)
    {
        errno = EWOULDBLOCK;
    }

    if (error != nullptr && !*result)
    {
        error->set_from_errno(errno);
    }

    return *result;
}

std::string tr_sys_dir_get_current(tr_error* error)
{
    auto buf = std::vector<char>{};
    buf.resize(PATH_MAX);

    for (;;)
    {
        if (char const* const ret = getcwd(std::data(buf), std::size(buf)); ret != nullptr)
        {
            return ret;
        }

        if (errno == ERANGE)
        {
            buf.resize(std::size(buf) * 2U);
            continue;
        }

        if (error != nullptr)
        {
            error->set_from_errno(errno);
        }

        return {};
    }
}

namespace
{
#ifndef HAVE_MKDIRP
[[nodiscard]] bool tr_mkdirp_(std::string_view path, int permissions, tr_error* error)
{
    auto walk = path.find_first_not_of('/'); // walk past the root
    auto subpath = tr_pathbuf{};

    while (walk < std::size(path))
    {
        auto const end = path.find('/', walk);
        subpath.assign(path.substr(0, end));
        auto const info = tr_sys_path_get_info(subpath, 0);
        if (info && !info->isFolder())
        {
            if (error != nullptr)
            {
                error->set(ENOTDIR, fmt::format("File is in the way: {:s}", path));
            }

            return false;
        }
        if (!info && mkdir(subpath, permissions) == -1)
        {
            if (error != nullptr)
            {
                error->set_from_errno(errno);
            }

            return false;
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        walk = end + 1;
    }

    return true;
}
#endif
} // namespace

bool tr_sys_dir_create(char const* path, int flags, int permissions, tr_error* error)
{
    TR_ASSERT(path != nullptr);

    auto ret = false;
    auto local_error = tr_error{};

    if ((flags & TR_SYS_DIR_CREATE_PARENTS) != 0)
    {
#ifdef HAVE_MKDIRP
        ret = mkdirp(path, permissions) != -1;
#else
        ret = tr_mkdirp_(path, permissions, &local_error);
#endif
    }
    else
    {
        ret = mkdir(path, permissions) != -1;
    }

    if (!ret && errno == EEXIST)
    {
        if (auto const info = tr_sys_path_get_info(path); info && info->type == TR_SYS_PATH_IS_DIRECTORY)
        {
            local_error = {};
            ret = true;
        }
        else
        {
            errno = EEXIST;
        }
    }

    if (error != nullptr && !ret)
    {
        if (!local_error)
        {
            local_error.set_from_errno(errno);
        }

        *error = std::move(local_error);
    }

    return ret;
}

bool tr_sys_dir_create_temp(char* path_template, tr_error* error)
{
    TR_ASSERT(path_template != nullptr);

#ifdef HAVE_MKDTEMP

    bool const ret = mkdtemp(path_template) != nullptr;

#else

    bool const ret = mktemp(path_template) != nullptr && mkdir(path_template, 0700) != -1;

#endif

    if (error != nullptr && !ret)
    {
        error->set_from_errno(errno);
    }

    return ret;
}

tr_sys_dir_t tr_sys_dir_open(std::string_view path, tr_error* error)
{
    if (auto* const ret = opendir(tr_pathbuf{ path }); ret != nullptr)
    {
        return (tr_sys_dir_t)ret;
    }

    if (error != nullptr)
    {
        error->set_from_errno(errno);
    }

    return TR_BAD_SYS_DIR;
}

char const* tr_sys_dir_read_name(tr_sys_dir_t handle, tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_DIR);

    errno = 0;

    if (auto const* const entry = readdir(static_cast<DIR*>(handle)); entry != nullptr)
    {
        return entry->d_name;
    }

    if (error != nullptr && errno != 0)
    {
        error->set_from_errno(errno);
    }

    return {};
}

bool tr_sys_dir_close(tr_sys_dir_t handle, tr_error* error)
{
    TR_ASSERT(handle != TR_BAD_SYS_DIR);

    if (auto const ret = closedir(static_cast<DIR*>(handle)) != -1; ret)
    {
        return ret;
    }

    if (error != nullptr)
    {
        error->set_from_errno(errno);
    }

    return {};
}
