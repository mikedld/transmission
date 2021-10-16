/*
 * This file Copyright (C) 2009-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <algorithm>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <libtransmission/transmission.h>

#include "conf.h" /* gtr_pref_string_get */
#include "hig.h"
#include "relocate.h"
#include "util.h"

#define DATA_KEY "gtr-relocate-data"

static char* previousLocation = nullptr;

struct relocate_dialog_data
{
    int done;
    bool do_move;
    guint timer;
    TrCore* core;
    GSList* torrent_ids;
    GtkWidget* message_dialog;
    GtkWidget* chooser_dialog;
};

static void data_free(gpointer gdata)
{
    auto* data = static_cast<relocate_dialog_data*>(gdata);
    g_source_remove(data->timer);
    g_slist_free(data->torrent_ids);
    g_free(data);
}

/***
****
***/

static void startMovingNextTorrent(struct relocate_dialog_data* data)
{
    char* str;
    int const id = GPOINTER_TO_INT(data->torrent_ids->data);

    tr_torrent* tor = gtr_core_find_torrent(data->core, id);

    if (tor != nullptr)
    {
        tr_torrentSetLocation(tor, previousLocation, data->do_move, nullptr, &data->done);
    }

    data->torrent_ids = g_slist_delete_link(data->torrent_ids, data->torrent_ids);

    str = g_strdup_printf(_("Moving \"%s\""), tr_torrentName(tor));
    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(data->message_dialog), str);
    g_free(str);
}

/* every once in awhile, check to see if the move is done.
 * if so, delete the dialog */
static gboolean onTimer(gpointer gdata)
{
    auto* data = static_cast<relocate_dialog_data*>(gdata);
    int const done = data->done;

    if (done == TR_LOC_ERROR)
    {
        auto const flags = GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);
        GtkWidget* w = gtk_message_dialog_new(
            GTK_WINDOW(data->message_dialog),
            flags,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "%s",
            _("Couldn't move torrent"));
        gtk_dialog_run(GTK_DIALOG(w));
        gtk_widget_destroy(GTK_WIDGET(data->message_dialog));
    }
    else if (done == TR_LOC_DONE)
    {
        if (data->torrent_ids != nullptr)
        {
            startMovingNextTorrent(data);
        }
        else
        {
            gtk_widget_destroy(GTK_WIDGET(data->chooser_dialog));
        }
    }

    return G_SOURCE_CONTINUE;
}

static void onResponse(GtkDialog* dialog, int response, gconstpointer user_data)
{
    TR_UNUSED(user_data);

    if (response == GTK_RESPONSE_APPLY)
    {
        GtkWidget* w;
        GObject* d = G_OBJECT(dialog);
        auto* data = static_cast<relocate_dialog_data*>(g_object_get_data(d, DATA_KEY));
        auto* chooser = static_cast<GtkFileChooser*>(g_object_get_data(d, "chooser"));
        auto* move_tb = static_cast<GtkToggleButton*>(g_object_get_data(d, "move_rb"));
        char* location = gtk_file_chooser_get_filename(chooser);

        data->do_move = gtk_toggle_button_get_active(move_tb);

        /* pop up a dialog saying that the work is in progress */
        w = gtk_message_dialog_new(
            GTK_WINDOW(dialog),
            GtkDialogFlags(GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL),
            GTK_MESSAGE_INFO,
            GTK_BUTTONS_CLOSE,
            nullptr);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(w), _("This may take a moment…"));
        gtk_dialog_set_response_sensitive(GTK_DIALOG(w), GTK_RESPONSE_CLOSE, FALSE);
        gtk_widget_show(w);

        /* remember this location so that it can be the default next time */
        g_free(previousLocation);
        previousLocation = location;

        /* start the move and periodically check its status */
        data->message_dialog = w;
        data->done = TR_LOC_DONE;
        data->timer = gdk_threads_add_timeout_seconds(1, onTimer, data);
        onTimer(data);
    }
    else
    {
        gtk_widget_destroy(GTK_WIDGET(dialog));
    }
}

GtkWidget* gtr_relocate_dialog_new(GtkWindow* parent, TrCore* core, std::vector<int> const& torrent_ids)
{
    guint row;
    GtkWidget* w;
    GtkWidget* d;
    GtkWidget* t;
    struct relocate_dialog_data* data;

    d = gtk_dialog_new_with_buttons(
        _("Set Torrent Location"),
        parent,
        GtkDialogFlags(GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL),
        TR_ARG_TUPLE(_("_Cancel"), GTK_RESPONSE_CANCEL),
        TR_ARG_TUPLE(_("_Apply"), GTK_RESPONSE_APPLY),
        nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    g_signal_connect(d, "response", G_CALLBACK(onResponse), nullptr);

    row = 0;
    t = hig_workarea_create();
    hig_workarea_add_section_title(t, &row, _("Location"));

    if (previousLocation == nullptr)
    {
        previousLocation = g_strdup(gtr_pref_string_get(TR_KEY_download_dir));
    }

    w = gtk_file_chooser_button_new(_("Set Torrent Location"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(w), previousLocation);
    g_object_set_data(G_OBJECT(d), "chooser", w);
    hig_workarea_add_row(t, &row, _("Torrent _location:"), w, nullptr);
    w = gtk_radio_button_new_with_mnemonic(nullptr, _("_Move from the current folder"));
    g_object_set_data(G_OBJECT(d), "move_rb", w);
    hig_workarea_add_wide_control(t, &row, w);
    w = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(w), _("Local data is _already there"));
    hig_workarea_add_wide_control(t, &row, w);
    gtr_dialog_set_content(GTK_DIALOG(d), t);

    data = g_new0(struct relocate_dialog_data, 1);
    data->core = core;
    data->torrent_ids = g_slist_alloc();
    data->chooser_dialog = d;

    std::for_each(
        torrent_ids.rbegin(),
        torrent_ids.rend(),
        [data](auto const id) { data->torrent_ids = g_slist_prepend(data->torrent_ids, GINT_TO_POINTER(id)); });

    g_object_set_data_full(G_OBJECT(d), DATA_KEY, data, data_free);

    return d;
}
