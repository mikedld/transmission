// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstdint>

struct tr_ctor;
struct tr_torrent;

enum
{
    TR_FR_DOWNLOADED = (1 << 0),
    TR_FR_UPLOADED = (1 << 1),
    TR_FR_CORRUPT = (1 << 2),
    TR_FR_PEERS = (1 << 3),
    TR_FR_PROGRESS = (1 << 4),
    TR_FR_DND = (1 << 5),
    TR_FR_FILE_PRIORITIES = (1 << 6),
    TR_FR_BANDWIDTH_PRIORITY = (1 << 7),
    TR_FR_SPEEDLIMIT = (1 << 8),
    TR_FR_RUN = (1 << 9),
    TR_FR_DOWNLOAD_DIR = (1 << 10),
    TR_FR_INCOMPLETE_DIR = (1 << 11),
    TR_FR_MAX_PEERS = (1 << 12),
    TR_FR_ADDED_DATE = (1 << 13),
    TR_FR_DONE_DATE = (1 << 14),
    TR_FR_ACTIVITY_DATE = (1 << 15),
    TR_FR_RATIOLIMIT = (1 << 16),
    TR_FR_IDLELIMIT = (1 << 17),
    TR_FR_TIME_SEEDING = (1 << 18),
    TR_FR_TIME_DOWNLOADING = (1 << 19),
    TR_FR_FILENAMES = (1 << 20),
    TR_FR_NAME = (1 << 21),
    TR_FR_LABELS = (1 << 22)
};

/**
 * Returns a bitwise-or'ed set of the loaded resume data
 */
uint64_t tr_torrentLoadResume(tr_torrent* tor, uint64_t fieldsToLoad, tr_ctor const* ctor, bool* didRenameToHashOnlyName);

void tr_torrentSaveResume(tr_torrent* tor);
