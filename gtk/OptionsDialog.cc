// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "OptionsDialog.h"

#include "FileList.h"
#include "FreeSpaceLabel.h"
#include "GtkCompat.h"
#include "PathButton.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

#include <libtransmission/transmission.h>
#include <libtransmission/file.h> /* tr_sys_path_is_same() */

#include <giomm/file.h>
#include <glibmm/i18n.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/combobox.h>
#include <gtkmm/filefilter.h>

#include <memory>
#include <utility>

using namespace std::string_view_literals;

/****
*****
****/

namespace
{

auto const ShowOptionsDialogChoice = "show_options_dialog"sv; // TODO(C++20): Use ""s

std::string get_source_file(tr_ctor& ctor)
{
    if (char const* source_file = tr_ctorGetSourceFile(&ctor); source_file != nullptr)
    {
        return source_file;
    }

    return "";
}

std::string get_download_dir(tr_ctor& ctor)
{
    char const* str = nullptr;
    if (!tr_ctorGetDownloadDir(&ctor, TR_FORCE, &str))
    {
        g_assert_not_reached();
    }

    g_assert(str != nullptr);
    return str;
}

} // namespace

/****
*****
****/

class OptionsDialog::Impl
{
public:
    Impl(
        OptionsDialog& dialog,
        Glib::RefPtr<Gtk::Builder> const& builder,
        Glib::RefPtr<Session> const& core,
        std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor);
    ~Impl() = default;

    TR_DISABLE_COPY_MOVE(Impl)

private:
    void sourceChanged(PathButton* b);
    void downloadDirChanged(PathButton* b);

    void removeOldTorrent();
    void updateTorrent();

    void addResponseCB(int response);

private:
    OptionsDialog& dialog_;
    Glib::RefPtr<Session> const core_;
    std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor_;

    std::string filename_;
    std::string downloadDir_;
    tr_torrent* tor_ = nullptr;

    FileList* file_list_ = nullptr;
    Gtk::CheckButton* run_check_ = nullptr;
    Gtk::CheckButton* trash_check_ = nullptr;
    Gtk::ComboBox* priority_combo_ = nullptr;
    FreeSpaceLabel* freespace_label_ = nullptr;
};

void OptionsDialog::Impl::removeOldTorrent()
{
    if (tor_ != nullptr)
    {
        file_list_->clear();
        tr_torrentRemove(tor_, false, nullptr, nullptr, nullptr, nullptr);
        tor_ = nullptr;
    }
}

void OptionsDialog::Impl::addResponseCB(int response)
{
    if (tor_ != nullptr)
    {
        if (response == TR_GTK_RESPONSE_TYPE(ACCEPT))
        {
            tr_torrentSetPriority(tor_, static_cast<tr_priority_t>(gtr_combo_box_get_active_enum(*priority_combo_)));

            if (run_check_->get_active())
            {
                tr_torrentStart(tor_);
            }

            core_->add_torrent(Torrent::create(tor_), false);

            if (trash_check_->get_active())
            {
                gtr_file_trash_or_remove(filename_, nullptr);
            }

            gtr_save_recent_dir("download", core_, downloadDir_);
        }
        else if (response == TR_GTK_RESPONSE_TYPE(CANCEL))
        {
            removeOldTorrent();
        }
    }

    dialog_.close();
}

void OptionsDialog::Impl::updateTorrent()
{
    bool const isLocalFile = tr_ctorGetSourceFile(ctor_.get()) != nullptr;
    trash_check_->set_sensitive(isLocalFile);

    if (tor_ == nullptr)
    {
        file_list_->clear();
        file_list_->set_sensitive(false);
    }
    else
    {
        tr_torrentSetDownloadDir(tor_, downloadDir_.c_str());
        file_list_->set_sensitive(tr_torrentHasMetadata(tor_));
        file_list_->set_torrent(tr_torrentId(tor_));
        tr_torrentVerify(tor_);
    }
}

/**
 * When the source torrent file is deleted
 * (such as, if it was a temp file that a web browser passed to us),
 * gtk invokes this callback and `filename' will be nullptr.
 * The `filename' tests here are to prevent us from losing the current
 * metadata when that happens.
 */
void OptionsDialog::Impl::sourceChanged(PathButton* b)
{
    auto const filename = b->get_filename();

    /* maybe instantiate a torrent */
    if (!filename.empty() || tor_ == nullptr)
    {
        bool new_file = false;

        if (!filename.empty() && (filename_.empty() || !tr_sys_path_is_same(filename, filename_)))
        {
            filename_ = filename;
            tr_ctorSetMetainfoFromFile(ctor_.get(), filename_.c_str(), nullptr);
            new_file = true;
        }

        tr_ctorSetDownloadDir(ctor_.get(), TR_FORCE, downloadDir_.c_str());
        tr_ctorSetPaused(ctor_.get(), TR_FORCE, true);
        tr_ctorSetDeleteSource(ctor_.get(), false);

        tr_torrent* duplicate_of = nullptr;
        if (tr_torrent* const torrent = tr_torrentNew(ctor_.get(), &duplicate_of); torrent != nullptr)
        {
            removeOldTorrent();
            tor_ = torrent;
        }
        else if (new_file)
        {
            gtr_add_torrent_error_dialog(*b, duplicate_of, filename_);
        }

        updateTorrent();
    }
}

void OptionsDialog::Impl::downloadDirChanged(PathButton* b)
{
    auto const fname = b->get_filename();

    if (!fname.empty() && (downloadDir_.empty() || !tr_sys_path_is_same(fname, downloadDir_)))
    {
        downloadDir_ = fname;
        updateTorrent();

        freespace_label_->set_dir(downloadDir_);
    }
}

namespace
{

template<typename FileChooserT>
void addTorrentFilters(FileChooserT* chooser)
{
    auto filter = Gtk::FileFilter::create();
    filter->set_name(_("Torrent files"));
    filter->add_pattern("*.torrent");
    chooser->add_filter(filter);

    filter = Gtk::FileFilter::create();
    filter->set_name(_("All files"));
    filter->add_pattern("*");
    chooser->add_filter(filter);
}

} // namespace

/****
*****
****/

OptionsDialog::OptionsDialog(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core,
    std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor)
    : Gtk::Dialog(cast_item)
    , impl_(std::make_unique<Impl>(*this, builder, core, std::move(ctor)))
{
    set_transient_for(parent);
}

OptionsDialog::~OptionsDialog() = default;

std::unique_ptr<OptionsDialog> OptionsDialog::create(
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core,
    std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("OptionsDialog.ui"));
    return std::unique_ptr<OptionsDialog>(
        gtr_get_widget_derived<OptionsDialog>(builder, "OptionsDialog", parent, core, std::move(ctor)));
}

OptionsDialog::Impl::Impl(
    OptionsDialog& dialog,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Session> const& core,
    std::unique_ptr<tr_ctor, void (*)(tr_ctor*)> ctor)
    : dialog_(dialog)
    , core_(core)
    , ctor_(std::move(ctor))
    , filename_(get_source_file(*ctor_))
    , downloadDir_(get_download_dir(*ctor_))
    , file_list_(gtr_get_widget_derived<FileList>(builder, "files_view_scroll", "files_view", core_, 0))
    , run_check_(gtr_get_widget<Gtk::CheckButton>(builder, "start_check"))
    , trash_check_(gtr_get_widget<Gtk::CheckButton>(builder, "trash_check"))
    , priority_combo_(gtr_get_widget<Gtk::ComboBox>(builder, "priority_combo"))
    , freespace_label_(gtr_get_widget_derived<FreeSpaceLabel>(builder, "free_space_label", core_, downloadDir_))
{
    dialog_.set_default_response(TR_GTK_RESPONSE_TYPE(ACCEPT));
    dialog.signal_response().connect(sigc::mem_fun(*this, &Impl::addResponseCB));

    gtr_priority_combo_init(*priority_combo_);
    gtr_combo_box_set_active_enum(*priority_combo_, TR_PRI_NORMAL);

    auto* source_chooser = gtr_get_widget_derived<PathButton>(builder, "source_button");
    addTorrentFilters(source_chooser);
    source_chooser->signal_selection_changed().connect([this, source_chooser]() { sourceChanged(source_chooser); });

    auto* destination_chooser = gtr_get_widget_derived<PathButton>(builder, "destination_button");
    destination_chooser->set_filename(downloadDir_);
    destination_chooser->set_shortcut_folders(gtr_get_recent_dirs("download"));

    destination_chooser->signal_selection_changed().connect([this, destination_chooser]()
                                                            { downloadDirChanged(destination_chooser); });

    bool flag = false;
    if (!tr_ctorGetPaused(ctor_.get(), TR_FORCE, &flag))
    {
        g_assert_not_reached();
    }

    run_check_->set_active(!flag);

    if (!tr_ctorGetDeleteSource(ctor_.get(), &flag))
    {
        g_assert_not_reached();
    }

    trash_check_->set_active(flag);

    /* trigger sourceChanged, either directly or indirectly,
     * so that it creates the tor/gtor objects */
    if (!filename_.empty())
    {
        source_chooser->set_filename(filename_);
    }
    else
    {
        sourceChanged(source_chooser);
    }

    dialog_.get_widget_for_response(TR_GTK_RESPONSE_TYPE(ACCEPT))->grab_focus();
}

/****
*****
****/

void TorrentFileChooserDialog::onOpenDialogResponse(int response, Glib::RefPtr<Session> const& core)
{
    if (response == TR_GTK_RESPONSE_TYPE(ACCEPT))
    {
        bool const do_start = gtr_pref_flag_get(TR_KEY_start_added_torrents);
        bool const do_prompt = get_choice(std::string(ShowOptionsDialogChoice)) == "true";
        bool const do_notify = false;

        auto const files = IF_GTKMM4(get_files2, get_files)();
        g_assert(!files.empty());

        /* remember this folder the next time we use this dialog */
        if (auto const folder = IF_GTKMM4(get_current_folder, get_current_folder_file)(); folder != nullptr)
        {
            gtr_pref_string_set(TR_KEY_open_dialog_dir, folder->get_path());
        }
        else if (auto const parent = files.front()->get_parent(); parent != nullptr)
        {
            gtr_pref_string_set(TR_KEY_open_dialog_dir, parent->get_path());
        }

        core->add_files(files, do_start, do_prompt, do_notify);
    }
}

std::unique_ptr<TorrentFileChooserDialog> TorrentFileChooserDialog::create(
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core)
{
    return std::unique_ptr<TorrentFileChooserDialog>(new TorrentFileChooserDialog(parent, core));
}

TorrentFileChooserDialog::TorrentFileChooserDialog(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
    : Gtk::FileChooserNative(_("Open a Torrent"), parent, TR_GTK_FILE_CHOOSER_ACTION(OPEN), _("_Open"), _("_Cancel"))
{
    set_modal(true);

    set_select_multiple(true);
    addTorrentFilters(this);
    signal_response().connect([this, core](int response) { onOpenDialogResponse(response, core); });

    if (auto const folder = gtr_pref_string_get(TR_KEY_open_dialog_dir); !folder.empty())
    {
        IF_GTKMM4(set_current_folder, set_current_folder_file)(Gio::File::create_for_path(folder));
    }

    add_choice(std::string(ShowOptionsDialogChoice), _("Show options dialog"));
    set_choice(std::string(ShowOptionsDialogChoice), gtr_pref_flag_get(TR_KEY_show_options_window) ? "true" : "false");
}

/***
****
***/

void TorrentUrlChooserDialog::onOpenURLResponse(int response, Gtk::Entry const& entry, Glib::RefPtr<Session> const& core)
{
    if (response == TR_GTK_RESPONSE_TYPE(CANCEL))
    {
        close();
    }
    else if (response == TR_GTK_RESPONSE_TYPE(ACCEPT))
    {
        auto const url = gtr_str_strip(entry.get_text());

        if (url.empty())
        {
            return;
        }

        if (core->add_from_url(url))
        {
            close();
        }
        else
        {
            gtr_unrecognized_url_dialog(*this, url);
        }
    }
}

std::unique_ptr<TorrentUrlChooserDialog> TorrentUrlChooserDialog::create(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("TorrentUrlChooserDialog.ui"));
    return std::unique_ptr<TorrentUrlChooserDialog>(
        gtr_get_widget_derived<TorrentUrlChooserDialog>(builder, "TorrentUrlChooserDialog", parent, core));
}

TorrentUrlChooserDialog::TorrentUrlChooserDialog(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core)
    : Gtk::Dialog(cast_item)
{
    set_transient_for(parent);

    auto* const e = gtr_get_widget<Gtk::Entry>(builder, "url_entry");
    gtr_paste_clipboard_url_into_entry(*e);

    signal_response().connect([this, e, core](int response) { onOpenURLResponse(response, *e, core); });

    if (e->get_text_length() == 0)
    {
        e->grab_focus();
    }
    else
    {
        get_widget_for_response(TR_GTK_RESPONSE_TYPE(ACCEPT))->grab_focus();
    }
}
