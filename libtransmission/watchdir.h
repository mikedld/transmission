/*
 * This file Copyright (C) 2015-2016 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <string_view>

struct event_base;

using tr_watchdir_t = struct tr_watchdir*;

enum tr_watchdir_status
{
    TR_WATCHDIR_ACCEPT,
    TR_WATCHDIR_IGNORE,
    TR_WATCHDIR_RETRY
};

using tr_watchdir_cb = tr_watchdir_status (*)(tr_watchdir_t handle, char const* name, void* user_data);

/* ... */

tr_watchdir_t tr_watchdir_new(
    std::string_view path,
    tr_watchdir_cb callback,
    void* callback_user_data,
    struct event_base* event_base,
    bool force_generic);

void tr_watchdir_free(tr_watchdir_t handle);

char const* tr_watchdir_get_path(tr_watchdir_t handle);
