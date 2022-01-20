// This file Copyright © 2012-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <memory>

#include <gtkmm.h>

#include <libtransmission/tr-macros.h>

typedef struct tr_session tr_session;

class FilterBar : public Gtk::Box
{
public:
    FilterBar(tr_session* session, Glib::RefPtr<Gtk::TreeModel> const& torrent_model);
    ~FilterBar() override;

    TR_DISABLE_COPY_MOVE(FilterBar)

    Glib::RefPtr<Gtk::TreeModel> get_filter_model() const;

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
};
