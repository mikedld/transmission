// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

struct tr_address;
struct tr_blocklistFile;

tr_blocklistFile* tr_blocklistFileNew(char const* filename, bool isEnabled);

char const* tr_blocklistFileGetFilename(tr_blocklistFile const* b);

int tr_blocklistFileGetRuleCount(tr_blocklistFile const* b);

void tr_blocklistFileFree(tr_blocklistFile* b);

void tr_blocklistFileSetEnabled(tr_blocklistFile* b, bool isEnabled);

bool tr_blocklistFileHasAddress(tr_blocklistFile* b, struct tr_address const* addr);

int tr_blocklistFileSetContent(tr_blocklistFile* b, char const* filename);
