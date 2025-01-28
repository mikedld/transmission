// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstdint> // uint64_t

#include "libtransmission/transmission.h"

#include "libtransmission/torrent.h"

namespace tr_resume
{

using fields_t = uint64_t;

auto inline constexpr Downloaded = fields_t{ 1 << 0 };
auto inline constexpr Uploaded = fields_t{ 1 << 1 };
auto inline constexpr Corrupt = fields_t{ 1 << 2 };
auto inline constexpr Peers = fields_t{ 1 << 3 };
auto inline constexpr Progress = fields_t{ 1 << 4 };
auto inline constexpr Dnd = fields_t{ 1 << 5 };
auto inline constexpr FilePriorities = fields_t{ 1 << 6 };
auto inline constexpr BandwidthPriority = fields_t{ 1 << 7 };
auto inline constexpr Speedlimit = fields_t{ 1 << 8 };
auto inline constexpr Run = fields_t{ 1 << 9 };
auto inline constexpr DownloadDir = fields_t{ 1 << 10 };
auto inline constexpr IncompleteDir = fields_t{ 1 << 11 };
auto inline constexpr MaxPeers = fields_t{ 1 << 12 };
auto inline constexpr AddedDate = fields_t{ 1 << 13 };
auto inline constexpr DoneDate = fields_t{ 1 << 14 };
auto inline constexpr ActivityDate = fields_t{ 1 << 15 };
auto inline constexpr Ratiolimit = fields_t{ 1 << 16 };
auto inline constexpr Idlelimit = fields_t{ 1 << 17 };
auto inline constexpr TimeSeeding = fields_t{ 1 << 18 };
auto inline constexpr TimeDownloading = fields_t{ 1 << 19 };
auto inline constexpr Filenames = fields_t{ 1 << 20 };
auto inline constexpr Name = fields_t{ 1 << 21 };
auto inline constexpr Labels = fields_t{ 1 << 22 };
auto inline constexpr Group = fields_t{ 1 << 23 };
auto inline constexpr SequentialDownload = fields_t{ 1 << 24 };
auto inline constexpr QueueState = fields_t{ 1 << 25 };

auto inline constexpr All = ~fields_t{ 0 };

auto inline constexpr FileConflicts = fields_t{ 1 << 24 };

fields_t load(tr_torrent* tor, tr_torrent::ResumeHelper& helper, fields_t fields_to_load, tr_ctor const& ctor);

void save(tr_torrent* tor, tr_torrent::ResumeHelper const& helper);

} // namespace tr_resume
