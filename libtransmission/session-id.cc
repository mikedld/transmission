// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <string_view>
#include <iterator> // for std::back_inserter

#ifndef _WIN32
#include <cerrno>
#include <sys/stat.h>
#endif

#include <fmt/core.h>

#include "libtransmission/crypto-utils.h" // for tr_rand_obj()
#include "libtransmission/error-types.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/platform.h"
#include "libtransmission/session-id.h"
#include "libtransmission/tr-strbuf.h" // for tr_pathbuf
#include "libtransmission/utils.h" // for _()

using namespace std::literals;

namespace
{

void get_lockfile_path(std::string_view session_id, tr_pathbuf& path)
{
    fmt::format_to(std::back_inserter(path), "{:s}/tr_session_id_{:s}", tr_getSessionIdDir(), session_id);
}

tr_sys_file_t create_lockfile(std::string_view session_id)
{
    if (std::empty(session_id))
    {
        return TR_BAD_SYS_FILE;
    }

    auto lockfile_path = tr_pathbuf{};
    get_lockfile_path(session_id, lockfile_path);

    auto error = tr_error{};
    auto lockfile_fd = tr_sys_file_open(lockfile_path, TR_SYS_FILE_READ | TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0600, &error);

    if (lockfile_fd != TR_BAD_SYS_FILE)
    {
        if (tr_sys_file_lock(lockfile_fd, TR_SYS_FILE_LOCK_EX | TR_SYS_FILE_LOCK_NB, &error))
        {
#ifndef _WIN32
            /* Allow any user to lock the file regardless of current umask */
            fchmod(lockfile_fd, 0644);
#endif
        }
        else
        {
            tr_sys_file_close(lockfile_fd);
            lockfile_fd = TR_BAD_SYS_FILE;
        }
    }

    if (error)
    {
        tr_logAddWarn(fmt::format(
            _("Couldn't create '{path}': {error} ({error_code})"),
            fmt::arg("path", lockfile_path),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code())));
    }

    return lockfile_fd;
}

void destroy_lockfile(tr_sys_file_t lockfile_fd, std::string_view session_id)
{
    if (lockfile_fd != TR_BAD_SYS_FILE)
    {
        tr_sys_file_close(lockfile_fd);
    }

    if (!std::empty(session_id))
    {
        auto lockfile_path = tr_pathbuf{};
        get_lockfile_path(session_id, lockfile_path);
        tr_sys_path_remove(lockfile_path);
    }
}

#ifndef _WIN32
auto constexpr WouldBlock = EWOULDBLOCK;
#else
auto constexpr WouldBlock = ERROR_LOCK_VIOLATION;
#endif

} // namespace

tr_session_id::session_id_t tr_session_id::make_session_id()
{
    auto session_id = tr_rand_obj<session_id_t>();
    static auto constexpr Pool = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"sv;
    for (auto& chr : session_id)
    {
        chr = Pool[static_cast<unsigned char>(chr) % std::size(Pool)];
    }
    session_id.back() = '\0';
    return session_id;
}

tr_session_id::~tr_session_id()
{
    destroy_lockfile(current_lock_file_, std::data(current_value_));
    destroy_lockfile(previous_lock_file_, std::data(previous_value_));
}

bool tr_session_id::is_local(std::string_view session_id) noexcept
{
    if (std::empty(session_id))
    {
        return false;
    }

    auto is_local = false;
    auto lockfile_path = tr_pathbuf{};
    get_lockfile_path(session_id, lockfile_path);
    auto error = tr_error{};
    if (auto lockfile_fd = tr_sys_file_open(lockfile_path, TR_SYS_FILE_READ, 0, &error); lockfile_fd == TR_BAD_SYS_FILE)
    {
        if (tr_error_is_enoent(error.code()))
        {
            error = {};
        }
    }
    else
    {
        if (!tr_sys_file_lock(lockfile_fd, TR_SYS_FILE_LOCK_SH | TR_SYS_FILE_LOCK_NB, &error) && (error.code() == WouldBlock))
        {
            is_local = true;
            error = {};
        }

        tr_sys_file_close(lockfile_fd);
    }

    if (error)
    {
        tr_logAddWarn(fmt::format(
            _("Couldn't open session lock file '{path}': {error} ({error_code})"),
            fmt::arg("path", lockfile_path),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code())));
    }

    return is_local;
}

std::string_view tr_session_id::sv() const noexcept
{
    if (auto const now = get_current_time_(); now >= expires_at_)
    {
        destroy_lockfile(previous_lock_file_, std::data(previous_value_));
        previous_value_ = current_value_;
        previous_lock_file_ = current_lock_file_;
        current_value_ = make_session_id();
        current_lock_file_ = create_lockfile(std::data(current_value_));
        expires_at_ = now + SessionIdDurationSec;
    }

    // -1 to strip the '\0'
    return std::string_view{ std::data(current_value_), std::size(current_value_) - 1 };
}

char const* tr_session_id::c_str() const noexcept
{
    return std::data(sv()); // current_value_ is zero-terminated
}
