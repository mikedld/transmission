/*
 * This file Copyright (C) 2010-2021 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <memory>

#include <gtkmm.h>

typedef struct _TrCore TrCore;
typedef struct tr_ctor tr_ctor;

class TorrentUrlChooserDialog : public Gtk::Dialog
{
public:
    static std::unique_ptr<TorrentUrlChooserDialog> create(Gtk::Window& parent, TrCore* core);

protected:
    TorrentUrlChooserDialog(Gtk::Window& parent, TrCore* core);

private:
    void onOpenURLResponse(int response, TrCore* core);
};

class TorrentFileChooserDialog : public Gtk::FileChooserDialog
{
public:
    static std::unique_ptr<TorrentFileChooserDialog> create(Gtk::Window& parent, TrCore* core);

protected:
    TorrentFileChooserDialog(Gtk::Window& parent, TrCore* core);

private:
    void onOpenDialogResponse(int response, TrCore* core);
};

class OptionsDialog : public Gtk::Dialog
{
public:
    ~OptionsDialog() override;

    static std::unique_ptr<OptionsDialog> create(
        Gtk::Window& parent,
        TrCore* core,
        std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor);

protected:
    OptionsDialog(Gtk::Window& parent, TrCore* core, std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor);

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
};
