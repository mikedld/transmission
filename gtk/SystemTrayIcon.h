// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <memory>

#include <gtkmm.h>

#include <libtransmission/tr-macros.h>

class Session;

class SystemTrayIcon
{
public:
    SystemTrayIcon(Gtk::Window& main_window, Glib::RefPtr<Session> const& core);
    ~SystemTrayIcon();

    TR_DISABLE_COPY_MOVE(SystemTrayIcon)

    void refresh();

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
};
