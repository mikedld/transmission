// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include "Dialogs.h"

#include "GtkCompat.h"
#include "Session.h"
#include "Utils.h"

#include <glibmm/i18n.h>
#include <glibmm/ustring.h>
#include <gtkmm/messagedialog.h>

#include <fmt/core.h>

#include <memory>
#include <vector>

/***
****
***/

void gtr_confirm_remove(
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core,
    std::vector<tr_torrent_id_t> const& torrent_ids,
    bool delete_files)
{
    int connected = 0;
    int incomplete = 0;
    int const count = torrent_ids.size();

    if (count == 0)
    {
        return;
    }

    for (auto const id : torrent_ids)
    {
        tr_torrent* tor = core->find_torrent(id);
        tr_stat const* stat = tr_torrentStat(tor);

        if (stat->leftUntilDone != 0)
        {
            ++incomplete;
        }

        if (stat->peersConnected != 0)
        {
            ++connected;
        }
    }

    auto const primary_text = fmt::format(
        fmt::runtime(
            !delete_files ? ngettext("Remove torrent?", "Remove {count:L} torrents?", count) :
                            ngettext(
                                "Delete this torrent's downloaded files?",
                                "Delete these {count:L} torrents' downloaded files?",
                                count)),
        fmt::arg("count", count));

    Glib::ustring secondary_text;
    if (incomplete == 0 && connected == 0)
    {
        secondary_text = ngettext(
            "Once removed, continuing the transfer will require the torrent file or magnet link.",
            "Once removed, continuing the transfers will require the torrent files or magnet links.",
            count);
    }
    else if (count == incomplete)
    {
        secondary_text = ngettext(
            "This torrent has not finished downloading.",
            "These torrents have not finished downloading.",
            count);
    }
    else if (count == connected)
    {
        secondary_text = ngettext("This torrent is connected to peers.", "These torrents are connected to peers.", count);
    }
    else
    {
        if (connected != 0)
        {
            secondary_text += ngettext(
                "One of these torrents is connected to peers.",
                "Some of these torrents are connected to peers.",
                connected);
        }

        if (connected != 0 && incomplete != 0)
        {
            secondary_text += "\n";
        }

        if (incomplete != 0)
        {
            secondary_text += ngettext(
                "One of these torrents has not finished downloading.",
                "Some of these torrents have not finished downloading.",
                incomplete);
        }
    }

    auto d = std::make_shared<Gtk::MessageDialog>(
        parent,
        fmt::format("<big><b>{}</b></big>", primary_text),
        true /*use_markup*/,
        TR_GTK_MESSAGE_TYPE(WARNING),
        TR_GTK_BUTTONS_TYPE(NONE),
        true /*modal*/);

    if (!secondary_text.empty())
    {
        d->set_secondary_text(secondary_text, true);
    }

    d->add_button(_("_Cancel"), TR_GTK_RESPONSE_TYPE(CANCEL));
    d->add_button(delete_files ? _("_Delete") : _("_Remove"), TR_GTK_RESPONSE_TYPE(ACCEPT));
    d->set_default_response(TR_GTK_RESPONSE_TYPE(CANCEL));

    d->signal_response().connect(
        [d, core, torrent_ids, delete_files](int response) mutable
        {
            if (response == TR_GTK_RESPONSE_TYPE(ACCEPT))
            {
                for (auto const id : torrent_ids)
                {
                    core->remove_torrent(id, delete_files);
                }
            }

            d.reset();
        });

    d->show();
}
