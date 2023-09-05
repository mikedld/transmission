// This file Copyright © 2021-2023 Transmission authors and contributors.
// This file is licensed under the MIT (SPDX: MIT) license,
// A copy of this license can be found in licenses/ .

#include "Session.h"

#include "Actions.h"
#include "ListModelAdapter.h"
#include "Notify.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "SortListModel.hh"
#include "Torrent.h"
#include "TorrentSorter.h"
#include "Utils.h"

#include <libtransmission/transmission.h>
#include <libtransmission/log.h>
#include <libtransmission/rpcimpl.h>
#include <libtransmission/torrent-metainfo.h>
#include <libtransmission/tr-assert.h>
#include <libtransmission/utils.h> // tr_time()
#include <libtransmission/variant.h>
#include <libtransmission/web-utils.h> // tr_urlIsValid()

#include <giomm/asyncresult.h>
#include <giomm/dbusconnection.h>
#include <giomm/fileinfo.h>
#include <giomm/filemonitor.h>
#include <giomm/liststore.h>
#include <glibmm/error.h>
#include <glibmm/fileutils.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/stringutils.h>
#include <glibmm/variant.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gtkmm/sortlistmodel.h>
#else
#include <gtkmm/treemodelsort.h>
#endif

#include <fmt/core.h>

#include <algorithm>
#include <cinttypes> // PRId64
#include <cstring> // strstr
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace std::literals;

class Session::Impl
{
public:
    Impl(Session& core, tr_session* session);
    ~Impl();

    TR_DISABLE_COPY_MOVE(Impl)

    tr_session* close();

    Glib::RefPtr<Gio::ListStore<Torrent>> get_raw_model() const;
    Glib::RefPtr<SortListModel<Torrent>> get_model();
    tr_session* get_session() const;

    std::pair<Glib::RefPtr<Torrent>, guint> find_torrent_by_id(tr_torrent_id_t torrent_id) const;

    size_t get_active_torrent_count() const;

    void update();
    void torrents_added();

    void add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify);
    int add_ctor(tr_ctor* ctor, bool do_prompt, bool do_notify);
    void add_torrent(Glib::RefPtr<Torrent> const& torrent, bool do_notify);
    bool add_from_url(Glib::ustring const& url);

    void send_rpc_request(tr_variant const* request, int64_t tag, std::function<void(tr_variant&)> const& response_func);

    void commit_prefs_change(tr_quark key);

    auto& signal_add_error()
    {
        return signal_add_error_;
    }

    auto& signal_add_prompt()
    {
        return signal_add_prompt_;
    }

    auto& signal_blocklist_updated()
    {
        return signal_blocklist_updated_;
    }

    auto& signal_busy()
    {
        return signal_busy_;
    }

    auto& signal_prefs_changed()
    {
        return signal_prefs_changed_;
    }

    auto& signal_port_tested()
    {
        return signal_port_tested_;
    }

    auto& signal_torrents_changed()
    {
        return signal_torrents_changed_;
    }

    [[nodiscard]] constexpr auto& favicon_cache()
    {
        return favicon_cache_;
    }

private:
    Glib::RefPtr<Session> get_core_ptr() const;

    bool is_busy() const;
    void add_to_busy(int addMe);
    void inc_busy();
    void dec_busy();

    bool add_file(Glib::RefPtr<Gio::File> const& file, bool do_start, bool do_prompt, bool do_notify);
    void add_file_async_callback(
        Glib::RefPtr<Gio::File> const& file,
        Glib::RefPtr<Gio::AsyncResult>& result,
        tr_ctor* ctor,
        bool do_prompt,
        bool do_notify);

    Glib::RefPtr<Torrent> create_new_torrent(tr_ctor* ctor);

    void maybe_inhibit_hibernation();
    void set_hibernation_allowed(bool allowed);

    void watchdir_update();
    void watchdir_scan();
    void watchdir_monitor_file(Glib::RefPtr<Gio::File> const& file);
    bool watchdir_idle();
    void on_file_changed_in_watchdir(
        Glib::RefPtr<Gio::File> const& file,
        Glib::RefPtr<Gio::File> const& other_type,
        IF_GLIBMM2_68(Gio::FileMonitor::Event, Gio::FileMonitorEvent) event_type);

    void on_pref_changed(tr_quark key);

    void on_torrent_completeness_changed(tr_torrent* tor, tr_completeness completeness, bool was_running);
    void on_torrent_metadata_changed(tr_torrent* raw_torrent);

private:
    Session& core_;

    sigc::signal<void(ErrorCode, Glib::ustring const&)> signal_add_error_;
    sigc::signal<void(tr_ctor*)> signal_add_prompt_;
    sigc::signal<void(bool)> signal_blocklist_updated_;
    sigc::signal<void(bool)> signal_busy_;
    sigc::signal<void(tr_quark)> signal_prefs_changed_;
    sigc::signal<void(bool)> signal_port_tested_;
    sigc::signal<void(std::unordered_set<tr_torrent_id_t> const&, Torrent::ChangeFlags)> signal_torrents_changed_;

    Glib::RefPtr<Gio::FileMonitor> monitor_;
    sigc::connection monitor_tag_;
    Glib::RefPtr<Gio::File> monitor_dir_;
    std::vector<Glib::RefPtr<Gio::File>> monitor_files_;
    sigc::connection monitor_idle_tag_;

    bool adding_from_watch_dir_ = false;
    bool inhibit_allowed_ = false;
    bool have_inhibit_cookie_ = false;
    bool dbus_error_ = false;
    guint inhibit_cookie_ = 0;
    gint busy_count_ = 0;
    Glib::RefPtr<Gio::ListStore<Torrent>> raw_model_;
    Glib::RefPtr<SortListModel<Torrent>> sorted_model_;
    Glib::RefPtr<TorrentSorter> sorter_ = TorrentSorter::create();
    tr_session* session_ = nullptr;

    FaviconCache<Glib::RefPtr<Gdk::Pixbuf>> favicon_cache_;
};

Glib::RefPtr<Session> Session::Impl::get_core_ptr() const
{
    core_.reference();
    return Glib::make_refptr_for_instance(&core_);
}

/***
****
***/

Glib::RefPtr<Gio::ListStore<Torrent>> Session::Impl::get_raw_model() const
{
    return raw_model_;
}

Glib::RefPtr<Gio::ListModel> Session::get_model() const
{
    return impl_->get_raw_model();
}

Glib::RefPtr<Session::Model> Session::get_sorted_model() const
{
    return impl_->get_model();
}

Glib::RefPtr<SortListModel<Torrent>> Session::Impl::get_model()
{
    return sorted_model_;
}

tr_session* Session::get_session() const
{
    return impl_->get_session();
}

tr_session* Session::Impl::get_session() const
{
    return session_;
}

/***
****  BUSY
***/

bool Session::Impl::is_busy() const
{
    return busy_count_ > 0;
}

void Session::Impl::add_to_busy(int addMe)
{
    bool const wasBusy = is_busy();

    busy_count_ += addMe;

    if (wasBusy != is_busy())
    {
        signal_busy_.emit(is_busy());
    }
}

void Session::Impl::inc_busy()
{
    add_to_busy(1);
}

void Session::Impl::dec_busy()
{
    add_to_busy(-1);
}

/***
****
****  WATCHDIR
****
***/

namespace
{

time_t get_file_mtime(Glib::RefPtr<Gio::File> const& file)
{
    try
    {
        return file->query_info(G_FILE_ATTRIBUTE_TIME_MODIFIED)->get_attribute_uint64(G_FILE_ATTRIBUTE_TIME_MODIFIED);
    }
    catch (Glib::Error const&)
    {
        return 0;
    }
}

void rename_torrent(Glib::RefPtr<Gio::File> const& file)
{
    auto info = Glib::RefPtr<Gio::FileInfo>();

    try
    {
        info = file->query_info(G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);
    }
    catch (Glib::Error const&)
    {
        return;
    }

    auto const old_name = info->get_attribute_as_string(G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);
    auto const new_name = fmt::format("{}.added", old_name);

    try
    {
        file->set_display_name(new_name);
    }
    catch (Glib::Error const& e)
    {
        gtr_message(fmt::format(
            _("Couldn't rename '{old_path}' as '{path}': {error} ({error_code})"),
            fmt::arg("old_path", old_name),
            fmt::arg("path", new_name),
            fmt::arg("error", e.what()),
            fmt::arg("error_code", e.code())));
    }
}

} // namespace

bool Session::Impl::watchdir_idle()
{
    std::vector<Glib::RefPtr<Gio::File>> changing;
    std::vector<Glib::RefPtr<Gio::File>> unchanging;
    time_t const now = tr_time();

    /* separate the files into two lists: changing and unchanging */
    for (auto const& file : monitor_files_)
    {
        time_t const mtime = get_file_mtime(file);

        if (mtime + 2 >= now)
        {
            changing.push_back(file);
        }
        else
        {
            unchanging.push_back(file);
        }
    }

    /* add the files that have stopped changing */
    if (!unchanging.empty())
    {
        bool const do_start = gtr_pref_flag_get(TR_KEY_start_added_torrents);
        bool const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);

        adding_from_watch_dir_ = true;
        add_files(unchanging, do_start, do_prompt, true);
        std::for_each(unchanging.begin(), unchanging.end(), rename_torrent);
        adding_from_watch_dir_ = false;
    }

    /* keep monitoring the ones that are still changing */
    monitor_files_ = changing;

    /* if monitor_files is nonempty, keep checking every second */
    if (!monitor_files_.empty())
    {
        return true;
    }

    monitor_idle_tag_.disconnect();
    return false;
}

/* If this file is a torrent, add it to our list */
void Session::Impl::watchdir_monitor_file(Glib::RefPtr<Gio::File> const& file)
{
    auto const filename = file->get_path();
    bool const is_torrent = Glib::str_has_suffix(filename, ".torrent");

    if (is_torrent)
    {
        /* if we're not already watching this file, start watching it now */
        bool const found = std::any_of(
            monitor_files_.begin(),
            monitor_files_.end(),
            [file](auto const& f) { return file->equal(f); });

        if (!found)
        {
            monitor_files_.push_back(file);

            if (!monitor_idle_tag_.connected())
            {
                monitor_idle_tag_ = Glib::signal_timeout().connect_seconds(sigc::mem_fun(*this, &Impl::watchdir_idle), 1);
            }
        }
    }
}

/* GFileMonitor noticed a file was created */
void Session::Impl::on_file_changed_in_watchdir(
    Glib::RefPtr<Gio::File> const& file,
    Glib::RefPtr<Gio::File> const& /*other_type*/,
    IF_GLIBMM2_68(Gio::FileMonitor::Event, Gio::FileMonitorEvent) event_type)
{
    if (event_type == TR_GIO_FILE_MONITOR_EVENT(CREATED))
    {
        watchdir_monitor_file(file);
    }
}

/* walk through the pre-existing files in the watchdir */
void Session::Impl::watchdir_scan()
{
    auto const dirname = gtr_pref_string_get(TR_KEY_watch_dir);

    try
    {
        for (auto const& name : Glib::Dir(dirname))
        {
            watchdir_monitor_file(Gio::File::create_for_path(Glib::build_filename(dirname, name)));
        }
    }
    catch (Glib::FileError const&)
    {
    }
}

void Session::Impl::watchdir_update()
{
    bool const is_enabled = gtr_pref_flag_get(TR_KEY_watch_dir_enabled);
    auto const dir = Gio::File::create_for_path(gtr_pref_string_get(TR_KEY_watch_dir));

    if (monitor_ != nullptr && (!is_enabled || !dir->equal(monitor_dir_)))
    {
        monitor_tag_.disconnect();
        monitor_->cancel();

        monitor_dir_.reset();
        monitor_.reset();
    }

    if (!is_enabled || monitor_ != nullptr)
    {
        return;
    }

    auto monitor = Glib::RefPtr<Gio::FileMonitor>();

    try
    {
        monitor = dir->monitor_directory();
    }
    catch (Glib::Error const&)
    {
        return;
    }

    watchdir_scan();

    monitor_ = monitor;
    monitor_dir_ = dir;
    monitor_tag_ = monitor_->signal_changed().connect(sigc::mem_fun(*this, &Impl::on_file_changed_in_watchdir));
}

/***
****
***/

void Session::Impl::on_pref_changed(tr_quark const key)
{
    switch (key)
    {
    case TR_KEY_sort_mode:
        sorter_->set_mode(gtr_pref_string_get(TR_KEY_sort_mode));
        break;

    case TR_KEY_sort_reversed:
        sorter_->set_reversed(gtr_pref_flag_get(TR_KEY_sort_reversed));
        break;

    case TR_KEY_peer_limit_global:
        tr_sessionSetPeerLimit(session_, gtr_pref_int_get(key));
        break;

    case TR_KEY_peer_limit_per_torrent:
        tr_sessionSetPeerLimitPerTorrent(session_, gtr_pref_int_get(key));
        break;

    case TR_KEY_inhibit_desktop_hibernation:
        maybe_inhibit_hibernation();
        break;

    case TR_KEY_watch_dir:
    case TR_KEY_watch_dir_enabled:
        watchdir_update();
        break;

    default:
        break;
    }
}

/**
***
**/

Glib::RefPtr<Session> Session::create(tr_session* session)
{
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return Glib::make_refptr_for_instance(new Session(session));
}

Session::Session(tr_session* session)
    : Glib::ObjectBase(typeid(Session))
    , impl_(std::make_unique<Impl>(*this, session))
{
}

Session::~Session() = default;

Session::Impl::Impl(Session& core, tr_session* session)
    : core_{ core }
    , session_{ session }
{
    raw_model_ = Gio::ListStore<Torrent>::create();
    signal_torrents_changed_.connect(sigc::hide<0>(sigc::mem_fun(*sorter_, &TorrentSorter::update)));
    sorted_model_ = SortListModel<Torrent>::create(gtr_ptr_static_cast<Gio::ListModel>(raw_model_), sorter_);

    /* init from prefs & listen to pref changes */
    on_pref_changed(TR_KEY_sort_mode);
    on_pref_changed(TR_KEY_sort_reversed);
    on_pref_changed(TR_KEY_watch_dir_enabled);
    on_pref_changed(TR_KEY_peer_limit_global);
    on_pref_changed(TR_KEY_inhibit_desktop_hibernation);
    signal_prefs_changed_.connect([this](auto key) { on_pref_changed(key); });

    tr_sessionSetMetadataCallback(
        session,
        [](auto* /*session*/, auto* tor, gpointer impl) { static_cast<Impl*>(impl)->on_torrent_metadata_changed(tor); },
        this);

    tr_sessionSetCompletenessCallback(
        session,
        [](auto* tor, auto completeness, bool was_running, gpointer impl)
        { static_cast<Impl*>(impl)->on_torrent_completeness_changed(tor, completeness, was_running); },
        this);
}

Session::Impl::~Impl()
{
    monitor_idle_tag_.disconnect();
}

tr_session* Session::close()
{
    return impl_->close();
}

tr_session* Session::Impl::close()
{
    auto* session = session_;

    if (session != nullptr)
    {
        session_ = nullptr;
        gtr_pref_save(session);
    }

    return session;
}

/***
****  COMPLETENESS CALLBACK
***/

/* this is called in the libtransmission thread, *NOT* the GTK+ thread,
   so delegate to the GTK+ thread before calling notify's dbus code... */
void Session::Impl::on_torrent_completeness_changed(tr_torrent* tor, tr_completeness completeness, bool was_running)
{
    if (was_running && completeness != TR_LEECH && tr_torrentStat(tor)->sizeWhenDone != 0)
    {
        Glib::signal_idle().connect(
            [core = get_core_ptr(), torrent_id = tr_torrentId(tor)]()
            {
                gtr_notify_torrent_completed(core, torrent_id);
                return false;
            });
    }
}

/***
****  METADATA CALLBACK
***/

namespace
{

struct metadata_callback_data
{
    Session* core;
    tr_torrent_id_t torrent_id;
};

} // namespace

std::pair<Glib::RefPtr<Torrent>, guint> Session::Impl::find_torrent_by_id(tr_torrent_id_t torrent_id) const
{
    auto begin_position = 0U;
    auto end_position = raw_model_->get_n_items();

    while (begin_position < end_position)
    {
        auto const position = begin_position + (end_position - begin_position) / 2;
        auto const torrent = raw_model_->get_item(position);
        auto const current_torrent_id = torrent->get_id();

        if (current_torrent_id == torrent_id)
        {
            return { torrent, position };
        }

        (current_torrent_id < torrent_id ? begin_position : end_position) = position;
    }

    return {};
}

/* this is called in the libtransmission thread, *NOT* the GTK+ thread,
   so delegate to the GTK+ thread before changing our list store... */
void Session::Impl::on_torrent_metadata_changed(tr_torrent* raw_torrent)
{
    Glib::signal_idle().connect(
        [this, core = get_core_ptr(), torrent_id = tr_torrentId(raw_torrent)]()
        {
            /* update the torrent's collated name */
            if (auto const& [torrent, position] = find_torrent_by_id(torrent_id); torrent)
            {
                torrent->update();
            }

            return false;
        });
}

/***
****
****  ADDING TORRENTS
****
***/

void Session::add_torrent(Glib::RefPtr<Torrent> const& torrent, bool do_notify)
{
    impl_->add_torrent(torrent, do_notify);
}

void Session::Impl::add_torrent(Glib::RefPtr<Torrent> const& torrent, bool do_notify)
{
    if (torrent != nullptr)
    {
        raw_model_->insert_sorted(torrent, &Torrent::compare_by_id);

        if (do_notify)
        {
            gtr_notify_torrent_added(get_core_ptr(), torrent->get_id());
        }
    }
}

Glib::RefPtr<Torrent> Session::Impl::create_new_torrent(tr_ctor* ctor)
{
    bool do_trash = false;

    /* let the gtk client handle the removal, since libT
     * doesn't have any concept of the glib trash API */
    tr_ctorGetDeleteSource(ctor, &do_trash);
    tr_ctorSetDeleteSource(ctor, false);
    tr_torrent* const tor = tr_torrentNew(ctor, nullptr);

    if (tor != nullptr && do_trash)
    {
        char const* config = tr_sessionGetConfigDir(session_);
        char const* source = tr_ctorGetSourceFile(ctor);

        if (source != nullptr)
        {
            /* #1294: don't delete the .torrent file if it's our internal copy */
            bool const is_internal = strstr(source, config) == source;

            if (!is_internal)
            {
                gtr_file_trash_or_remove(source, nullptr);
            }
        }
    }

    return Torrent::create(tor);
}

int Session::Impl::add_ctor(tr_ctor* ctor, bool do_prompt, bool do_notify)
{
    auto const* metainfo = tr_ctorGetMetainfo(ctor);
    if (metainfo == nullptr)
    {
        return TR_PARSE_ERR;
    }

    if (tr_torrentFindFromMetainfo(get_session(), metainfo) != nullptr)
    {
        /* don't complain about torrent files in the watch directory
         * that have already been added... that gets annoying and we
         * don't want to be nagging users to clean up their watch dirs */
        if (tr_ctorGetSourceFile(ctor) == nullptr || !adding_from_watch_dir_)
        {
            signal_add_error_.emit(ERR_ADD_TORRENT_DUP, metainfo->name().c_str());
        }

        tr_ctorFree(ctor);
        return TR_PARSE_DUPLICATE;
    }

    if (!do_prompt)
    {
        add_torrent(create_new_torrent(ctor), do_notify);
        tr_ctorFree(ctor);
        return 0;
    }

    signal_add_prompt_.emit(ctor);
    return 0;
}

namespace
{

void core_apply_defaults(tr_ctor* ctor)
{
    if (!tr_ctorGetPaused(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetPaused(ctor, TR_FORCE, !gtr_pref_flag_get(TR_KEY_start_added_torrents));
    }

    if (!tr_ctorGetDeleteSource(ctor, nullptr))
    {
        tr_ctorSetDeleteSource(ctor, gtr_pref_flag_get(TR_KEY_trash_original_torrent_files));
    }

    if (!tr_ctorGetPeerLimit(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetPeerLimit(ctor, TR_FORCE, gtr_pref_int_get(TR_KEY_peer_limit_per_torrent));
    }

    if (!tr_ctorGetDownloadDir(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetDownloadDir(ctor, TR_FORCE, gtr_pref_string_get(TR_KEY_download_dir).c_str());
    }
}

} // namespace

void Session::add_ctor(tr_ctor* ctor)
{
    bool const do_notify = false;
    bool const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);
    core_apply_defaults(ctor);
    impl_->add_ctor(ctor, do_prompt, do_notify);
}

/***
****
***/

void Session::Impl::add_file_async_callback(
    Glib::RefPtr<Gio::File> const& file,
    Glib::RefPtr<Gio::AsyncResult>& result,
    tr_ctor* ctor,
    bool do_prompt,
    bool do_notify)
{
    try
    {
        gsize length = 0;
        char* contents = nullptr;

        if (!file->load_contents_finish(result, contents, length))
        {
            gtr_message(fmt::format(_("Couldn't read '{path}'"), fmt::arg("path", file->get_parse_name())));
        }
        else if (tr_ctorSetMetainfo(ctor, contents, length, nullptr))
        {
            add_ctor(ctor, do_prompt, do_notify);
        }
        else
        {
            tr_ctorFree(ctor);
        }
    }
    catch (Glib::Error const& e)
    {
        gtr_message(fmt::format(
            _("Couldn't read '{path}': {error} ({error_code})"),
            fmt::arg("path", file->get_parse_name()),
            fmt::arg("error", e.what()),
            fmt::arg("error_code", e.code())));
    }

    dec_busy();
}

bool Session::Impl::add_file(Glib::RefPtr<Gio::File> const& file, bool do_start, bool do_prompt, bool do_notify)
{
    auto const* const session = get_session();
    if (session == nullptr)
    {
        return false;
    }

    bool handled = false;
    auto* ctor = tr_ctorNew(session);
    core_apply_defaults(ctor);
    tr_ctorSetPaused(ctor, TR_FORCE, !do_start);

    bool loaded = false;
    if (auto const path = file->get_path(); !std::empty(path))
    {
        // try to treat it as a file...
        loaded = tr_ctorSetMetainfoFromFile(ctor, path.c_str(), nullptr);
    }

    if (!loaded)
    {
        // try to treat it as a magnet link...
        loaded = tr_ctorSetMetainfoFromMagnetLink(ctor, file->get_uri().c_str(), nullptr);
    }

    // if we could make sense of it, add it
    if (loaded)
    {
        handled = true;
        add_ctor(ctor, do_prompt, do_notify);
    }
    else if (tr_urlIsValid(file->get_uri()))
    {
        handled = true;
        inc_busy();
        file->load_contents_async([this, file, ctor, do_prompt, do_notify](auto& result)
                                  { add_file_async_callback(file, result, ctor, do_prompt, do_notify); });
    }
    else
    {
        tr_ctorFree(ctor);
        std::cerr << fmt::format(_("Couldn't add torrent file '{path}'"), fmt::arg("path", file->get_parse_name()))
                  << std::endl;
    }

    return handled;
}

bool Session::add_from_url(Glib::ustring const& url)
{
    return impl_->add_from_url(url);
}

bool Session::Impl::add_from_url(Glib::ustring const& url)
{
    auto const file = Gio::File::create_for_uri(url);
    auto const do_start = gtr_pref_flag_get(TR_KEY_start_added_torrents);
    auto const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);
    auto const do_notify = false;

    auto const handled = add_file(file, do_start, do_prompt, do_notify);
    torrents_added();
    return handled;
}

void Session::add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify)
{
    impl_->add_files(files, do_start, do_prompt, do_notify);
}

void Session::Impl::add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify)
{
    for (auto const& file : files)
    {
        add_file(file, do_start, do_prompt, do_notify);
    }

    torrents_added();
}

void Session::torrents_added()
{
    impl_->torrents_added();
}

void Session::Impl::torrents_added()
{
    update();
    signal_add_error_.emit(ERR_NO_MORE_TORRENTS, {});
}

void Session::torrent_changed(tr_torrent_id_t id)
{
    if (auto const& [torrent, position] = impl_->find_torrent_by_id(id); torrent)
    {
        torrent->update();
    }
}

void Session::remove_torrent(tr_torrent_id_t id, bool delete_files)
{
    if (auto const& [torrent, position] = impl_->find_torrent_by_id(id); torrent)
    {
        /* remove from the gui */
        impl_->get_raw_model()->remove(position);

        /* remove the torrent */
        tr_torrentRemove(
            &torrent->get_underlying(),
            delete_files,
            [](char const* filename, void* /*user_data*/, tr_error** error)
            { return gtr_file_trash_or_remove(filename, error); },
            nullptr);
    }
}

void Session::load(bool force_paused)
{
    auto* const ctor = tr_ctorNew(impl_->get_session());

    if (force_paused)
    {
        tr_ctorSetPaused(ctor, TR_FORCE, true);
    }

    tr_ctorSetPeerLimit(ctor, TR_FALLBACK, gtr_pref_int_get(TR_KEY_peer_limit_per_torrent));

    auto* session = impl_->get_session();
    auto const n_torrents = tr_sessionLoadTorrents(session, ctor);
    tr_ctorFree(ctor);

    auto raw_torrents = std::vector<tr_torrent*>{};
    raw_torrents.resize(n_torrents);
    tr_sessionGetAllTorrents(session, std::data(raw_torrents), std::size(raw_torrents));

    auto torrents = std::vector<Glib::RefPtr<Torrent>>();
    torrents.reserve(raw_torrents.size());
    std::transform(raw_torrents.begin(), raw_torrents.end(), std::back_inserter(torrents), &Torrent::create);
    std::sort(torrents.begin(), torrents.end(), &Torrent::less_by_id);

    auto const model = impl_->get_raw_model();
    model->splice(0, model->get_n_items(), torrents);
}

void Session::clear()
{
    impl_->get_raw_model()->remove_all();
}

/***
****
***/

void Session::update()
{
    impl_->update();
}

void Session::start_now(tr_torrent_id_t id)
{
    auto top = tr_variant::make_map(2U);
    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-start-now");

    auto* args = tr_variantDictAddDict(&top, TR_KEY_arguments, 1);
    auto* ids = tr_variantDictAddList(args, TR_KEY_ids, 1);
    tr_variantListAddInt(ids, id);
    exec(&top);
}

void Session::Impl::update()
{
    auto torrent_ids = std::unordered_set<tr_torrent_id_t>();
    auto changes = Torrent::ChangeFlags();

    /* update the model */
    for (auto i = 0U, count = raw_model_->get_n_items(); i < count; ++i)
    {
        auto const torrent = raw_model_->get_item(i);
        if (auto const torrent_changes = torrent->update(); torrent_changes.any())
        {
            torrent_ids.insert(torrent->get_id());
            changes |= torrent_changes;
        }
    }

    /* update hibernation */
    maybe_inhibit_hibernation();

    if (changes.any())
    {
        signal_torrents_changed_.emit(torrent_ids, changes);
    }
}

/**
***  Hibernate
**/

namespace
{

auto const SessionManagerServiceName = "org.gnome.SessionManager"sv; // TODO(C++20): Use ""s
auto const SessionManagerInterface = "org.gnome.SessionManager"sv; // TODO(C++20): Use ""s
auto const SessionManagerObjectPath = "/org/gnome/SessionManager"sv; // TODO(C++20): Use ""s

bool gtr_inhibit_hibernation(guint32& cookie)
{
    bool success = false;
    char const* application = "Transmission BitTorrent Client";
    char const* reason = "BitTorrent Activity";
    int const toplevel_xid = 0;
    int const flags = 4; /* Inhibit suspending the session or computer */

    try
    {
        auto const connection = Gio::DBus::Connection::get_sync(TR_GIO_DBUS_BUS_TYPE(SESSION));

        auto response = connection->call_sync(
            std::string(SessionManagerObjectPath),
            std::string(SessionManagerInterface),
            "Inhibit",
            Glib::VariantContainerBase::create_tuple({
                Glib::Variant<Glib::ustring>::create(application),
                Glib::Variant<guint32>::create(toplevel_xid),
                Glib::Variant<Glib::ustring>::create(reason),
                Glib::Variant<guint32>::create(flags),
            }),
            std::string(SessionManagerServiceName),
            1000);

        cookie = Glib::VariantBase::cast_dynamic<Glib::Variant<guint32>>(response.get_child(0)).get();

        /* logging */
        tr_logAddInfo(_("Inhibiting desktop hibernation"));

        success = true;
    }
    catch (Glib::Error const& e)
    {
        tr_logAddError(fmt::format(_("Couldn't inhibit desktop hibernation: {error}"), fmt::arg("error", e.what())));
    }

    return success;
}

void gtr_uninhibit_hibernation(guint inhibit_cookie)
{
    try
    {
        auto const connection = Gio::DBus::Connection::get_sync(TR_GIO_DBUS_BUS_TYPE(SESSION));

        connection->call_sync(
            std::string(SessionManagerObjectPath),
            std::string(SessionManagerInterface),
            "Uninhibit",
            Glib::VariantContainerBase::create_tuple({ Glib::Variant<guint32>::create(inhibit_cookie) }),
            std::string(SessionManagerServiceName),
            1000);

        /* logging */
        tr_logAddInfo(_("Allowing desktop hibernation"));
    }
    catch (Glib::Error const& e)
    {
        tr_logAddError(fmt::format(_("Couldn't inhibit desktop hibernation: {error}"), fmt::arg("error", e.what())));
    }
}

} // namespace

void Session::Impl::set_hibernation_allowed(bool allowed)
{
    inhibit_allowed_ = allowed;

    if (allowed && have_inhibit_cookie_)
    {
        gtr_uninhibit_hibernation(inhibit_cookie_);
        have_inhibit_cookie_ = false;
    }

    if (!allowed && !have_inhibit_cookie_ && !dbus_error_)
    {
        if (gtr_inhibit_hibernation(inhibit_cookie_))
        {
            have_inhibit_cookie_ = true;
        }
        else
        {
            dbus_error_ = true;
        }
    }
}

void Session::Impl::maybe_inhibit_hibernation()
{
    /* hibernation is allowed if EITHER
     * (a) the "inhibit" pref is turned off OR
     * (b) there aren't any active torrents */
    bool const hibernation_allowed = !gtr_pref_flag_get(TR_KEY_inhibit_desktop_hibernation) || get_active_torrent_count() == 0;
    set_hibernation_allowed(hibernation_allowed);
}

/**
***  Prefs
**/

void Session::Impl::commit_prefs_change(tr_quark const key)
{
    signal_prefs_changed_.emit(key);
    gtr_pref_save(session_);
}

void Session::set_pref(tr_quark const key, std::string const& newval)
{
    if (newval != gtr_pref_string_get(key))
    {
        gtr_pref_string_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, bool newval)
{
    if (newval != gtr_pref_flag_get(key))
    {
        gtr_pref_flag_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, int newval)
{
    if (newval != gtr_pref_int_get(key))
    {
        gtr_pref_int_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, double newval)
{
    if (std::fabs(newval - gtr_pref_double_get(key)) >= 0.0001)
    {
        gtr_pref_double_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

/***
****
****  RPC Interface
****
***/

/* #define DEBUG_RPC */

namespace
{

int64_t nextTag = 1;

std::map<int64_t, std::function<void(tr_variant&)>> pendingRequests;

bool core_read_rpc_response_idle(tr_variant& response)
{
    if (int64_t tag = 0; tr_variantDictFindInt(&response, TR_KEY_tag, &tag))
    {
        if (auto const data_it = pendingRequests.find(tag); data_it != pendingRequests.end())
        {
            if (auto const& response_func = data_it->second; response_func)
            {
                response_func(response);
            }

            pendingRequests.erase(data_it);
        }
        else
        {
            gtr_warning(fmt::format(_("Couldn't find pending RPC request for tag {tag}"), fmt::arg("tag", tag)));
        }
    }

    return false;
}

void core_read_rpc_response(tr_session* /*session*/, tr_variant* response, gpointer /*user_data*/)
{
    auto owned_response = std::make_shared<tr_variant>();
    tr_variantInitBool(owned_response.get(), false);
    std::swap(*owned_response, *response);

    Glib::signal_idle().connect([owned_response]() mutable { return core_read_rpc_response_idle(*owned_response); });
}

} // namespace

void Session::Impl::send_rpc_request(
    tr_variant const* request,
    int64_t tag,
    std::function<void(tr_variant&)> const& response_func)
{
    if (session_ == nullptr)
    {
        gtr_error("GTK+ client doesn't support connections to remote servers yet.");
    }
    else
    {
        /* remember this request */
        pendingRequests.try_emplace(tag, response_func);

        /* make the request */
#ifdef DEBUG_RPC
        gtr_message(fmt::format("request: [{}]", tr_variantToStr(request, TR_VARIANT_FMT_JSON_LEAN)));
#endif

        tr_rpc_request_exec_json(session_, request, core_read_rpc_response, nullptr);
    }
}

/***
****  Sending a test-port request via RPC
***/

void Session::port_test()
{
    auto const tag = nextTag;
    ++nextTag;

    auto request = tr_variant::make_map(2U);
    tr_variantDictAddStrView(&request, TR_KEY_method, "port-test");
    tr_variantDictAddInt(&request, TR_KEY_tag, tag);
    impl_->send_rpc_request(
        &request,
        tag,
        [this](auto& response)
        {
            tr_variant* args = nullptr;
            bool is_open = false;

            if (!tr_variantDictFindDict(&response, TR_KEY_arguments, &args) ||
                !tr_variantDictFindBool(args, TR_KEY_port_is_open, &is_open))
            {
                is_open = false;
            }

            impl_->signal_port_tested().emit(is_open);
        });
}

/***
****  Updating a blocklist via RPC
***/

void Session::blocklist_update()
{
    auto const tag = nextTag;
    ++nextTag;

    auto request = tr_variant::make_map(2U);
    tr_variantDictAddStrView(&request, TR_KEY_method, "blocklist-update");
    tr_variantDictAddInt(&request, TR_KEY_tag, tag);
    impl_->send_rpc_request(
        &request,
        tag,
        [this](auto& response)
        {
            tr_variant* args = nullptr;
            int64_t ruleCount = 0;

            if (!tr_variantDictFindDict(&response, TR_KEY_arguments, &args) ||
                !tr_variantDictFindInt(args, TR_KEY_blocklist_size, &ruleCount))
            {
                ruleCount = -1;
            }

            if (ruleCount > 0)
            {
                gtr_pref_int_set(TR_KEY_blocklist_date, tr_time());
            }

            impl_->signal_blocklist_updated().emit(ruleCount >= 0);
        });
}

/***
****
***/

void Session::exec(tr_variant const* request)
{
    auto const tag = nextTag;
    ++nextTag;

    impl_->send_rpc_request(request, tag, {});
}

/***
****
***/

size_t Session::get_torrent_count() const
{
    return impl_->get_raw_model()->get_n_items();
}

size_t Session::get_active_torrent_count() const
{
    return impl_->get_active_torrent_count();
}

size_t Session::Impl::get_active_torrent_count() const
{
    size_t activeCount = 0;

    for (auto i = 0U, count = raw_model_->get_n_items(); i < count; ++i)
    {
        if (raw_model_->get_item(i)->get_activity() != TR_STATUS_STOPPED)
        {
            ++activeCount;
        }
    }

    return activeCount;
}

tr_torrent* Session::find_torrent(tr_torrent_id_t id) const
{
    tr_torrent* tor = nullptr;

    if (auto* const session = impl_->get_session(); session != nullptr)
    {
        tor = tr_torrentFindFromId(session, id);
    }

    return tor;
}

FaviconCache<Glib::RefPtr<Gdk::Pixbuf>>& Session::favicon_cache() const
{
    return impl_->favicon_cache();
}

void Session::open_folder(tr_torrent_id_t torrent_id) const
{
    auto const* tor = find_torrent(torrent_id);

    if (tor != nullptr)
    {
        bool const single = tr_torrentFileCount(tor) == 1;
        char const* currentDir = tr_torrentGetCurrentDir(tor);

        if (single)
        {
            gtr_open_file(currentDir);
        }
        else
        {
            gtr_open_file(Glib::build_filename(currentDir, tr_torrentName(tor)));
        }
    }
}

sigc::signal<void(Session::ErrorCode, Glib::ustring const&)>& Session::signal_add_error()
{
    return impl_->signal_add_error();
}

sigc::signal<void(tr_ctor*)>& Session::signal_add_prompt()
{
    return impl_->signal_add_prompt();
}

sigc::signal<void(bool)>& Session::signal_blocklist_updated()
{
    return impl_->signal_blocklist_updated();
}

sigc::signal<void(bool)>& Session::signal_busy()
{
    return impl_->signal_busy();
}

sigc::signal<void(tr_quark)>& Session::signal_prefs_changed()
{
    return impl_->signal_prefs_changed();
}

sigc::signal<void(bool)>& Session::signal_port_tested()
{
    return impl_->signal_port_tested();
}

sigc::signal<void(std::unordered_set<tr_torrent_id_t> const&, Torrent::ChangeFlags)>& Session::signal_torrents_changed()
{
    return impl_->signal_torrents_changed();
}
