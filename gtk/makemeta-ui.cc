/*
 * This file Copyright (C) 2007-2021 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <glibmm.h>
#include <glibmm/i18n.h>

#include <libtransmission/transmission.h>
#include <libtransmission/makemeta.h>
#include <libtransmission/utils.h> /* tr_formatter_mem_B() */

#include "hig.h"
#include "makemeta-ui.h"
#include "tr-core.h"
#include "tr-prefs.h"
#include "util.h"

#define FILE_CHOSEN_KEY "file-is-chosen"

namespace
{

class MakeProgressDialog : public Gtk::Dialog
{
public:
    MakeProgressDialog(Gtk::Window& parent, tr_metainfo_builder& builder, std::string const& target, TrCore* core);
    ~MakeProgressDialog();

private:
    bool onProgressDialogRefresh();
    void onProgressDialogResponse(int response);

    void addTorrent();

private:
    tr_metainfo_builder& builder_;
    std::string const target_;
    TrCore* const core_;

    sigc::connection progress_tag_;
    Gtk::Label* progress_label_ = nullptr;
    Gtk::ProgressBar* progress_bar_ = nullptr;
};

} // namespace

class MakeDialog::Impl
{
public:
    Impl(MakeDialog& dialog, TrCore* core);

private:
    void onSourceToggled2(Gtk::ToggleButton* tb, Gtk::FileChooserButton* chooser);
    void onChooserChosen(Gtk::FileChooserButton* chooser);
    void onResponse(int response);

    void on_drag_data_received(
        Glib::RefPtr<Gdk::DragContext> const& drag_context,
        int x,
        int y,
        Gtk::SelectionData const& selection_data,
        guint info,
        guint time_);

    void updatePiecesLabel();

    void setFilename(std::string const& filename);

    void makeProgressDialog(std::string const& target);

private:
    MakeDialog& dialog_;
    TrCore* const core_;

    Gtk::RadioButton* file_radio_ = nullptr;
    Gtk::FileChooserButton* file_chooser_ = nullptr;
    Gtk::RadioButton* folder_radio_ = nullptr;
    Gtk::FileChooserButton* folder_chooser_ = nullptr;
    Gtk::Label* pieces_lb_ = nullptr;
    Gtk::FileChooserButton* destination_chooser_ = nullptr;
    Gtk::CheckButton* comment_check_ = nullptr;
    Gtk::Entry* comment_entry_ = nullptr;
    Gtk::CheckButton* private_check_ = nullptr;
    std::unique_ptr<MakeProgressDialog> progress_dialog_;
    Glib::RefPtr<Gtk::TextBuffer> announce_text_buffer_;
    std::unique_ptr<tr_metainfo_builder, void (*)(tr_metainfo_builder*)> builder_ = { nullptr, nullptr };
};

bool MakeProgressDialog::onProgressDialogRefresh()
{
    Glib::ustring str;
    double const fraction = builder_.pieceCount != 0 ? (double)builder_.pieceIndex / builder_.pieceCount : 0;
    auto const base = Glib::path_get_basename(builder_.top);

    /* progress label */
    if (!builder_.isDone)
    {
        str = Glib::ustring::sprintf(_("Creating \"%s\""), base);
    }
    else if (builder_.result == TR_MAKEMETA_OK)
    {
        str = Glib::ustring::sprintf(_("Created \"%s\"!"), base);
    }
    else if (builder_.result == TR_MAKEMETA_URL)
    {
        str = Glib::ustring::sprintf(_("Error: invalid announce URL \"%s\""), builder_.errfile);
    }
    else if (builder_.result == TR_MAKEMETA_CANCELLED)
    {
        str = _("Cancelled");
    }
    else if (builder_.result == TR_MAKEMETA_IO_READ)
    {
        str = Glib::ustring::sprintf(_("Error reading \"%s\": %s"), builder_.errfile, Glib::strerror(builder_.my_errno));
    }
    else if (builder_.result == TR_MAKEMETA_IO_WRITE)
    {
        str = Glib::ustring::sprintf(_("Error writing \"%s\": %s"), builder_.errfile, Glib::strerror(builder_.my_errno));
    }
    else
    {
        g_assert_not_reached();
    }

    gtr_label_set_text(Glib::unwrap(progress_label_), str.c_str());

    /* progress bar */
    if (builder_.pieceIndex == 0)
    {
        str.clear();
    }
    else
    {
        char sizebuf[128];
        tr_strlsize(sizebuf, (uint64_t)builder_.pieceIndex * (uint64_t)builder_.pieceSize, sizeof(sizebuf));
        /* how much data we've scanned through to generate checksums */
        str = Glib::ustring::sprintf(_("Scanned %s"), sizebuf);
    }

    progress_bar_->set_fraction(fraction);
    progress_bar_->set_text(str);

    /* buttons */
    set_response_sensitive(Gtk::RESPONSE_CANCEL, !builder_.isDone);
    set_response_sensitive(Gtk::RESPONSE_CLOSE, builder_.isDone);
    set_response_sensitive(Gtk::RESPONSE_ACCEPT, builder_.isDone && !builder_.result);

    return true;
}

MakeProgressDialog::~MakeProgressDialog()
{
    progress_tag_.disconnect();
}

void MakeProgressDialog::addTorrent()
{
    tr_ctor* ctor = tr_ctorNew(gtr_core_session(core_));
    tr_ctorSetMetainfoFromFile(ctor, target_.c_str());
    tr_ctorSetDownloadDir(ctor, TR_FORCE, Glib::path_get_dirname(builder_.top).c_str());
    gtr_core_add_ctor(core_, ctor);
}

void MakeProgressDialog::onProgressDialogResponse(int response)
{
    switch (response)
    {
    case Gtk::RESPONSE_CANCEL:
        builder_.abortFlag = true;
        hide();
        break;

    case Gtk::RESPONSE_ACCEPT:
        addTorrent();
        [[fallthrough]];

    case Gtk::RESPONSE_CLOSE:
        hide();
        break;

    default:
        g_assert(0 && "unhandled response");
    }
}

MakeProgressDialog::MakeProgressDialog(
    Gtk::Window& parent,
    tr_metainfo_builder& builder,
    std::string const& target,
    TrCore* core)
    : Gtk::Dialog(_("New Torrent"), parent, true)
    , builder_(builder)
    , target_(target)
    , core_(core)
{
    add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    add_button(_("_Close"), Gtk::RESPONSE_CLOSE);
    add_button(_("_Add"), Gtk::RESPONSE_ACCEPT);
    signal_response().connect(sigc::mem_fun(this, &MakeProgressDialog::onProgressDialogResponse));

    auto* fr = Gtk::make_managed<Gtk::Frame>();
    fr->set_border_width(GUI_PAD_BIG);
    fr->set_shadow_type(Gtk::SHADOW_NONE);
    auto* v = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, GUI_PAD);
    fr->add(*v);

    progress_label_ = Gtk::make_managed<Gtk::Label>(_("Creating torrent…"));
    progress_label_->set_halign(Gtk::ALIGN_START);
    progress_label_->set_valign(Gtk::ALIGN_CENTER);
    progress_label_->set_justify(Gtk::JUSTIFY_LEFT);
    v->pack_start(*progress_label_, false, false, 0);

    progress_bar_ = Gtk::make_managed<Gtk::ProgressBar>();
    v->pack_start(*progress_bar_, false, false, 0);

    progress_tag_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(this, &MakeProgressDialog::onProgressDialogRefresh),
        SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS);
    onProgressDialogRefresh();

    gtr_dialog_set_content(Glib::unwrap(this), Glib::unwrap(static_cast<Gtk::Widget*>(fr)));
}

void MakeDialog::Impl::makeProgressDialog(std::string const& target)
{
    progress_dialog_ = std::make_unique<MakeProgressDialog>(dialog_, *builder_, target, core_);
    progress_dialog_->signal_hide().connect(
        [this]()
        {
            progress_dialog_.reset();

            if (!builder_->result)
            {
                dialog_.hide();
            }
        });
    progress_dialog_->show();
}

void MakeDialog::Impl::onResponse(int response)
{
    if (response == Gtk::RESPONSE_ACCEPT)
    {
        if (builder_ != nullptr)
        {
            auto const comment = comment_entry_->get_text();
            bool const isPrivate = private_check_->get_active();
            bool const useComment = comment_check_->get_active();

            /* destination file */
            auto const dir = destination_chooser_->get_filename();
            auto const base = Glib::path_get_basename(builder_->top);
            auto const target = Glib::ustring::sprintf("%s/%s.torrent", dir, base).raw();

            /* build the array of trackers */
            auto const tracker_text = announce_text_buffer_->get_text(false);
            std::istringstream tracker_strings(tracker_text);

            std::vector<tr_tracker_info> trackers;
            std::list<std::string> announce_urls;
            int tier = 0;

            std::string str;
            while (std::getline(tracker_strings, str))
            {
                if (str.empty())
                {
                    ++tier;
                }
                else
                {
                    announce_urls.push_front(str);
                    trackers.push_back(tr_tracker_info{ tier, announce_urls.front().data(), nullptr, 0 });
                }
            }

            /* build the .torrent */
            makeProgressDialog(target);
            tr_makeMetaInfo(
                builder_.get(),
                target.c_str(),
                trackers.data(),
                trackers.size(),
                useComment ? comment.c_str() : nullptr,
                isPrivate);
        }
    }
    else if (response == Gtk::RESPONSE_CLOSE)
    {
        dialog_.hide();
    }
}

/***
****
***/

namespace
{

void onSourceToggled(Gtk::ToggleButton* tb, Gtk::Widget* widget)
{
    widget->set_sensitive(tb->get_active());
}

} // namespace

void MakeDialog::Impl::updatePiecesLabel()
{
    char const* filename = builder_ != nullptr ? builder_->top : nullptr;
    Glib::ustring gstr;

    gstr += "<i>";

    if (filename == nullptr)
    {
        gstr += _("No source selected");
    }
    else
    {
        char buf[128];
        tr_strlsize(buf, builder_->totalSize, sizeof(buf));
        gstr += Glib::ustring::sprintf(
            ngettext("%1$s; %2$'d File", "%1$s; %2$'d Files", builder_->fileCount),
            buf,
            builder_->fileCount);
        gstr += "; ";

        tr_formatter_mem_B(buf, builder_->pieceSize, sizeof(buf));
        gstr += Glib::ustring::sprintf(
            ngettext("%1$'d Piece @ %2$s", "%1$'d Pieces @ %2$s", builder_->pieceCount),
            builder_->pieceCount,
            buf);
    }

    gstr += "</i>";
    pieces_lb_->set_markup(gstr);
}

void MakeDialog::Impl::setFilename(std::string const& filename)
{
    builder_.reset();

    if (!filename.empty())
    {
        builder_ = { tr_metaInfoBuilderCreate(filename.c_str()), &tr_metaInfoBuilderFree };
    }

    updatePiecesLabel();
}

void MakeDialog::Impl::onChooserChosen(Gtk::FileChooserButton* chooser)
{
    chooser->set_data(FILE_CHOSEN_KEY, GINT_TO_POINTER(true));
    setFilename(chooser->get_filename());
}

void MakeDialog::Impl::onSourceToggled2(Gtk::ToggleButton* tb, Gtk::FileChooserButton* chooser)
{
    if (tb->get_active())
    {
        if (chooser->get_data(FILE_CHOSEN_KEY) != nullptr)
        {
            onChooserChosen(chooser);
        }
        else
        {
            setFilename({});
        }
    }
}

void MakeDialog::Impl::on_drag_data_received(
    Glib::RefPtr<Gdk::DragContext> const& drag_context,
    int /*x*/,
    int /*y*/,
    Gtk::SelectionData const& selection_data,
    guint /*info*/,
    guint time_)
{
    bool success = false;
    auto const uris = selection_data.get_uris();

    if (!uris.empty())
    {
        auto const uri = uris.front();
        auto const filename = Glib::filename_from_uri(uri);

        if (Glib::file_test(filename, Glib::FILE_TEST_IS_DIR))
        {
            /* a directory was dragged onto the dialog... */
            folder_radio_->set_active(true);
            folder_chooser_->set_current_folder(filename);
            success = true;
        }
        else if (Glib::file_test(filename, Glib::FILE_TEST_IS_REGULAR))
        {
            /* a file was dragged on to the dialog... */
            file_radio_->set_active(true);
            file_chooser_->set_filename(filename);
            success = true;
        }
    }

    drag_context->drag_finish(success, false, time_);
}

std::unique_ptr<MakeDialog> MakeDialog::create(Gtk::Window& parent, TrCore* core)
{
    return std::unique_ptr<MakeDialog>(new MakeDialog(parent, core));
}

MakeDialog::MakeDialog(Gtk::Window& parent, TrCore* core)
    : Gtk::Dialog(_("New Torrent"), parent)
    , impl_(std::make_unique<Impl>(*this, core))
{
}

MakeDialog::~MakeDialog() = default;

MakeDialog::Impl::Impl(MakeDialog& dialog, TrCore* core)
    : dialog_(dialog)
    , core_(core)
{
    guint row = 0;

    dialog_.add_button(_("_Close"), Gtk::RESPONSE_CLOSE);
    dialog_.add_button(_("_New"), Gtk::RESPONSE_ACCEPT);
    dialog_.signal_response().connect(sigc::mem_fun(this, &Impl::onResponse));

    auto* t = Gtk::make_managed<HigWorkarea>();

    t->add_section_title(row, _("Files"));

    destination_chooser_ = Gtk::make_managed<Gtk::FileChooserButton>(Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    destination_chooser_->set_current_folder(Glib::get_user_special_dir(Glib::USER_DIRECTORY_DESKTOP));
    t->add_row(row, _("Sa_ve to:"), *destination_chooser_);

    Gtk::RadioButton::Group slist;

    folder_radio_ = Gtk::make_managed<Gtk::RadioButton>(slist, _("Source F_older:"), true);
    folder_radio_->set_active(false);
    folder_chooser_ = Gtk::make_managed<Gtk::FileChooserButton>(Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    folder_radio_->signal_toggled().connect([this]() { onSourceToggled2(folder_radio_, folder_chooser_); });
    folder_radio_->signal_toggled().connect([this]() { onSourceToggled(folder_radio_, folder_chooser_); });
    folder_chooser_->signal_selection_changed().connect([this]() { onChooserChosen(folder_chooser_); });
    folder_chooser_->set_sensitive(false);
    t->add_row_w(row, *folder_radio_, *folder_chooser_);

    file_radio_ = Gtk::make_managed<Gtk::RadioButton>(slist, _("Source _File:"), true);
    file_radio_->set_active(true);
    file_chooser_ = Gtk::make_managed<Gtk::FileChooserButton>(Gtk::FILE_CHOOSER_ACTION_OPEN);
    file_radio_->signal_toggled().connect([this]() { onSourceToggled2(file_radio_, file_chooser_); });
    file_radio_->signal_toggled().connect([this]() { onSourceToggled(file_radio_, file_chooser_); });
    file_chooser_->signal_selection_changed().connect([this]() { onChooserChosen(file_chooser_); });
    t->add_row_w(row, *file_radio_, *file_chooser_);

    pieces_lb_ = Gtk::make_managed<Gtk::Label>();
    pieces_lb_->set_markup(_("<i>No source selected</i>"));
    t->add_row(row, {}, *pieces_lb_);

    t->add_section_divider(row);
    t->add_section_title(row, _("Properties"));

    auto* v = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, GUI_PAD_SMALL);
    announce_text_buffer_ = Gtk::TextBuffer::create();
    auto* w = Gtk::make_managed<Gtk::TextView>(announce_text_buffer_);
    w->set_size_request(-1, 80);
    auto* sw = Gtk::make_managed<Gtk::ScrolledWindow>();
    sw->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    sw->add(*w);
    auto* fr = Gtk::make_managed<Gtk::Frame>();
    fr->set_shadow_type(Gtk::SHADOW_IN);
    fr->add(*sw);
    v->pack_start(*fr, true, true, 0);
    auto* l = Gtk::make_managed<Gtk::Label>();
    l->set_markup(
        _("To add a backup URL, add it on the line after the primary URL.\n"
          "To add another primary URL, add it after a blank line."));
    l->set_justify(Gtk::JUSTIFY_LEFT);
    l->set_halign(Gtk::ALIGN_START);
    l->set_valign(Gtk::ALIGN_CENTER);
    v->pack_start(*l, false, false, 0);
    t->add_tall_row(row, _("_Trackers:"), *v);

    comment_check_ = Gtk::make_managed<Gtk::CheckButton>(_("Co_mment:"), true);
    comment_check_->set_active(false);
    comment_entry_ = Gtk::make_managed<Gtk::Entry>();
    comment_entry_->set_sensitive(false);
    comment_check_->signal_toggled().connect([this]() { onSourceToggled(comment_check_, comment_entry_); });
    t->add_row_w(row, *comment_check_, *comment_entry_);

    private_check_ = t->add_wide_checkbutton(row, _("_Private torrent"), false);

    gtr_dialog_set_content(Glib::unwrap(&dialog_), Glib::unwrap(static_cast<Gtk::Widget*>(t)));

    dialog_.drag_dest_set(Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_COPY);
    dialog_.drag_dest_add_uri_targets();
    dialog_.signal_drag_data_received().connect(sigc::mem_fun(this, &Impl::on_drag_data_received));
}
