/*
 * This file Copyright (C) 2012-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cinttypes> // intX_t
#include <cstddef> // size_t
#include <ctime>

#include "transmission.h"

struct tr_torrent;

// defined by BEP #9
inline constexpr int METADATA_PIECE_SIZE = 1024 * 16;

void* tr_torrentGetMetadataPiece(tr_torrent const* tor, int piece, size_t* len);

void tr_torrentSetMetadataPiece(tr_torrent* tor, int piece, void const* data, int len);

bool tr_torrentGetNextMetadataRequest(tr_torrent* tor, time_t now, int* setme);

bool tr_torrentSetMetadataSizeHint(tr_torrent* tor, int64_t metadata_size);

double tr_torrentGetMetadataPercent(tr_torrent const* tor);
