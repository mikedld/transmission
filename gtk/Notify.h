// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <glibmm.h>

class Session;

void gtr_notify_init();

void gtr_notify_torrent_added(Glib::RefPtr<Session> const& core, int torrent_id);

void gtr_notify_torrent_completed(Glib::RefPtr<Session> const& core, int torrent_id);
