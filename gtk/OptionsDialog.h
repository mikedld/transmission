// This file Copyright © 2010-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <memory>

#include <gtkmm.h>

#include <libtransmission/tr-macros.h>

class Session;
typedef struct tr_ctor tr_ctor;

class TorrentUrlChooserDialog : public Gtk::Dialog
{
public:
    TR_DISABLE_COPY_MOVE(TorrentUrlChooserDialog)

    static std::unique_ptr<TorrentUrlChooserDialog> create(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

protected:
    TorrentUrlChooserDialog(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

private:
    void onOpenURLResponse(int response, Glib::RefPtr<Session> const& core);
};

class TorrentFileChooserDialog : public Gtk::FileChooserDialog
{
public:
    TR_DISABLE_COPY_MOVE(TorrentFileChooserDialog)

    static std::unique_ptr<TorrentFileChooserDialog> create(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

protected:
    TorrentFileChooserDialog(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

private:
    void onOpenDialogResponse(int response, Glib::RefPtr<Session> const& core);
};

class OptionsDialog : public Gtk::Dialog
{
public:
    ~OptionsDialog() override;

    TR_DISABLE_COPY_MOVE(OptionsDialog)

    static std::unique_ptr<OptionsDialog> create(
        Gtk::Window& parent,
        Glib::RefPtr<Session> const& core,
        std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor);

protected:
    OptionsDialog(Gtk::Window& parent, Glib::RefPtr<Session> const& core, std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor);

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
};
