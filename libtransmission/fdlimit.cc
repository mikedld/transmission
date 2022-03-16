// This file Copyright © 2005-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <ctime>

#include <fmt/core.h>

#include "transmission.h"

#include "error-types.h"
#include "error.h"
#include "fdlimit.h"
#include "file.h"
#include "log.h"
#include "session.h"
#include "torrent.h" /* tr_isTorrent() */
#include "tr-assert.h"
#include "utils.h" // tr_time()

/***
****
****  Local Files
****
***/

static bool preallocate_file_sparse(tr_sys_file_t fd, uint64_t length, tr_error** error)
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

static bool preallocate_file_full(tr_sys_file_t fd, uint64_t length, tr_error** error)
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

/*****
******
******
******
*****/

struct tr_cached_file
{
    bool is_writable;
    tr_sys_file_t fd;
    int torrent_id;
    tr_file_index_t file_index;
    time_t used_at;
};

static constexpr bool cached_file_is_open(struct tr_cached_file const* o)
{
    TR_ASSERT(o != nullptr);

    return (o != nullptr) && (o->fd != TR_BAD_SYS_FILE);
}

static void cached_file_close(struct tr_cached_file* o)
{
    TR_ASSERT(cached_file_is_open(o));

    if (o != nullptr)
    {
        tr_sys_file_close(o->fd, nullptr);
        o->fd = TR_BAD_SYS_FILE;
    }
}

/**
 * returns 0 on success, or an errno value on failure.
 * errno values include ENOENT if the parent folder doesn't exist,
 * plus the errno values set by tr_sys_dir_create () and tr_sys_file_open ().
 */
// TODO: remove goto
static int cached_file_open(
    struct tr_cached_file* o,
    char const* filename,
    bool writable,
    tr_preallocation_mode allocation,
    uint64_t file_size)
{
    int flags = 0;
    tr_sys_path_info info = {};
    bool already_existed = false;
    bool resize_needed = false;
    tr_sys_file_t fd = TR_BAD_SYS_FILE;
    tr_error* error = nullptr;

    /* create subfolders, if any */
    if (writable)
    {
        char* dir = tr_sys_path_dirname(filename, &error);

        if (dir == nullptr)
        {
            tr_logAddError(fmt::format(
                _("Couldn't create '{path}': {error} ({error_code})"),
                fmt::arg("path", filename),
                fmt::arg("error", error->message),
                fmt::arg("error_code", error->code)));
            goto FAIL;
        }

        if (!tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0777, &error))
        {
            tr_logAddError(fmt::format(
                _("Couldn't create '{path}': {error} ({error_code})"),
                fmt::arg("path", dir),
                fmt::arg("error", error->message),
                fmt::arg("error_code", error->code)));
            tr_free(dir);
            goto FAIL;
        }

        tr_free(dir);
    }

    already_existed = tr_sys_path_get_info(filename, 0, &info, nullptr) && info.type == TR_SYS_PATH_IS_FILE;

    /* we can't resize the file w/o write permissions */
    resize_needed = already_existed && (file_size < info.size);
    writable |= resize_needed;

    /* open the file */
    flags = writable ? (TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE) : 0;
    flags |= TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL;
    fd = tr_sys_file_open(filename, flags, 0666, &error);

    if (fd == TR_BAD_SYS_FILE)
    {
        tr_logAddError(fmt::format(
            _("Couldn't open '{path}': {error} ({error_code})"),
            fmt::arg("path", filename),
            fmt::arg("error", error->message),
            fmt::arg("error_code", error->code)));
        goto FAIL;
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
            goto FAIL;
        }

        tr_logAddDebug(fmt::format("Preallocated file '{}' ({}, size: {})", filename, type, file_size));
    }

    /* If the file already exists and it's too large, truncate it.
     * This is a fringe case that happens if a torrent's been updated
     * and one of the updated torrent's files is smaller.
     * https://trac.transmissionbt.com/ticket/2228
     * https://bugs.launchpad.net/ubuntu/+source/transmission/+bug/318249
     */
    if (resize_needed && !tr_sys_file_truncate(fd, file_size, &error))
    {
        tr_logAddWarn(fmt::format(
            _("Couldn't truncate '{path}': {error} ({error_code})"),
            fmt::arg("path", filename),
            fmt::arg("error", error->message),
            fmt::arg("error_code", error->code)));
        goto FAIL;
    }

    o->fd = fd;
    return 0;

FAIL:
    int const err = error->code;
    tr_error_free(error);

    if (fd != TR_BAD_SYS_FILE)
    {
        tr_sys_file_close(fd, nullptr);
    }

    return err;
}

/***
****
***/

struct tr_fileset
{
    struct tr_cached_file* begin;
    struct tr_cached_file const* end;
};

static void fileset_construct(struct tr_fileset* set, int n)
{
    set->begin = tr_new(struct tr_cached_file, n);
    set->end = set->begin + n;

    for (struct tr_cached_file* o = set->begin; o != set->end; ++o)
    {
        *o = { false, TR_BAD_SYS_FILE, 0, 0, 0 };
    }
}

static void fileset_close_all(struct tr_fileset* set)
{
    if (set != nullptr)
    {
        for (struct tr_cached_file* o = set->begin; o != set->end; ++o)
        {
            if (cached_file_is_open(o))
            {
                cached_file_close(o);
            }
        }
    }
}

static void fileset_destruct(struct tr_fileset* set)
{
    fileset_close_all(set);
    tr_free(set->begin);
    set->end = set->begin = nullptr;
}

static void fileset_close_torrent(struct tr_fileset* set, int torrent_id)
{
    if (set != nullptr)
    {
        for (struct tr_cached_file* o = set->begin; o != set->end; ++o)
        {
            if (o->torrent_id == torrent_id && cached_file_is_open(o))
            {
                cached_file_close(o);
            }
        }
    }
}

static struct tr_cached_file* fileset_lookup(struct tr_fileset* set, int torrent_id, tr_file_index_t i)
{
    if (set != nullptr)
    {
        for (struct tr_cached_file* o = set->begin; o != set->end; ++o)
        {
            if (torrent_id == o->torrent_id && i == o->file_index && cached_file_is_open(o))
            {
                return o;
            }
        }
    }

    return nullptr;
}

static struct tr_cached_file* fileset_get_empty_slot(struct tr_fileset* set)
{
    struct tr_cached_file* cull = nullptr;

    if (set != nullptr && set->begin != nullptr)
    {
        /* try to find an unused slot */
        for (struct tr_cached_file* o = set->begin; o != set->end; ++o)
        {
            if (!cached_file_is_open(o))
            {
                return o;
            }
        }

        /* all slots are full... recycle the least recently used */
        for (struct tr_cached_file* o = set->begin; o != set->end; ++o)
        {
            if (cull == nullptr || o->used_at < cull->used_at)
            {
                cull = o;
            }
        }

        cached_file_close(cull);
    }

    return cull;
}

/***
****
****  Startup / Shutdown
****
***/

struct tr_fdInfo
{
    int peerCount;
    struct tr_fileset fileset;
};

static void ensureSessionFdInfoExists(tr_session* session)
{
    TR_ASSERT(tr_isSession(session));

    if (session->fdInfo == nullptr)
    {
        int const FILE_CACHE_SIZE = 32;

        /* Create the local file cache */
        auto* const i = tr_new0(struct tr_fdInfo, 1);
        fileset_construct(&i->fileset, FILE_CACHE_SIZE);
        session->fdInfo = i;
    }
}

void tr_fdClose(tr_session* session)
{
    if (session != nullptr && session->fdInfo != nullptr)
    {
        struct tr_fdInfo* i = session->fdInfo;
        fileset_destruct(&i->fileset);
        tr_free(i);
        session->fdInfo = nullptr;
    }
}

/***
****
***/

static struct tr_fileset* get_fileset(tr_session* session)
{
    if (session == nullptr)
    {
        return nullptr;
    }

    ensureSessionFdInfoExists(session);
    return &session->fdInfo->fileset;
}

void tr_fdFileClose(tr_session* s, tr_torrent const* tor, tr_file_index_t i)
{
    tr_cached_file* const o = fileset_lookup(get_fileset(s), tr_torrentId(tor), i);
    if (o != nullptr)
    {
        /* flush writable files so that their mtimes will be
         * up-to-date when this function returns to the caller... */
        if (o->is_writable)
        {
            tr_sys_file_flush(o->fd, nullptr);
        }

        cached_file_close(o);
    }
}

tr_sys_file_t tr_fdFileGetCached(tr_session* s, int torrent_id, tr_file_index_t i, bool writable)
{
    struct tr_cached_file* o = fileset_lookup(get_fileset(s), torrent_id, i);

    if (o == nullptr || (writable && !o->is_writable))
    {
        return TR_BAD_SYS_FILE;
    }

    o->used_at = tr_time();
    return o->fd;
}

void tr_fdTorrentClose(tr_session* session, int torrent_id)
{
    auto const lock = session->unique_lock();

    fileset_close_torrent(get_fileset(session), torrent_id);
}

/* returns an fd on success, or a TR_BAD_SYS_FILE on failure and sets errno */
tr_sys_file_t tr_fdFileCheckout(
    tr_session* session,
    int torrent_id,
    tr_file_index_t i,
    char const* filename,
    bool writable,
    tr_preallocation_mode allocation,
    uint64_t file_size)
{
    struct tr_fileset* set = get_fileset(session);
    struct tr_cached_file* o = fileset_lookup(set, torrent_id, i);

    if (o != nullptr && writable && !o->is_writable)
    {
        cached_file_close(o); /* close it so we can reopen in rw mode */
    }
    else if (o == nullptr)
    {
        o = fileset_get_empty_slot(set);
    }

    if (!cached_file_is_open(o))
    {
        if (int const err = cached_file_open(o, filename, writable, allocation, file_size); err != 0)
        {
            errno = err;
            return TR_BAD_SYS_FILE;
        }

        tr_logAddTrace(fmt::format("opened '{}' writable {}", filename, writable ? 'y' : 'n'));
        o->is_writable = writable;
    }

    tr_logAddTrace(fmt::format("checking out '{}'", filename));
    o->torrent_id = torrent_id;
    o->file_index = i;
    o->used_at = tr_time();
    return o->fd;
}

/***
****
****  Sockets
****
***/

tr_socket_t tr_fdSocketCreate(tr_session* session, int domain, int type)
{
    TR_ASSERT(tr_isSession(session));

    tr_socket_t s = TR_BAD_SOCKET;

    ensureSessionFdInfoExists(session);
    tr_fdInfo* gFd = session->fdInfo;

    if (gFd->peerCount < session->peerLimit)
    {
        s = socket(domain, type, 0);

        if ((s == TR_BAD_SOCKET) && (sockerrno != EAFNOSUPPORT))
        {
            tr_logAddWarn(fmt::format(
                _("Couldn't create socket: {error} ({error_code})"),
                fmt::arg("error", tr_net_strerror(sockerrno)),
                fmt::arg("error_code", sockerrno)));
        }
    }

    if (s != TR_BAD_SOCKET)
    {
        ++gFd->peerCount;
    }

    TR_ASSERT(gFd->peerCount >= 0);

    if (s != TR_BAD_SOCKET)
    {
        static bool buf_logged = false;

        if (!buf_logged)
        {
            int i = 0;
            socklen_t size = sizeof(i);

            if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&i), &size) != -1)
            {
                tr_logAddTrace(fmt::format("SO_SNDBUF size is {}", i));
            }

            i = 0;
            size = sizeof(i);

            if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&i), &size) != -1)
            {
                tr_logAddTrace(fmt::format("SO_RCVBUF size is {}", i));
            }

            buf_logged = true;
        }
    }

    return s;
}

tr_socket_t tr_fdSocketAccept(tr_session* s, tr_socket_t sockfd, tr_address* addr, tr_port* port)
{
    TR_ASSERT(tr_isSession(s));
    TR_ASSERT(addr != nullptr);
    TR_ASSERT(port != nullptr);

    ensureSessionFdInfoExists(s);
    tr_fdInfo* const gFd = s->fdInfo;

    struct sockaddr_storage sock;
    socklen_t len = sizeof(struct sockaddr_storage);
    tr_socket_t fd = accept(sockfd, (struct sockaddr*)&sock, &len);

    if (fd != TR_BAD_SOCKET)
    {
        if (gFd->peerCount < s->peerLimit && tr_address_from_sockaddr_storage(addr, port, &sock))
        {
            ++gFd->peerCount;
        }
        else
        {
            tr_netCloseSocket(fd);
            fd = TR_BAD_SOCKET;
        }
    }

    return fd;
}

void tr_fdSocketClose(tr_session* session, tr_socket_t fd)
{
    TR_ASSERT(tr_isSession(session));

    if (session->fdInfo != nullptr)
    {
        struct tr_fdInfo* gFd = session->fdInfo;

        if (fd != TR_BAD_SOCKET)
        {
            tr_netCloseSocket(fd);
            --gFd->peerCount;
        }

        TR_ASSERT(gFd->peerCount >= 0);
    }
}
