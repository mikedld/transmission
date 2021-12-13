/*
 * This file Copyright (C) 2007-2021 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <memory>

#include <gtkmm.h>

#include <libtransmission/tr-macros.h>

class Session;

class MakeDialog : public Gtk::Dialog
{
public:
    ~MakeDialog() override;

    TR_DISABLE_COPY_MOVE(MakeDialog)

    static std::unique_ptr<MakeDialog> create(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

protected:
    MakeDialog(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
};
