// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <memory>
#include <vector>

#include <glibmm.h>
#include <gtkmm.h>

#include <libtransmission/tr-macros.h>

class Session;

class DetailsDialog : public Gtk::Dialog
{
public:
    ~DetailsDialog() override;

    TR_DISABLE_COPY_MOVE(DetailsDialog)

    static std::unique_ptr<DetailsDialog> create(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

    void set_torrents(std::vector<int> const& torrent_ids);

protected:
    DetailsDialog(Gtk::Window& parent, Glib::RefPtr<Session> const& core);

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
};
