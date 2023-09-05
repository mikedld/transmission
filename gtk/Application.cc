// This file Copyright © 2005-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include "Application.h"

#include "Actions.h"
#include "DetailsDialog.h"
#include "Dialogs.h"
#include "FilterBar.h"
#include "GtkCompat.h"
#include "HigWorkarea.h" // GUI_PAD, GUI_PAD_BIG
#include "MainWindow.h"
#include "MakeDialog.h"
#include "MessageLogWindow.h"
#include "OptionsDialog.h"
#include "PathButton.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "RelocateDialog.h"
#include "Session.h"
#include "StatsDialog.h"
#include "SystemTrayIcon.h"
#include "Torrent.h"
#include "Utils.h"

#include <libtransmission/transmission.h>
#include <libtransmission/log.h>
#include <libtransmission/rpcimpl.h>
#include <libtransmission/utils.h>
#include <libtransmission/version.h>

#include <gdkmm/display.h>
#include <giomm/appinfo.h>
#include <giomm/error.h>
#include <giomm/menu.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/value.h>
#include <glibmm/vectorutils.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/builder.h>
#include <gtkmm/button.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/grid.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/window.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gtkmm/droptarget.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/shortcutcontroller.h>
#else
#include <gdkmm/dragcontext.h>
#include <gtkmm/selectiondata.h>
#endif

#include <fmt/core.h>

#include <algorithm>
#include <csignal>
#include <cstdlib> // exit()
#include <ctime>
#include <iterator> // std::back_inserter
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <glib/gmessages.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

using namespace std::literals;

#if GTKMM_CHECK_VERSION(4, 0, 0)
using FileListValue = Glib::Value<GSList*>;
using FileListHandler = Glib::SListHandler<Glib::RefPtr<Gio::File>>;

using StringValue = Glib::Value<Glib::ustring>;
#endif

#define SHOW_LICENSE

namespace
{

auto const AppIconName = "transmission"sv; // TODO(C++20): Use ""s

char const* const LICENSE =
    "Copyright 2005-2023. All code is copyrighted by the respective authors.\n"
    "\n"
    "Transmission can be redistributed and/or modified under the terms of the "
    "GNU GPL, versions 2 or 3, or by any future license endorsed by Mnemosyne LLC."
    "\n"
    "In addition, linking to and/or using OpenSSL is allowed.\n"
    "\n"
    "This program is distributed in the hope that it will be useful, "
    "but WITHOUT ANY WARRANTY; without even the implied warranty of "
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"
    "\n"
    "Some of Transmission's source files have more permissive licenses. "
    "Those files may, of course, be used on their own under their own terms.\n";

} // namespace

class Application::Impl
{
public:
    Impl(Application& app, std::string const& config_dir, bool start_paused, bool start_iconified);
    ~Impl() = default;

    TR_DISABLE_COPY_MOVE(Impl)

    void open_files(std::vector<Glib::RefPtr<Gio::File>> const& files);

    void on_startup();
    void on_activate();

    void actions_handler(Glib::ustring const& action_name);

private:
    struct counts_data
    {
        int total_count = 0;
        int queued_count = 0;
        int stopped_count = 0;
    };

private:
    void show_details_dialog_for_selected_torrents();
    void show_about_dialog();

    bool refresh_actions();
    void refresh_actions_soon();

    void on_main_window_size_allocated();
    void on_main_window_focus_in();

#if GTKMM_CHECK_VERSION(4, 0, 0)
    bool on_drag_data_received(Glib::ValueBase const& value, double x, double y);
#else
    void on_drag_data_received(
        Glib::RefPtr<Gdk::DragContext> const& drag_context,
        gint x,
        gint y,
        Gtk::SelectionData const& selection_data,
        guint info,
        guint time_);
#endif

    bool on_rpc_changed_idle(tr_rpc_callback_type type, tr_torrent_id_t torrent_id);

    void placeWindowFromPrefs();
    void presentMainWindow();
    void hideMainWindow();
    void toggleMainWindow();

    bool winclose();
    void rowChangedCB(std::unordered_set<tr_torrent_id_t> const& torrent_ids, Torrent::ChangeFlags changes);

    void app_setup();
    void main_window_setup();

    bool on_session_closed();
    void on_app_exit();

    void show_torrent_errors(Glib::ustring const& primary, std::vector<std::string>& files);
    void flush_torrent_errors();

    bool update_model_once();
    void update_model_soon();
    bool update_model_loop();

    void on_core_busy(bool busy);
    void on_core_error(Session::ErrorCode code, Glib::ustring const& msg);
    void on_add_torrent(tr_ctor* ctor);
    void on_prefs_changed(tr_quark key);

    [[nodiscard]] std::vector<tr_torrent_id_t> get_selected_torrent_ids() const;
    [[nodiscard]] counts_data get_selected_torrent_counts() const;

    void start_all_torrents();
    void pause_all_torrents();
    void copy_magnet_link_to_clipboard(Glib::RefPtr<Torrent> const& torrent) const;
    bool call_rpc_for_selected_torrents(std::string const& method);
    void remove_selected(bool delete_files);

    static tr_rpc_callback_status on_rpc_changed(
        tr_session* session,
        tr_rpc_callback_type type,
        tr_torrent* tor,
        gpointer gdata);

private:
    Application& app_;

    std::string const config_dir_;
    bool const start_paused_;
    bool const start_iconified_;
    bool is_iconified_ = false;
    bool is_closing_ = false;

    Glib::RefPtr<Gtk::Builder> ui_builder_;

    unsigned int activation_count_ = 0;
    sigc::connection timer_;
    sigc::connection update_model_soon_tag_;
    sigc::connection refresh_actions_tag_;
    std::unique_ptr<SystemTrayIcon> icon_;
    std::unique_ptr<MainWindow> wind_;
    Glib::RefPtr<Session> core_;
    std::unique_ptr<MessageLogWindow> msgwin_;
    std::unique_ptr<PrefsDialog> prefs_;
    std::vector<std::string> error_list_;
    std::vector<std::string> duplicates_list_;
    std::map<std::string, std::unique_ptr<DetailsDialog>> details_;
};

namespace
{

template<typename T>
void gtr_window_present(T const& window)
{
    window->present(GDK_CURRENT_TIME);
}

/***
****
****  DETAILS DIALOGS MANAGEMENT
****
***/

std::string get_details_dialog_key(std::vector<tr_torrent_id_t> const& id_list)
{
    auto tmp = id_list;
    std::sort(tmp.begin(), tmp.end());

    std::ostringstream gstr;

    for (auto const id : tmp)
    {
        gstr << id << ' ';
    }

    return gstr.str();
}

} // namespace

std::vector<tr_torrent_id_t> Application::Impl::get_selected_torrent_ids() const
{
    std::vector<tr_torrent_id_t> ids;
    wind_->for_each_selected_torrent([&ids](auto const& torrent) { ids.push_back(torrent->get_id()); });
    return ids;
}

void Application::Impl::show_details_dialog_for_selected_torrents()
{
    auto const ids = get_selected_torrent_ids();
    auto const key = get_details_dialog_key(ids);

    auto dialog_it = details_.find(key);

    if (dialog_it == details_.end())
    {
        auto dialog = DetailsDialog::create(*wind_, core_);
        dialog->set_torrents(ids);
        gtr_window_on_close(*dialog, [this, key]() { details_.erase(key); });
        dialog_it = details_.try_emplace(key, std::move(dialog)).first;
        dialog_it->second->show();
    }

    gtr_window_present(dialog_it->second);
}

/****
*****
*****  ON SELECTION CHANGED
*****
****/

Application::Impl::counts_data Application::Impl::get_selected_torrent_counts() const
{
    counts_data counts;

    wind_->for_each_selected_torrent(
        [&counts](auto const& torrent)
        {
            ++counts.total_count;

            auto const activity = torrent->get_activity();

            if (activity == TR_STATUS_DOWNLOAD_WAIT || activity == TR_STATUS_SEED_WAIT)
            {
                ++counts.queued_count;
            }

            if (activity == TR_STATUS_STOPPED)
            {
                ++counts.stopped_count;
            }
        });

    return counts;
}

bool Application::Impl::refresh_actions()
{
    if (!is_closing_)
    {
        size_t const total = core_->get_torrent_count();
        size_t const active = core_->get_active_torrent_count();
        auto const torrent_count = core_->get_model()->get_n_items();

        auto const sel_counts = get_selected_torrent_counts();
        bool const has_selection = sel_counts.total_count > 0;

        gtr_action_set_sensitive("select-all", torrent_count != 0);
        gtr_action_set_sensitive("deselect-all", torrent_count != 0);
        gtr_action_set_sensitive("pause-all-torrents", active != 0);
        gtr_action_set_sensitive("start-all-torrents", active != total);

        gtr_action_set_sensitive("torrent-stop", (sel_counts.stopped_count < sel_counts.total_count));
        gtr_action_set_sensitive("torrent-start", (sel_counts.stopped_count) > 0);
        gtr_action_set_sensitive("torrent-start-now", (sel_counts.stopped_count + sel_counts.queued_count) > 0);
        gtr_action_set_sensitive("torrent-verify", has_selection);
        gtr_action_set_sensitive("remove-torrent", has_selection);
        gtr_action_set_sensitive("delete-torrent", has_selection);
        gtr_action_set_sensitive("relocate-torrent", has_selection);
        gtr_action_set_sensitive("queue-move-top", has_selection);
        gtr_action_set_sensitive("queue-move-up", has_selection);
        gtr_action_set_sensitive("queue-move-down", has_selection);
        gtr_action_set_sensitive("queue-move-bottom", has_selection);
        gtr_action_set_sensitive("show-torrent-properties", has_selection);
        gtr_action_set_sensitive("open-torrent-folder", sel_counts.total_count == 1);
        gtr_action_set_sensitive("copy-magnet-link-to-clipboard", sel_counts.total_count == 1);

        bool const can_update = wind_ != nullptr &&
            wind_->for_each_selected_torrent_until([](auto const& torrent)
                                                   { return tr_torrentCanManualUpdate(&torrent->get_underlying()); });
        gtr_action_set_sensitive("torrent-reannounce", can_update);
    }

    refresh_actions_tag_.disconnect();
    return false;
}

void Application::Impl::refresh_actions_soon()
{
    if (!is_closing_ && !refresh_actions_tag_.connected())
    {
        refresh_actions_tag_ = Glib::signal_idle().connect(sigc::mem_fun(*this, &Impl::refresh_actions));
    }
}

/***
****
***/

namespace
{

bool has_magnet_link_handler()
{
    return bool{ Gio::AppInfo::get_default_for_uri_scheme("magnet") };
}

void register_magnet_link_handler()
{
    std::string const content_type = "x-scheme-handler/magnet";

    try
    {
        auto const app = Gio::AppInfo::create_from_commandline(
            "transmission-gtk",
            "transmission-gtk",
            TR_GIO_APP_INFO_CREATE_FLAGS(SUPPORTS_URIS));
        app->set_as_default_for_type(content_type);
    }
    catch (Gio::Error const& e)
    {
        gtr_warning(fmt::format(
            _("Couldn't register Transmission as a {content_type} handler: {error} ({error_code})"),
            fmt::arg("content_type", content_type),
            fmt::arg("error", e.what()),
            fmt::arg("error_code", static_cast<int>(e.code()))));
    }
}

void ensure_magnet_handler_exists()
{
    if (!has_magnet_link_handler())
    {
        register_magnet_link_handler();
    }
}

} // namespace

void Application::Impl::on_main_window_size_allocated()
{
#if GTKMM_CHECK_VERSION(4, 0, 0)
    bool const is_maximized = wind_->is_maximized();
#else
    auto const gdk_window = wind_->get_window();
    bool const is_maximized = gdk_window != nullptr && (gdk_window->get_state() & Gdk::WINDOW_STATE_MAXIMIZED) != 0;
#endif

    gtr_pref_flag_set(TR_KEY_main_window_is_maximized, is_maximized);

    if (!is_maximized)
    {
#if !GTKMM_CHECK_VERSION(4, 0, 0)
        int x = 0;
        int y = 0;
        wind_->get_position(x, y);
        gtr_pref_int_set(TR_KEY_main_window_x, x);
        gtr_pref_int_set(TR_KEY_main_window_y, y);
#endif

        int w = 0;
        int h = 0;
#if GTKMM_CHECK_VERSION(4, 0, 0)
        wind_->get_default_size(w, h);
#else
        wind_->get_size(w, h);
#endif
        gtr_pref_int_set(TR_KEY_main_window_width, w);
        gtr_pref_int_set(TR_KEY_main_window_height, h);
    }
}

/***
**** listen to changes that come from RPC
***/

bool Application::Impl::on_rpc_changed_idle(tr_rpc_callback_type type, tr_torrent_id_t torrent_id)
{
    switch (type)
    {
    case TR_RPC_SESSION_CLOSE:
        gtr_action_activate("quit");
        break;

    case TR_RPC_TORRENT_ADDED:
        if (auto* tor = core_->find_torrent(torrent_id); tor != nullptr)
        {
            core_->add_torrent(Torrent::create(tor), true);
        }

        break;

    case TR_RPC_TORRENT_REMOVING:
        core_->remove_torrent(torrent_id, false);
        break;

    case TR_RPC_TORRENT_TRASHING:
        core_->remove_torrent(torrent_id, true);
        break;

    case TR_RPC_SESSION_CHANGED:
        {
            tr_variant* newval = nullptr;
            tr_variant* oldvals = gtr_pref_get_all();
            tr_quark key = TR_KEY_NONE;
            std::vector<tr_quark> changed_keys;
            auto const* const session = core_->get_session();
            auto tmp = tr_variant::make_map(100U);
            tr_sessionGetSettings(session, &tmp);

            auto const serde = tr_variant_serde::benc();
            for (int i = 0; tr_variantDictChild(&tmp, i, &key, &newval); ++i)
            {
                bool changed = true;

                if (tr_variant const* oldval = tr_variantDictFind(oldvals, key); oldval != nullptr)
                {
                    changed = serde.to_string(*oldval) != serde.to_string(*newval);
                }

                if (changed)
                {
                    changed_keys.push_back(key);
                }
            }

            tr_sessionGetSettings(session, oldvals);

            for (auto const changed_key : changed_keys)
            {
                core_->signal_prefs_changed().emit(changed_key);
            }
            break;
        }

    case TR_RPC_TORRENT_CHANGED:
    case TR_RPC_TORRENT_MOVED:
    case TR_RPC_TORRENT_STARTED:
    case TR_RPC_TORRENT_STOPPED:
    case TR_RPC_SESSION_QUEUE_POSITIONS_CHANGED:
        /* nothing interesting to do here */
        break;

    default:
        g_assert_not_reached();
    }

    return false;
}

tr_rpc_callback_status Application::Impl::on_rpc_changed(
    tr_session* /*session*/,
    tr_rpc_callback_type type,
    tr_torrent* tor,
    gpointer gdata)
{
    auto* impl = static_cast<Impl*>(gdata);
    auto const torrent_id = tr_torrentId(tor);

    Glib::signal_idle().connect([impl, type, torrent_id]() { return impl->on_rpc_changed_idle(type, torrent_id); });

    return TR_RPC_NOREMOVE;
}

/***
****  signal handling
***/

namespace
{

#ifdef G_OS_UNIX

gboolean signal_handler(gpointer user_data)
{
    gtr_message(_("Got termination signal, trying to shut down cleanly. Do it again if it gets stuck."));
    gtr_actions_handler("quit", user_data);
    return G_SOURCE_REMOVE;
}

#endif

} // namespace

/****
*****
*****
****/

void Application::on_startup()
{
    Gtk::Application::on_startup();

    impl_->on_startup();
}

void Application::Impl::on_startup()
{
    IF_GTKMM4(Gtk::IconTheme::get_for_display(Gdk::Display::get_default()), Gtk::IconTheme::get_default())
        ->add_resource_path(gtr_get_full_resource_path("icons"s));
    Gtk::Window::set_default_icon_name(std::string(AppIconName));

    /* Add style provider to the window. */
    auto css_provider = Gtk::CssProvider::create();
    css_provider->load_from_resource(gtr_get_full_resource_path("transmission-ui.css"));
    Gtk::StyleContext::IF_GTKMM4(add_provider_for_display, add_provider_for_screen)(
        IF_GTKMM4(Gdk::Display::get_default(), Gdk::Screen::get_default()),
        css_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    FilterBar();
    PathButton();

    tr_session* session = nullptr;

#ifdef G_OS_UNIX
    g_unix_signal_add(SIGINT, &signal_handler, this);
    g_unix_signal_add(SIGTERM, &signal_handler, this);
#endif

    /* ensure the directories are created */
    if (auto const str = gtr_pref_string_get(TR_KEY_download_dir); !str.empty())
    {
        (void)g_mkdir_with_parents(str.c_str(), 0777);
    }

    if (auto const str = gtr_pref_string_get(TR_KEY_incomplete_dir); !str.empty())
    {
        (void)g_mkdir_with_parents(str.c_str(), 0777);
    }

    /* initialize the libtransmission session */
    session = tr_sessionInit(config_dir_.c_str(), true, gtr_pref_get_all());

    gtr_pref_flag_set(TR_KEY_alt_speed_enabled, tr_sessionUsesAltSpeed(session));
    gtr_pref_int_set(TR_KEY_peer_port, tr_sessionGetPeerPort(session));
    core_ = Session::create(session);

    /* init the ui manager */
    ui_builder_ = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("transmission-ui.xml"s));
    auto const actions = gtr_actions_init(ui_builder_, this);

    auto const main_menu = gtr_action_get_object<Gio::Menu>("main-window-menu");
    app_.set_menubar(main_menu);

    /* create main window now to be a parent to any error dialogs */
    wind_ = MainWindow::create(app_, actions, core_);
    wind_->set_show_menubar(true);
#if GTKMM_CHECK_VERSION(4, 0, 0)
    wind_->property_maximized().signal_changed().connect(sigc::mem_fun(*this, &Impl::on_main_window_size_allocated));
    wind_->property_default_width().signal_changed().connect(sigc::mem_fun(*this, &Impl::on_main_window_size_allocated));
    wind_->property_default_height().signal_changed().connect(sigc::mem_fun(*this, &Impl::on_main_window_size_allocated));
#else
    wind_->signal_size_allocate().connect(sigc::hide<0>(sigc::mem_fun(*this, &Impl::on_main_window_size_allocated)));
#endif

#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto const shortcut_controller = Gtk::ShortcutController::create(gtr_shortcuts_get_from_menu(main_menu));
    shortcut_controller->set_scope(Gtk::ShortcutScope::GLOBAL);
    wind_->add_controller(shortcut_controller);
#endif

    app_.hold();
    app_setup();
    tr_sessionSetRPCCallback(session, &Impl::on_rpc_changed, this);

    /* check & see if it's time to update the blocklist */
    if (gtr_pref_flag_get(TR_KEY_blocklist_enabled) && gtr_pref_flag_get(TR_KEY_blocklist_updates_enabled))
    {
        int64_t const last_time = gtr_pref_int_get(TR_KEY_blocklist_date);
        int const SECONDS_IN_A_WEEK = 7 * 24 * 60 * 60;
        time_t const now = time(nullptr);

        if (last_time + SECONDS_IN_A_WEEK < now)
        {
            core_->blocklist_update();
        }
    }

    /* if there's no magnet link handler registered, register us */
    ensure_magnet_handler_exists();
}

void Application::on_activate()
{
    Gtk::Application::on_activate();

    impl_->on_activate();
}

void Application::Impl::on_activate()
{
    activation_count_++;

    /* GApplication emits an 'activate' signal when bootstrapping the primary.
     * Ordinarily we handle that by presenting the main window, but if the user
     * started Transmission minimized, ignore that initial signal... */
    if (start_iconified_ && activation_count_ == 1)
    {
        return;
    }

    gtr_action_activate("present-main-window");
}

void Application::Impl::open_files(std::vector<Glib::RefPtr<Gio::File>> const& files)
{
    bool const do_start = gtr_pref_flag_get(TR_KEY_start_added_torrents) && !start_paused_;
    bool const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);
    bool const do_notify = true;

    core_->add_files(files, do_start, do_prompt, do_notify);
}

void Application::on_open(std::vector<Glib::RefPtr<Gio::File>> const& files, Glib::ustring const& hint)
{
    Gtk::Application::on_open(files, hint);

    impl_->open_files(files);
}

namespace
{

std::string get_application_id(std::string const& config_dir)
{
    struct stat sb = {};
    (void)::stat(config_dir.c_str(), &sb);
    return fmt::format("com.transmissionbt.transmission_{}_{}", sb.st_dev, sb.st_ino);
}

} // namespace

Application::Application(std::string const& config_dir, bool start_paused, bool start_iconified)
    : Gtk::Application(get_application_id(config_dir), TR_GIO_APPLICATION_FLAGS(HANDLES_OPEN))
    , impl_(std::make_unique<Impl>(*this, config_dir, start_paused, start_iconified))
{
}

Application::~Application() = default;

Application::Impl::Impl(Application& app, std::string const& config_dir, bool start_paused, bool start_iconified)
    : app_(app)
    , config_dir_(config_dir)
    , start_paused_(start_paused)
    , start_iconified_(start_iconified)
{
}

void Application::Impl::on_core_busy(bool busy)
{
    wind_->set_busy(busy);
}

void Application::Impl::app_setup()
{
    if (start_iconified_)
    {
        gtr_pref_flag_set(TR_KEY_show_notification_area_icon, true);
    }

    gtr_actions_set_core(core_);

    /* set up core handlers */
    core_->signal_busy().connect(sigc::mem_fun(*this, &Impl::on_core_busy));
    core_->signal_add_error().connect(sigc::mem_fun(*this, &Impl::on_core_error));
    core_->signal_add_prompt().connect(sigc::mem_fun(*this, &Impl::on_add_torrent));
    core_->signal_prefs_changed().connect(sigc::mem_fun(*this, &Impl::on_prefs_changed));

    /* add torrents from command-line and saved state */
    core_->load(start_paused_);
    core_->torrents_added();

    /* set up main window */
    main_window_setup();

    /* set up the icon */
    on_prefs_changed(TR_KEY_show_notification_area_icon);

    /* start model update timer */
    timer_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(*this, &Impl::update_model_loop),
        MAIN_WINDOW_REFRESH_INTERVAL_SECONDS);
    update_model_once();

    /* either show the window or iconify it */
    if (!start_iconified_)
    {
        wind_->show();
        gtr_action_set_toggled("toggle-main-window", true);
    }
    else
    {
        gtr_window_set_skip_taskbar_hint(*wind_, icon_ != nullptr);
        gtr_action_set_toggled("toggle-main-window", false);
    }

    if (!gtr_pref_flag_get(TR_KEY_user_has_given_informed_consent))
    {
        auto w = std::make_shared<Gtk::MessageDialog>(
            *wind_,
            _("Transmission is a file sharing program. When you run a torrent, its data will be "
              "made available to others by means of upload. Any content you share is your sole responsibility."),
            false,
            TR_GTK_MESSAGE_TYPE(OTHER),
            TR_GTK_BUTTONS_TYPE(NONE),
            true);
        w->add_button(_("_Cancel"), TR_GTK_RESPONSE_TYPE(REJECT));
        w->add_button(_("I _Agree"), TR_GTK_RESPONSE_TYPE(ACCEPT));
        w->set_default_response(TR_GTK_RESPONSE_TYPE(ACCEPT));
        w->signal_response().connect(
            [w](int response) mutable
            {
                if (response == TR_GTK_RESPONSE_TYPE(ACCEPT))
                {
                    // only show it once
                    gtr_pref_flag_set(TR_KEY_user_has_given_informed_consent, true);
                    w.reset();
                }
                else
                {
                    exit(0);
                }
            });
        w->show();
    }
}

void Application::Impl::placeWindowFromPrefs()
{
#if GTKMM_CHECK_VERSION(4, 0, 0)
    wind_->set_default_size((int)gtr_pref_int_get(TR_KEY_main_window_width), (int)gtr_pref_int_get(TR_KEY_main_window_height));
#else
    wind_->resize((int)gtr_pref_int_get(TR_KEY_main_window_width), (int)gtr_pref_int_get(TR_KEY_main_window_height));
    wind_->move((int)gtr_pref_int_get(TR_KEY_main_window_x), (int)gtr_pref_int_get(TR_KEY_main_window_y));
#endif
}

void Application::Impl::presentMainWindow()
{
    gtr_action_set_toggled("toggle-main-window", true);

    if (is_iconified_)
    {
        is_iconified_ = false;

        gtr_window_set_skip_taskbar_hint(*wind_, false);
    }

    if (!wind_->get_visible())
    {
        placeWindowFromPrefs();
        gtr_widget_set_visible(*wind_, true);
    }

    gtr_window_present(wind_);
    gtr_window_raise(*wind_);
}

void Application::Impl::hideMainWindow()
{
    gtr_action_set_toggled("toggle-main-window", false);

    gtr_window_set_skip_taskbar_hint(*wind_, true);
    gtr_widget_set_visible(*wind_, false);
    is_iconified_ = true;
}

void Application::Impl::toggleMainWindow()
{
    if (is_iconified_)
    {
        presentMainWindow();
    }
    else
    {
        hideMainWindow();
    }
}

bool Application::Impl::winclose()
{
    if (icon_ != nullptr)
    {
        gtr_action_activate("toggle-main-window");
    }
    else
    {
        on_app_exit();
    }

    return true; /* don't propagate event further */
}

void Application::Impl::rowChangedCB(std::unordered_set<tr_torrent_id_t> const& torrent_ids, Torrent::ChangeFlags changes)
{
    if (changes.test(Torrent::ChangeFlag::ACTIVITY) &&
        wind_->for_each_selected_torrent_until([&torrent_ids](auto const& torrent)
                                               { return torrent_ids.find(torrent->get_id()) != torrent_ids.end(); }))
    {
        refresh_actions_soon();
    }
}

#if GTKMM_CHECK_VERSION(4, 0, 0)

bool Application::Impl::on_drag_data_received(Glib::ValueBase const& value, double /*x*/, double /*y*/)
{
    if (G_VALUE_HOLDS(value.gobj(), GDK_TYPE_FILE_LIST))
    {
        FileListValue files_value;
        files_value.init(value.gobj());
        open_files(FileListHandler::slist_to_vector(files_value.get(), Glib::OwnershipType::OWNERSHIP_NONE));
        return true;
    }

    if (G_VALUE_HOLDS(value.gobj(), StringValue::value_type()))
    {
        StringValue string_value;
        string_value.init(value.gobj());
        if (auto const text = gtr_str_strip(string_value.get()); !text.empty())
        {
            return core_->add_from_url(text);
        }
    }

    return false;
}

#else

void Application::Impl::on_drag_data_received(
    Glib::RefPtr<Gdk::DragContext> const& drag_context,
    gint /*x*/,
    gint /*y*/,
    Gtk::SelectionData const& selection_data,
    guint /*info*/,
    guint time_)
{
    if (auto const uris = selection_data.get_uris(); !uris.empty())
    {
        auto files = std::vector<Glib::RefPtr<Gio::File>>();
        files.reserve(uris.size());
        std::transform(uris.begin(), uris.end(), std::back_inserter(files), &Gio::File::create_for_uri);

        open_files(files);
    }
    else
    {
        auto const text = gtr_str_strip(selection_data.get_text());

        if (!text.empty())
        {
            core_->add_from_url(text);
        }
    }

    drag_context->drag_finish(true, false, time_);
}

#endif

void Application::Impl::main_window_setup()
{
    wind_->signal_selection_changed().connect(sigc::mem_fun(*this, &Impl::refresh_actions_soon));
    refresh_actions_soon();
    core_->signal_torrents_changed().connect(sigc::mem_fun(*this, &Impl::rowChangedCB));
    gtr_window_on_close(*wind_, sigc::mem_fun(*this, &Impl::winclose));
    refresh_actions();

    /* register to handle URIs that get dragged onto our main window */
#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto drop_controller = Gtk::DropTarget::create(G_TYPE_INVALID, Gdk::DragAction::COPY);
    drop_controller->set_gtypes({ StringValue::value_type(), GDK_TYPE_FILE_LIST });
    drop_controller->signal_drop().connect(sigc::mem_fun(*this, &Impl::on_drag_data_received), false);
    wind_->add_controller(drop_controller);
#else
    wind_->drag_dest_set(Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_COPY);
    wind_->drag_dest_add_uri_targets();
    wind_->drag_dest_add_text_targets(); /* links dragged from browsers are text */
    wind_->signal_drag_data_received().connect(sigc::mem_fun(*this, &Impl::on_drag_data_received));
#endif
}

bool Application::Impl::on_session_closed()
{
    details_.clear();

    prefs_.reset();
    wind_.reset();

    core_.reset();

    icon_.reset();

    error_list_.clear();
    duplicates_list_.clear();

    app_.release();
    return false;
}

void Application::Impl::on_app_exit()
{
    if (is_closing_)
    {
        return;
    }

    is_closing_ = true;

    refresh_actions_tag_.disconnect();
    update_model_soon_tag_.disconnect();
    timer_.disconnect();

#if !GTKMM_CHECK_VERSION(4, 0, 0)
    wind_->remove();
#endif

    wind_->set_show_menubar(false);

    auto* p = Gtk::make_managed<Gtk::Grid>();
    p->set_column_spacing(GUI_PAD_BIG);
    p->set_halign(TR_GTK_ALIGN(CENTER));
    p->set_valign(TR_GTK_ALIGN(CENTER));
#if GTKMM_CHECK_VERSION(4, 0, 0)
    wind_->set_child(*p);
#else
    wind_->add(*p);
#endif

    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->property_icon_name() = "network-workgroup";
    icon->property_icon_size() = IF_GTKMM4(Gtk::IconSize::LARGE, Gtk::ICON_SIZE_DIALOG);
    p->attach(*icon, 0, 0, 1, 2);

    auto* top_label = Gtk::make_managed<Gtk::Label>();
    top_label->set_markup(fmt::format(FMT_STRING("<b>{:s}</b>"), _("Closing Connections…")));
    top_label->set_halign(TR_GTK_ALIGN(START));
    top_label->set_valign(TR_GTK_ALIGN(CENTER));
    p->attach(*top_label, 1, 0, 1, 1);

    auto* bottom_label = Gtk::make_managed<Gtk::Label>(_("Sending upload/download totals to tracker…"));
    bottom_label->set_halign(TR_GTK_ALIGN(START));
    bottom_label->set_valign(TR_GTK_ALIGN(CENTER));
    p->attach(*bottom_label, 1, 1, 1, 1);

    auto* button = Gtk::make_managed<Gtk::Button>(_("_Quit Now"), true);
    button->set_margin_top(GUI_PAD);
    button->set_halign(TR_GTK_ALIGN(START));
    button->set_valign(TR_GTK_ALIGN(END));
    button->signal_clicked().connect([]() { ::exit(0); });
    p->attach(*button, 1, 2, 1, 1);

#if !GTKMM_CHECK_VERSION(4, 0, 0)
    p->show_all();
#endif
    button->grab_focus();

    /* clear the UI */
    core_->clear();

    /* ensure the window is in its previous position & size.
     * this seems to be necessary because changing the main window's
     * child seems to unset the size */
    placeWindowFromPrefs();

    /* shut down libT */
    /* since tr_sessionClose () is a blocking function,
     * delegate its call to another thread here... when it's done,
     * punt the GUI teardown back to the GTK+ thread */
    std::thread(
        [this, session = core_->close()]()
        {
            tr_sessionClose(session);
            Glib::signal_idle().connect(sigc::mem_fun(*this, &Impl::on_session_closed));
        })
        .detach();
}

void Application::Impl::show_torrent_errors(Glib::ustring const& primary, std::vector<std::string>& files)
{
    std::ostringstream s;
    auto const leader = files.size() > 1 ? gtr_get_unicode_string(GtrUnicode::Bullet) : "";

    for (auto const& f : files)
    {
        s << leader << ' ' << f << '\n';
    }

    auto w = std::make_shared<Gtk::MessageDialog>(
        *wind_,
        primary,
        false,
        TR_GTK_MESSAGE_TYPE(ERROR),
        TR_GTK_BUTTONS_TYPE(CLOSE),
        true);
    w->set_secondary_text(s.str());
    w->signal_response().connect([w](int /*response*/) mutable { w.reset(); });
    w->show();

    files.clear();
}

void Application::Impl::flush_torrent_errors()
{
    if (!error_list_.empty())
    {
        show_torrent_errors(
            ngettext("Couldn't add corrupt torrent", "Couldn't add corrupt torrents", error_list_.size()),
            error_list_);
    }

    if (!duplicates_list_.empty())
    {
        show_torrent_errors(
            ngettext("Couldn't add duplicate torrent", "Couldn't add duplicate torrents", duplicates_list_.size()),
            duplicates_list_);
    }
}

void Application::Impl::on_core_error(Session::ErrorCode code, Glib::ustring const& msg)
{
    switch (code)
    {
    case Session::ERR_ADD_TORRENT_ERR:
        error_list_.push_back(Glib::path_get_basename(msg.raw()));
        break;

    case Session::ERR_ADD_TORRENT_DUP:
        duplicates_list_.push_back(msg);
        break;

    case Session::ERR_NO_MORE_TORRENTS:
        flush_torrent_errors();
        break;

    default:
        g_assert_not_reached();
        break;
    }
}

void Application::Impl::on_main_window_focus_in()
{
    if (wind_ != nullptr)
    {
        gtr_window_set_urgency_hint(*wind_, false);
    }
}

void Application::Impl::on_add_torrent(tr_ctor* ctor)
{
    auto w = std::shared_ptr<OptionsDialog>(
        OptionsDialog::create(*wind_, core_, std::unique_ptr<tr_ctor, decltype(&tr_ctorFree)>(ctor, &tr_ctorFree)));

    gtr_window_on_close(*w, [w]() mutable { w.reset(); });

#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto focus_controller = Gtk::EventControllerFocus::create();
    focus_controller->signal_enter().connect(sigc::mem_fun(*this, &Impl::on_main_window_focus_in));
    w->add_controller(focus_controller);
#else
    w->signal_focus_in_event().connect_notify(sigc::hide<0>(sigc::mem_fun(*this, &Impl::on_main_window_focus_in)));
#endif

    if (wind_ != nullptr)
    {
        gtr_window_set_urgency_hint(*wind_, true);
    }

    w->show();
}

void Application::Impl::on_prefs_changed(tr_quark const key)
{
    auto* tr = core_->get_session();

    switch (key)
    {
    case TR_KEY_encryption:
        tr_sessionSetEncryption(tr, static_cast<tr_encryption_mode>(gtr_pref_int_get(key)));
        break;

    case TR_KEY_default_trackers:
        tr_sessionSetDefaultTrackers(tr, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_download_dir:
        tr_sessionSetDownloadDir(tr, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_message_level:
        tr_logSetLevel(static_cast<tr_log_level>(gtr_pref_int_get(key)));
        break;

    case TR_KEY_peer_port:
        tr_sessionSetPeerPort(tr, gtr_pref_int_get(key));
        break;

    case TR_KEY_blocklist_enabled:
        tr_blocklistSetEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_blocklist_url:
        tr_blocklistSetURL(tr, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_show_notification_area_icon:
        if (bool const show = gtr_pref_flag_get(key); show && icon_ == nullptr)
        {
            icon_ = SystemTrayIcon::create(*wind_, core_);
        }
        else if (!show && icon_ != nullptr)
        {
            icon_.reset();
        }
        break;

    case TR_KEY_speed_limit_down_enabled:
        tr_sessionLimitSpeed(tr, TR_DOWN, gtr_pref_flag_get(key));
        break;

    case TR_KEY_speed_limit_down:
        tr_sessionSetSpeedLimit_KBps(tr, TR_DOWN, gtr_pref_int_get(key));
        break;

    case TR_KEY_speed_limit_up_enabled:
        tr_sessionLimitSpeed(tr, TR_UP, gtr_pref_flag_get(key));
        break;

    case TR_KEY_speed_limit_up:
        tr_sessionSetSpeedLimit_KBps(tr, TR_UP, gtr_pref_int_get(key));
        break;

    case TR_KEY_ratio_limit_enabled:
        tr_sessionSetRatioLimited(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_ratio_limit:
        tr_sessionSetRatioLimit(tr, gtr_pref_double_get(key));
        break;

    case TR_KEY_idle_seeding_limit:
        tr_sessionSetIdleLimit(tr, gtr_pref_int_get(key));
        break;

    case TR_KEY_idle_seeding_limit_enabled:
        tr_sessionSetIdleLimited(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_port_forwarding_enabled:
        tr_sessionSetPortForwardingEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_pex_enabled:
        tr_sessionSetPexEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_rename_partial_files:
        tr_sessionSetIncompleteFileNamingEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_download_queue_size:
        tr_sessionSetQueueSize(tr, TR_DOWN, gtr_pref_int_get(key));
        break;

    case TR_KEY_queue_stalled_minutes:
        tr_sessionSetQueueStalledMinutes(tr, gtr_pref_int_get(key));
        break;

    case TR_KEY_dht_enabled:
        tr_sessionSetDHTEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_utp_enabled:
        tr_sessionSetUTPEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_lpd_enabled:
        tr_sessionSetLPDEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_rpc_port:
        tr_sessionSetRPCPort(tr, gtr_pref_int_get(key));
        break;

    case TR_KEY_rpc_enabled:
        tr_sessionSetRPCEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_rpc_whitelist:
        tr_sessionSetRPCWhitelist(tr, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_rpc_whitelist_enabled:
        tr_sessionSetRPCWhitelistEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_rpc_username:
        tr_sessionSetRPCUsername(tr, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_rpc_password:
        tr_sessionSetRPCPassword(tr, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_rpc_authentication_required:
        tr_sessionSetRPCPasswordEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_alt_speed_up:
        tr_sessionSetAltSpeed_KBps(tr, TR_UP, gtr_pref_int_get(key));
        break;

    case TR_KEY_alt_speed_down:
        tr_sessionSetAltSpeed_KBps(tr, TR_DOWN, gtr_pref_int_get(key));
        break;

    case TR_KEY_alt_speed_enabled:
        {
            bool const b = gtr_pref_flag_get(key);
            tr_sessionUseAltSpeed(tr, b);
            gtr_action_set_toggled(std::string(tr_quark_get_string_view(key)), b);
            break;
        }

    case TR_KEY_alt_speed_time_begin:
        tr_sessionSetAltSpeedBegin(tr, gtr_pref_int_get(key));
        break;

    case TR_KEY_alt_speed_time_end:
        tr_sessionSetAltSpeedEnd(tr, gtr_pref_int_get(key));
        break;

    case TR_KEY_alt_speed_time_enabled:
        tr_sessionUseAltSpeedTime(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_alt_speed_time_day:
        tr_sessionSetAltSpeedDay(tr, static_cast<tr_sched_day>(gtr_pref_int_get(key)));
        break;

    case TR_KEY_peer_port_random_on_start:
        tr_sessionSetPeerPortRandomOnStart(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_incomplete_dir:
        tr_sessionSetIncompleteDir(tr, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_incomplete_dir_enabled:
        tr_sessionSetIncompleteDirEnabled(tr, gtr_pref_flag_get(key));
        break;

    case TR_KEY_script_torrent_done_enabled:
        tr_sessionSetScriptEnabled(tr, TR_SCRIPT_ON_TORRENT_DONE, gtr_pref_flag_get(key));
        break;

    case TR_KEY_script_torrent_done_filename:
        tr_sessionSetScript(tr, TR_SCRIPT_ON_TORRENT_DONE, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_script_torrent_done_seeding_enabled:
        tr_sessionSetScriptEnabled(tr, TR_SCRIPT_ON_TORRENT_DONE_SEEDING, gtr_pref_flag_get(key));
        break;

    case TR_KEY_script_torrent_done_seeding_filename:
        tr_sessionSetScript(tr, TR_SCRIPT_ON_TORRENT_DONE_SEEDING, gtr_pref_string_get(key).c_str());
        break;

    case TR_KEY_start_added_torrents:
        tr_sessionSetPaused(tr, !gtr_pref_flag_get(key));
        break;

    case TR_KEY_trash_original_torrent_files:
        tr_sessionSetDeleteSource(tr, gtr_pref_flag_get(key));
        break;

    default:
        break;
    }
}

bool Application::Impl::update_model_once()
{
    /* update the torrent data in the model */
    core_->update();

    /* refresh the main window's statusbar and toolbar buttons */
    if (wind_ != nullptr)
    {
        wind_->refresh();
    }

    /* update the actions */
    refresh_actions();

    /* update the status tray icon */
    if (icon_ != nullptr)
    {
        icon_->refresh();
    }

    update_model_soon_tag_.disconnect();
    return false;
}

void Application::Impl::update_model_soon()
{
    if (!update_model_soon_tag_.connected())
    {
        update_model_soon_tag_ = Glib::signal_idle().connect(sigc::mem_fun(*this, &Impl::update_model_once));
    }
}

bool Application::Impl::update_model_loop()
{
    if (!is_closing_)
    {
        update_model_once();
    }

    return !is_closing_;
}

void Application::Impl::show_about_dialog()
{
    auto const uri = Glib::ustring("https://transmissionbt.com/");
    auto const authors = std::vector<Glib::ustring>({
        "Charles Kerr (Backend; GTK+)",
        "Mitchell Livingston (Backend; macOS)",
        "Mike Gelfand",
    });

    auto d = std::make_shared<Gtk::AboutDialog>();
    d->set_authors(authors);
    d->set_comments(_("A fast and easy BitTorrent client"));
    d->set_copyright(_("Copyright © The Transmission Project"));
    d->set_logo_icon_name(std::string(AppIconName));
    d->set_name(Glib::get_application_name());
    /* Translators: translate "translator-credits" as your name
       to have it appear in the credits in the "About"
       dialog */
    d->set_translator_credits(_("translator-credits"));
    d->set_version(LONG_VERSION_STRING);
    d->set_website(uri);
    d->set_website_label(uri);
#ifdef SHOW_LICENSE
    d->set_license(LICENSE);
    d->set_wrap_license(true);
#endif
    d->set_transient_for(*wind_);
    d->set_modal(true);
    gtr_window_on_close(*d, [d]() mutable { d.reset(); });
#if !GTKMM_CHECK_VERSION(4, 0, 0)
    d->signal_response().connect_notify([&dref = *d](int /*response*/) { dref.close(); });
#endif
    d->show();
}

bool Application::Impl::call_rpc_for_selected_torrents(std::string const& method)
{
    bool invoked = false;
    auto* session = core_->get_session();

    auto top = tr_variant::make_map(2U);
    tr_variantDictAddStrView(&top, TR_KEY_method, method);
    auto* const args = tr_variantDictAddDict(&top, TR_KEY_arguments, 1);
    auto* const ids = tr_variantDictAddList(args, TR_KEY_ids, 0);
    wind_->for_each_selected_torrent([ids](auto const& torrent) { tr_variantListAddInt(ids, torrent->get_id()); });

    if (tr_variantListSize(ids) != 0)
    {
        tr_rpc_request_exec_json(session, &top, nullptr, nullptr);
        invoked = true;
    }

    return invoked;
}

void Application::Impl::remove_selected(bool delete_files)
{
    if (auto const l = get_selected_torrent_ids(); !l.empty())
    {
        gtr_confirm_remove(*wind_, core_, l, delete_files);
    }
}

void Application::Impl::start_all_torrents()
{
    auto* session = core_->get_session();
    auto request = tr_variant::make_map(1U);
    tr_variantDictAddStrView(&request, TR_KEY_method, "torrent-start"sv);
    tr_rpc_request_exec_json(session, &request, nullptr, nullptr);
}

void Application::Impl::pause_all_torrents()
{
    auto* session = core_->get_session();
    auto request = tr_variant::make_map(1U);
    tr_variantDictAddStrView(&request, TR_KEY_method, "torrent-stop"sv);
    tr_rpc_request_exec_json(session, &request, nullptr, nullptr);
}

void Application::Impl::copy_magnet_link_to_clipboard(Glib::RefPtr<Torrent> const& torrent) const
{
    auto const magnet = tr_torrentGetMagnetLink(&torrent->get_underlying());
    auto const display = wind_->get_display();

    /* this is The Right Thing for copy/paste... */
    IF_GTKMM4(display->get_clipboard(), Gtk::Clipboard::get_for_display(display, GDK_SELECTION_CLIPBOARD))->set_text(magnet);

    /* ...but people using plain ol' X need this instead */
    IF_GTKMM4(display->get_primary_clipboard(), Gtk::Clipboard::get_for_display(display, GDK_SELECTION_PRIMARY))
        ->set_text(magnet);
}

void gtr_actions_handler(Glib::ustring const& action_name, gpointer user_data)
{
    static_cast<Application::Impl*>(user_data)->actions_handler(action_name);
}

void Application::Impl::actions_handler(Glib::ustring const& action_name)
{
    bool changed = false;

    if (action_name == "open-torrent-from-url")
    {
        auto w = std::shared_ptr<TorrentUrlChooserDialog>(TorrentUrlChooserDialog::create(*wind_, core_));
        gtr_window_on_close(*w, [w]() mutable { w.reset(); });
        w->show();
    }
    else if (action_name == "open-torrent")
    {
        auto w = std::shared_ptr<TorrentFileChooserDialog>(TorrentFileChooserDialog::create(*wind_, core_));
        gtr_window_on_close(*w, [w]() mutable { w.reset(); });
        w->show();
    }
    else if (action_name == "show-stats")
    {
        auto dialog = std::shared_ptr<StatsDialog>(StatsDialog::create(*wind_, core_));
        gtr_window_on_close(*dialog, [dialog]() mutable { dialog.reset(); });
        dialog->show();
    }
    else if (action_name == "donate")
    {
        gtr_open_uri("https://transmissionbt.com/donate/");
    }
    else if (action_name == "pause-all-torrents")
    {
        pause_all_torrents();
    }
    else if (action_name == "start-all-torrents")
    {
        start_all_torrents();
    }
    else if (action_name == "copy-magnet-link-to-clipboard")
    {
        wind_->for_each_selected_torrent_until(
            sigc::bind_return(sigc::mem_fun(*this, &Impl::copy_magnet_link_to_clipboard), true));
    }
    else if (action_name == "relocate-torrent")
    {
        auto const ids = get_selected_torrent_ids();

        if (!ids.empty())
        {
            auto w = std::shared_ptr<RelocateDialog>(RelocateDialog::create(*wind_, core_, ids));
            gtr_window_on_close(*w, [w]() mutable { w.reset(); });
            w->show();
        }
    }
    else if (
        action_name == "torrent-start" || action_name == "torrent-start-now" || action_name == "torrent-stop" ||
        action_name == "torrent-reannounce" || action_name == "torrent-verify" || action_name == "queue-move-top" ||
        action_name == "queue-move-up" || action_name == "queue-move-down" || action_name == "queue-move-bottom")
    {
        changed = call_rpc_for_selected_torrents(action_name);
    }
    else if (action_name == "open-torrent-folder")
    {
        wind_->for_each_selected_torrent([this](auto const& torrent) { core_->open_folder(torrent->get_id()); });
    }
    else if (action_name == "show-torrent-properties")
    {
        show_details_dialog_for_selected_torrents();
    }
    else if (action_name == "new-torrent")
    {
        auto w = std::shared_ptr<MakeDialog>(MakeDialog::create(*wind_, core_));
        gtr_window_on_close(*w, [w]() mutable { w.reset(); });
        w->show();
    }
    else if (action_name == "remove-torrent")
    {
        remove_selected(false);
    }
    else if (action_name == "delete-torrent")
    {
        remove_selected(true);
    }
    else if (action_name == "quit")
    {
        on_app_exit();
    }
    else if (action_name == "select-all")
    {
        wind_->select_all();
    }
    else if (action_name == "deselect-all")
    {
        wind_->unselect_all();
    }
    else if (action_name == "edit-preferences")
    {
        if (prefs_ == nullptr)
        {
            prefs_ = PrefsDialog::create(*wind_, core_);
            gtr_window_on_close(*prefs_, [this]() { prefs_.reset(); });
        }

        gtr_window_present(prefs_);
    }
    else if (action_name == "toggle-message-log")
    {
        if (msgwin_ == nullptr)
        {
            msgwin_ = MessageLogWindow::create(*wind_, core_);
            gtr_window_on_close(
                *msgwin_,
                [this]()
                {
                    gtr_action_set_toggled("toggle-message-log", false);
                    msgwin_.reset();
                });

            gtr_action_set_toggled("toggle-message-log", true);
            msgwin_->show();
        }
        else
        {
            msgwin_->close();
        }
    }
    else if (action_name == "show-about-dialog")
    {
        show_about_dialog();
    }
    else if (action_name == "help")
    {
        gtr_open_uri(gtr_get_help_uri());
    }
    else if (action_name == "toggle-main-window")
    {
        toggleMainWindow();
    }
    else if (action_name == "present-main-window")
    {
        presentMainWindow();
    }
    else
    {
        gtr_error(fmt::format("Unhandled action: {}", action_name));
    }

    if (changed)
    {
        update_model_soon();
    }
}
