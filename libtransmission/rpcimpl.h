/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <cstddef> // size_t
#include <string_view>

#include "transmission.h"

/***
****  RPC processing
***/

struct tr_variant;

using tr_rpc_response_func = void (*)(tr_session* session, tr_variant* response, void* user_data);

/* http://www.json.org/ */
void tr_rpc_request_exec_json(
    tr_session* session,
    tr_variant const* request,
    tr_rpc_response_func callback,
    void* callback_user_data);

/* see the RPC spec's "Request URI Notation" section */
void tr_rpc_request_exec_uri(
    tr_session* session,
    std::string_view request_uri,
    tr_rpc_response_func callback,
    void* callback_user_data);

void tr_rpc_parse_list_str(tr_variant* setme, std::string_view str);
