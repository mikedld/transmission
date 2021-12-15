/*
 * This file Copyright (C) 2017 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <map>
#include <string_view>

struct tr_error;

bool tr_spawn_async(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    char const* work_dir,
    tr_error** error);
