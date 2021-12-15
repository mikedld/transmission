/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t

#include "tr-macros.h"

/**
 * @brief parse a peer-id into a human-readable client name and version number
 * @ingroup utils
 */
char* tr_clientForId(char* buf, size_t buflen, tr_peer_id_t peer_id);
