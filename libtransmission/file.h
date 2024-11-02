// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstdint> // uint64_t
#include <ctime> // time_t
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

struct tr_error;

/**
 * @addtogroup file_io File IO
 * @{
 */

#ifndef _WIN32

/** @brief Platform-specific file descriptor type. */
using tr_sys_file_t = int;
/** @brief Platform-specific invalid file descriptor constant. */
#define TR_BAD_SYS_FILE (-1)
/** @brief Platform-specific directory descriptor type. */
using tr_sys_dir_t = void*;

#else

using tr_sys_file_t = HANDLE;
#define TR_BAD_SYS_FILE INVALID_HANDLE_VALUE
struct tr_sys_dir_win32;
using tr_sys_dir_t = tr_sys_dir_win32*;

#endif

/** @brief Platform-specific invalid directory descriptor constant. */
#define TR_BAD_SYS_DIR ((tr_sys_dir_t) nullptr)

enum tr_sys_file_open_flags_t
{
    TR_SYS_FILE_READ = (1 << 0),
    TR_SYS_FILE_WRITE = (1 << 1),
    TR_SYS_FILE_CREATE = (1 << 2),
    TR_SYS_FILE_TRUNCATE = (1 << 3),
    TR_SYS_FILE_SEQUENTIAL = (1 << 4)
};

enum tr_sys_file_lock_flags_t
{
    TR_SYS_FILE_LOCK_SH = (1 << 0),
    TR_SYS_FILE_LOCK_EX = (1 << 1),
    TR_SYS_FILE_LOCK_NB = (1 << 2)
};

enum tr_sys_path_get_info_flags_t
{
    TR_SYS_PATH_NO_FOLLOW = (1 << 0)
};

enum tr_sys_file_preallocate_flags_t
{
    TR_SYS_FILE_PREALLOC_SPARSE = (1 << 0)
};

enum tr_sys_dir_create_flags_t
{
    TR_SYS_DIR_CREATE_PARENTS = (1 << 0)
};

enum tr_sys_path_type_t
{
    TR_SYS_PATH_IS_FILE,
    TR_SYS_PATH_IS_DIRECTORY,
    TR_SYS_PATH_IS_OTHER
};

struct tr_sys_path_info
{
    tr_sys_path_type_t type = {};
    uint64_t size = {};
    time_t last_modified_at = {};

    [[nodiscard]] constexpr auto isFile() const noexcept
    {
        return type == TR_SYS_PATH_IS_FILE;
    }

    [[nodiscard]] constexpr auto isFolder() const noexcept
    {
        return type == TR_SYS_PATH_IS_DIRECTORY;
    }
};

struct tr_sys_path_capacity
{
    int64_t free = -1;
    int64_t total = -1;
};

/**
 * @name Platform-specific wrapper functions
 *
 * Following functions accept paths in UTF-8 encoding and convert them to native
 * encoding internally if needed.
 * Descriptors returned (@ref tr_sys_file_t and @ref tr_sys_dir_t) may have
 * different type depending on platform and should generally not be passed to
 * native functions, but to wrapper functions instead.
 *
 * @{
 */

/* Path-related wrappers */

/**
 * @brief Portability wrapper for various in-kernel file copy functions, with a
 *        fallback to a userspace read/write loop.
 *
 * @param[in]  src_path  Path to source file.
 * @param[in]  dst_path  Path to destination file.
 * @param[out] error     Pointer to error object. Optional, pass `nullptr` if
 *                       you are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_path_copy(char const* src_path, char const* dst_path, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `stat()`.
 *
 * @param[in]  path  Path to file or directory.
 * @param[in]  flags Combination of @ref tr_sys_path_get_info_flags_t values.
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if you
 *                   are not interested in error details.
 *
 * @return info on success, or nullopt with `error` set accordingly.
 */
[[nodiscard]] std::optional<tr_sys_path_info> tr_sys_path_get_info(
    std::string_view path,
    int flags = 0,
    tr_error* error = nullptr);

/**
 * @brief Get disk capacity and free disk space (in bytes) for the specified folder.
 *
 * @param[in]  path  Path to directory.
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if you
 *                   are not interested in error details.
 */
[[nodiscard]] std::optional<tr_sys_path_capacity> tr_sys_path_get_capacity(std::string_view path, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `access()`.
 *
 * @param[in]  path  Path to file or directory.
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if you
 *                   are not interested in error details.
 *
 * @return `True` if path exists, `false` otherwise. Note that `false` will also
 *         be returned in case of error; if you need to distinguish the two,
 *         check if `error` is `nullptr` afterwards.
 */
bool tr_sys_path_exists(char const* path, tr_error* error = nullptr);

template<typename T, typename = decltype(&T::c_str)>
bool tr_sys_path_exists(T const& path, tr_error* error = nullptr)
{
    return tr_sys_path_exists(path.c_str(), error);
}

/**
 * @brief Check whether path is relative.
 *
 * This function only analyzes the string, so no error reporting is needed.
 *
 * @param[in] path Path to file or directory.
 *
 * @return `True` if path is relative, `false` otherwise
 */
bool tr_sys_path_is_relative(std::string_view path);

/**
 * @brief Test to see if the two filenames point to the same file.
 *
 * @param[in]  path1 Path to first file or directory.
 * @param[in]  path2 Path to second file or directory.
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if
 *                   you are not interested in error details.
 *
 * @return `True` if two paths point to the same file or directory, `false`
 *         otherwise. Note that `false` will also be returned in case of error;
 *         if you need to distinguish the two, check if `error` is `nullptr`
 *         afterwards.
 */
bool tr_sys_path_is_same(char const* path1, char const* path2, tr_error* error = nullptr);

template<typename T, typename U, typename = decltype(&T::c_str), typename = decltype(&U::c_str)>
bool tr_sys_path_is_same(T const& path1, U const& path2, tr_error* error = nullptr)
{
    return tr_sys_path_is_same(path1.c_str(), path2.c_str(), error);
}

/**
 * @brief Portability wrapper for `realpath()`.
 *
 * @param[in]  path  Path to file or directory.
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if you
 *                   are not interested in error details.
 *
 * @return Full path with symbolic links, `.` and `..` resolved on success,
 *         or an empty string otherwise (with `error` set accordingly).
 */
std::string tr_sys_path_resolve(std::string_view path, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `basename()`.
 *
 * @param[in]  path  Path to file or directory.
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if you
 *                   are not interested in error details.
 *
 * @return base name (last path component; parent path removed) on success,
 *         or empty string otherwise (with `error` set accordingly).
 */
std::string_view tr_sys_path_basename(std::string_view path, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `dirname()`.
 *
 * @param[in]  path  Path to file or directory.
 *
 * @return parent path substring of `path` (last path component removed) on
 *         success, or empty string otherwise with `error` set accordingly).
 */
std::string_view tr_sys_path_dirname(std::string_view path);

/**
 * @brief Portability wrapper for `rename()`.
 *
 * @param[in]  src_path Path to source file or directory.
 * @param[in]  dst_path Path to destination file or directory.
 * @param[out] error    Pointer to error object. Optional, pass `nullptr` if you
 *                      are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 *         Rename will generally only succeed if both source and destination are
 *         on the same partition.
 */
bool tr_sys_path_rename(char const* src_path, char const* dst_path, tr_error* error = nullptr);

template<typename T, typename U, typename = decltype(&T::c_str), typename = decltype(&U::c_str)>
bool tr_sys_path_rename(T const& src_path, U const& dst_path, tr_error* error = nullptr)
{
    return tr_sys_path_rename(src_path.c_str(), dst_path.c_str(), error);
}

/**
 * @brief Portability wrapper for `remove()`.
 *
 * @param[in]  path  Path to file or directory.
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if you
 *                   are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 *         Directory removal will only succeed if it is empty (contains no other
 *         files and directories).
 */
bool tr_sys_path_remove(char const* path, tr_error* error = nullptr);

template<typename T, typename = decltype(&T::c_str)>
bool tr_sys_path_remove(T const& path, tr_error* error = nullptr)
{
    return tr_sys_path_remove(path.c_str(), error);
}

/**
 * @brief Transform path separators to native ones, in-place.
 *
 * @param[in,out] path Path to transform.
 *
 * @return Same path but with native (and uniform) separators.
 */
char* tr_sys_path_native_separators(char* path);

/* File-related wrappers */

/**
 * @brief Portability wrapper for `open()`.
 *
 * @param[in]  path        Path to file.
 * @param[in]  flags       Combination of @ref tr_sys_file_open_flags_t values.
 * @param[in]  permissions Permissions to create file with (in case
                           @ref TR_SYS_FILE_CREATE is used). Not used on Windows.
 * @param[out] error       Pointer to error object. Optional, pass `nullptr` if
 *                         you are not interested in error details.
 *
 * @return Opened file descriptor on success, `TR_BAD_SYS_FILE` otherwise (with
 *         `error` set accordingly).
 */
tr_sys_file_t tr_sys_file_open(char const* path, int flags, int permissions, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `mkstemp()`.
 *
 * @param[in,out] path_template Template path to file. Should end with at least
 *                              six 'X' characters. Upon success, trailing 'X'
 *                              characters are replaced with actual random
 *                              characters used to form a unique path to
 *                              temporary file.
 * @param[out]    error         Pointer to error object. Optional, pass `nullptr`
 *                              if you are not interested in error details.
 *
 * @return Opened file descriptor on success, `TR_BAD_SYS_FILE` otherwise (with
 *         `error` set accordingly).
 */
tr_sys_file_t tr_sys_file_open_temp(char* path_template, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `close()`.
 *
 * @param[in]  handle Valid file descriptor.
 * @param[out] error  Pointer to error object. Optional, pass `nullptr` if you
 *                    are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_file_close(tr_sys_file_t handle, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `read()`.
 *
 * @param[in]  handle     Valid file descriptor.
 * @param[out] buffer     Buffer to store read data to.
 * @param[in]  size       Number of bytes to read.
 * @param[out] bytes_read Number of bytes actually read. Optional, pass `nullptr`
 *                        if you are not interested.
 * @param[out] error      Pointer to error object. Optional, pass `nullptr` if
 *                        you are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_file_read(tr_sys_file_t handle, void* buffer, uint64_t size, uint64_t* bytes_read, tr_error* error = nullptr);

/**
 * @brief Like `pread()`, except that the position is undefined afterwards.
 *        Not thread-safe.
 *
 * @param[in]  handle     Valid file descriptor.
 * @param[out] buffer     Buffer to store read data to.
 * @param[in]  size       Number of bytes to read.
 * @param[in]  offset     File offset in bytes to start reading from.
 * @param[out] bytes_read Number of bytes actually read. Optional, pass `nullptr`
 *                        if you are not interested.
 * @param[out] error      Pointer to error object. Optional, pass `nullptr` if
 *                        you are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_file_read_at(
    tr_sys_file_t handle,
    void* buffer,
    uint64_t size,
    uint64_t offset,
    uint64_t* bytes_read,
    tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `write()`.
 *
 * @param[in]  handle        Valid file descriptor.
 * @param[in]  buffer        Buffer to get data being written from.
 * @param[in]  size          Number of bytes to write.
 * @param[out] bytes_written Number of bytes actually written. Optional, pass
 *                           `nullptr` if you are not interested.
 * @param[out] error         Pointer to error object. Optional, pass `nullptr` if
 *                           you are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_file_write(
    tr_sys_file_t handle,
    void const* buffer,
    uint64_t size,
    uint64_t* bytes_written,
    tr_error* error = nullptr);

/**
 * @brief Like `pwrite()`, except that the position is undefined afterwards.
 *        Not thread-safe.
 *
 * @param[in]  handle        Valid file descriptor.
 * @param[in]  buffer        Buffer to get data being written from.
 * @param[in]  size          Number of bytes to write.
 * @param[in]  offset        File offset in bytes to start writing from.
 * @param[out] bytes_written Number of bytes actually written. Optional, pass
 *                           `nullptr` if you are not interested.
 * @param[out] error         Pointer to error object. Optional, pass `nullptr`
 *                          if you are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_file_write_at(
    tr_sys_file_t handle,
    void const* buffer,
    uint64_t size,
    uint64_t offset,
    uint64_t* bytes_written,
    tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `ftruncate()`.
 *
 * @param[in]  handle Valid file descriptor.
 * @param[in]  size   Number of bytes to truncate (or extend) file to.
 * @param[out] error  Pointer to error object. Optional, pass `nullptr` if you
 *                    are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_file_truncate(tr_sys_file_t handle, uint64_t size, tr_error* error = nullptr);

/**
 * @brief Preallocate file to specified size in full or sparse mode.
 *
 * @param[in]  handle Valid file descriptor.
 * @param[in]  size   Number of bytes to preallocate file to.
 * @param[in]  flags  Combination of @ref tr_sys_file_preallocate_flags_t values.
 * @param[out] error  Pointer to error object. Optional, pass `nullptr` if you
 *                    are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_file_preallocate(tr_sys_file_t handle, uint64_t size, int flags, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `flock()`.
 *
 * Don't try to upgrade or downgrade the lock unless you know what you are
 * doing, as behavior varies a bit between platforms.
 *
 * @param[in]  handle    Valid file descriptor.
 * @param[in]  operation Combination of @ref tr_sys_file_lock_flags_t values.
 * @param[out] error     Pointer to error object. Optional, pass `nullptr` if you
 *                       are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_file_lock(tr_sys_file_t handle, int operation, tr_error* error = nullptr);

/* Directory-related wrappers */

/**
 * @brief Portability wrapper for `getcwd()`.
 *
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if you are
 *                   not interested in error details.
 *
 * @return current directory on success, or an empty string otherwise
 *         (with `error` set accordingly).
 */
std::string tr_sys_dir_get_current(tr_error* error = nullptr);

/**
 * @brief Like `mkdir()`, but makes parent directories if needed.
 *
 * @param[in]  path        Path to directory.
 * @param[in]  flags       Combination of @ref tr_sys_dir_create_flags_t values.
 * @param[in]  permissions Permissions to create directory with. Not used on
                           Windows.
 * @param[out] error       Pointer to error object. Optional, pass `nullptr` if
 *                         you are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_dir_create(char const* path, int flags, int permissions, tr_error* error = nullptr);

template<typename T, typename = decltype(&T::c_str)>
bool tr_sys_dir_create(T const& path, int flags, int permissions, tr_error* error = nullptr)
{
    return tr_sys_dir_create(path.c_str(), flags, permissions, error);
}

/**
 * @brief Portability wrapper for `mkdtemp()`.
 *
 * @param[in,out] path_template Template path to directory. Should end with at
 *                              least six 'X' characters. Upon success, trailing
 *                              'X' characters are replaced with actual random
 *                              characters used to form a unique path to
 *                              temporary directory.
 * @param[out]    error         Pointer to error object. Optional, pass `nullptr`
 *                              if you are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_dir_create_temp(char* path_template, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `opendir()`.
 *
 * @param[in]  path  Path to directory.
 * @param[out] error Pointer to error object. Optional, pass `nullptr` if you are
 *                   not interested in error details.
 *
 * @return Opened directory descriptor on success, `TR_BAD_SYS_DIR` otherwise
 *         (with `error` set accordingly).
 */
tr_sys_dir_t tr_sys_dir_open(std::string_view path, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `readdir()`.
 *
 * @param[in]  handle Valid directory descriptor.
 * @param[out] error  Pointer to error object. Optional, pass `nullptr` if you
 *                    are not interested in error details.
 *
 * @return Pointer to next directory entry name (stored internally, DO NOT free
 *         it) on success, `nullptr` otherwise (with `error` set accordingly).
 *         Note that `nullptr` will also be returned in case of end of directory.
 *         If you need to distinguish the two, check if `error` is `nullptr`
 *         afterwards.
 */
char const* tr_sys_dir_read_name(tr_sys_dir_t handle, tr_error* error = nullptr);

/**
 * @brief Portability wrapper for `closedir()`.
 *
 * @param[in]  handle Valid directory descriptor.
 * @param[out] error  Pointer to error object. Optional, pass `nullptr` if you
 *                    are not interested in error details.
 *
 * @return `True` on success, `false` otherwise (with `error` set accordingly).
 */
bool tr_sys_dir_close(tr_sys_dir_t handle, tr_error* error = nullptr);

[[nodiscard]] constexpr bool tr_basename_is_not_dotfile(std::string_view sv)
{
    return std::empty(sv) || sv.front() != '.';
}

[[nodiscard]] std::vector<std::string> tr_sys_dir_get_files(
    std::string_view folder,
    std::function<bool(std::string_view name)> const& test = tr_basename_is_not_dotfile,
    tr_error* error = nullptr);

/** @} */
/** @} */
