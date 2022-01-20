// This file Copyright (C) 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <memory>

#include <gtkmm.h>

#include <libtransmission/tr-macros.h>

class Session;

class PrefsDialog : public Gtk::Dialog
{
public:
    ~PrefsDialog() override;

    TR_DISABLE_COPY_MOVE(PrefsDialog)

    static std::unique_ptr<PrefsDialog> create(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

protected:
    PrefsDialog(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
};

enum
{
    MAIN_WINDOW_REFRESH_INTERVAL_SECONDS = 2,
    SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS = 2
};
