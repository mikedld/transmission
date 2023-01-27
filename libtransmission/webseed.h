// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <string_view>

#include "peer-common.h"

tr_peer* tr_webseedNew(struct tr_torrent* torrent, std::string_view, tr_peer_callback callback);

tr_webseed_view tr_webseedView(tr_peer const* peer);
