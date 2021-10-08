/******************************************************************************
 * Copyright (c) Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <string.h> /* strlen() */

#include <glibmm/i18n.h>

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h> /* tr_formatter_speed_KBps() */

#include "actions.h"
#include "conf.h"
#include "filter.h"
#include "hig.h"
#include "torrent-cell-renderer.h"
#include "tr-core.h"
#include "tr-prefs.h"
#include "tr-window.h"
#include "util.h"

class MainWindow::Impl
{
public:
    Impl(MainWindow& window, Glib::RefPtr<Gtk::UIManager> const& ui_mgr, TrCore* core);
    ~Impl();

    Glib::RefPtr<Gtk::TreeSelection> get_selection() const;

    void refresh();

    void prefsChanged(tr_quark key);

private:
    Gtk::TreeView* makeview(Glib::RefPtr<Gtk::TreeModel> const& model);

    Gtk::Menu* createOptionsMenu();
    Gtk::Menu* createSpeedMenu(tr_direction dir);
    Gtk::Menu* createRatioMenu();

    void onSpeedToggled(Gtk::CheckMenuItem* check, tr_direction dir, bool enabled);
    void onSpeedSet(tr_direction dir, int KBps);

    void onRatioToggled(Gtk::CheckMenuItem* check, bool enabled);
    void onRatioSet(double ratio);

    void updateStats();
    void updateSpeeds();

    void syncAltSpeedButton();

    bool onAskTrackerQueryTooltip(int x, int y, bool keyboard_tip, Glib::RefPtr<Gtk::Tooltip> tooltip);
    void status_menu_toggled_cb(Gtk::CheckMenuItem* menu_item, char const* val);
    void onOptionsClicked(Gtk::Button* button);
    void onYinYangClicked(Gtk::Button* button);
    void alt_speed_toggled_cb();
    void onAltSpeedToggledIdle();

private:
    Gtk::RadioMenuItem* speedlimit_on_item_[2] = { nullptr, nullptr };
    Gtk::RadioMenuItem* speedlimit_off_item_[2] = { nullptr, nullptr };
    Gtk::RadioMenuItem* ratio_on_item_ = nullptr;
    Gtk::RadioMenuItem* ratio_off_item_ = nullptr;
    Gtk::ScrolledWindow* scroll_ = nullptr;
    Gtk::TreeView* view_ = nullptr;
    Gtk::Toolbar* toolbar_ = nullptr;
    FilterBar* filter_ = nullptr;
    Gtk::Grid* status_ = nullptr;
    Gtk::Menu* status_menu_;
    Gtk::Label* ul_lb_ = nullptr;
    Gtk::Label* dl_lb_ = nullptr;
    Gtk::Label* stats_lb_ = nullptr;
    Gtk::Image* alt_speed_image_ = nullptr;
    Gtk::ToggleButton* alt_speed_button_ = nullptr;
    Gtk::Menu* options_menu_ = nullptr;
    Glib::RefPtr<Gtk::TreeSelection> selection_;
    TorrentCellRenderer* renderer_ = nullptr;
    Gtk::TreeViewColumn* column_ = nullptr;
    TrCore* core_ = nullptr;
    gulong pref_handler_id_ = 0;
};

/***
****
***/

namespace
{

void on_popup_menu(GdkEventButton* event)
{
    auto* menu = Glib::wrap(GTK_MENU(gtr_action_get_widget("/main-window-popup")));

#if GTK_CHECK_VERSION(3, 22, 0)
    menu->popup_at_pointer(reinterpret_cast<GdkEvent*>(event));
#else
    menu->popup(event != nullptr ? event->button : 0, event != nullptr ? event->time : 0);
#endif
}

bool tree_view_search_equal_func(
    Glib::RefPtr<Gtk::TreeModel> const& /*model*/,
    int /*column*/,
    Glib::ustring const& key,
    Gtk::TreeModel::iterator const& iter)
{
    auto const name = iter->get_value(torrent_cols.name_collated);
    return name.find(key.lowercase()) == Glib::ustring::npos;
}

} // namespace

Gtk::TreeView* MainWindow::Impl::makeview(Glib::RefPtr<Gtk::TreeModel> const& model)
{
    auto* view = Gtk::make_managed<Gtk::TreeView>();
    view->set_search_column(torrent_cols.name_collated);
    view->set_search_equal_func(&tree_view_search_equal_func);
    view->set_headers_visible(false);
    view->set_fixed_height_mode(true);

    selection_ = view->get_selection();

    column_ = new Gtk::TreeViewColumn();
    column_->set_title(_("Torrent"));
    column_->set_resizable(true);
    column_->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);

    renderer_ = Gtk::make_managed<TorrentCellRenderer>();
    column_->pack_start(*renderer_, false);
    column_->add_attribute(renderer_->property_torrent(), torrent_cols.torrent);
    column_->add_attribute(renderer_->property_piece_upload_speed(), torrent_cols.speed_up);
    column_->add_attribute(renderer_->property_piece_download_speed(), torrent_cols.speed_down);

    view->append_column(*column_);
    renderer_->property_xpad() = GUI_PAD_SMALL;
    renderer_->property_ypad() = GUI_PAD_SMALL;

    selection_->set_mode(Gtk::SELECTION_MULTIPLE);

    view->signal_popup_menu().connect_notify([]() { on_popup_menu(nullptr); });
    view->signal_button_press_event().connect(
        [view](GdkEventButton* event) { return on_tree_view_button_pressed(view, event, &on_popup_menu); },
        false);
    view->signal_button_release_event().connect([view](GdkEventButton* event)
                                                { return on_tree_view_button_released(view, event); });
    view->signal_row_activated().connect([](auto const& /*path*/, auto* /*column*/)
                                         { gtr_action_activate("show-torrent-properties"); });

    view->set_model(model);

    return view;
}

void MainWindow::Impl::prefsChanged(tr_quark const key)
{
    switch (key)
    {
    case TR_KEY_compact_view:
        renderer_->property_compact() = gtr_pref_flag_get(key);
        /* since the cell size has changed, we need gtktreeview to revalidate
         * its fixed-height mode values. Unfortunately there's not an API call
         * for that, but it *does* revalidate when it thinks the style's been tweaked */
        g_signal_emit_by_name(Glib::unwrap(view_), "style-updated", nullptr, nullptr);
        break;

    case TR_KEY_show_statusbar:
        status_->set_visible(gtr_pref_flag_get(key));
        break;

    case TR_KEY_show_filterbar:
        filter_->set_visible(gtr_pref_flag_get(key));
        break;

    case TR_KEY_show_toolbar:
        toolbar_->set_visible(gtr_pref_flag_get(key));
        break;

    case TR_KEY_statusbar_stats:
        refresh();
        break;

    case TR_KEY_alt_speed_enabled:
    case TR_KEY_alt_speed_up:
    case TR_KEY_alt_speed_down:
        syncAltSpeedButton();
        break;

    default:
        break;
    }
}

MainWindow::Impl::~Impl()
{
    g_signal_handler_disconnect(core_, pref_handler_id_);
}

void MainWindow::Impl::onYinYangClicked(Gtk::Button* button)
{
#if GTK_CHECK_VERSION(3, 22, 0)
    status_menu_->popup_at_widget(button, Gdk::GRAVITY_NORTH_EAST, Gdk::GRAVITY_SOUTH_EAST, nullptr);
#else
    status_menu_->popup(0, gtk_get_current_event_time());
#endif
}

void MainWindow::Impl::status_menu_toggled_cb(Gtk::CheckMenuItem* menu_item, char const* val)
{
    if (menu_item->get_active())
    {
        gtr_core_set_pref(core_, TR_KEY_statusbar_stats, val);
    }
}

void MainWindow::Impl::syncAltSpeedButton()
{
    bool const b = gtr_pref_flag_get(TR_KEY_alt_speed_enabled);
    char const* const stock = b ? "alt-speed-on" : "alt-speed-off";

    char u[32];
    tr_formatter_speed_KBps(u, gtr_pref_int_get(TR_KEY_alt_speed_up), sizeof(u));
    char d[32];
    tr_formatter_speed_KBps(d, gtr_pref_int_get(TR_KEY_alt_speed_down), sizeof(d));

    auto const str = b ? Glib::ustring::sprintf(_("Click to disable Alternative Speed Limits\n (%1$s down, %2$s up)"), d, u) :
                         Glib::ustring::sprintf(_("Click to enable Alternative Speed Limits\n (%1$s down, %2$s up)"), d, u);

    alt_speed_button_->set_active(b);
    alt_speed_image_->set(Gtk::StockID(stock), Gtk::IconSize(-1));
    alt_speed_button_->set_halign(Gtk::ALIGN_CENTER);
    alt_speed_button_->set_valign(Gtk::ALIGN_CENTER);
    alt_speed_button_->set_tooltip_text(str);
}

void MainWindow::Impl::alt_speed_toggled_cb()
{
    gtr_core_set_pref_bool(core_, TR_KEY_alt_speed_enabled, alt_speed_button_->get_active());
}

/***
****  FILTER
***/

bool MainWindow::Impl::onAskTrackerQueryTooltip(int /*x*/, int /*y*/, bool /*keyboard_tip*/, Glib::RefPtr<Gtk::Tooltip> tooltip)
{
    bool handled;
    time_t maxTime = 0;
    time_t const now = time(nullptr);

    selection_->selected_foreach(
        [&maxTime](auto const& /*path*/, auto const& iter)
        {
            auto* tor = static_cast<tr_torrent*>(iter->get_value(torrent_cols.torrent));
            auto const* torStat = tr_torrentStatCached(tor);
            maxTime = std::max(maxTime, torStat->manualAnnounceTime);
        });

    if (maxTime <= now)
    {
        handled = false;
    }
    else
    {
        char timebuf[64];
        time_t const seconds = maxTime - now;

        tr_strltime(timebuf, seconds, sizeof(timebuf));
        tooltip->set_text(Glib::ustring::sprintf(_("Tracker will allow requests in %s"), timebuf));
        handled = true;
    }

    return handled;
}

void MainWindow::Impl::onAltSpeedToggledIdle()
{
    gtr_core_set_pref_bool(core_, TR_KEY_alt_speed_enabled, tr_sessionUsesAltSpeed(gtr_core_session(core_)));
}

/***
****  Speed limit menu
***/

void MainWindow::Impl::onSpeedToggled(Gtk::CheckMenuItem* check, tr_direction dir, bool enabled)
{
    if (check->get_active())
    {
        gtr_core_set_pref_bool(core_, dir == TR_UP ? TR_KEY_speed_limit_up_enabled : TR_KEY_speed_limit_down_enabled, enabled);
    }
}

void MainWindow::Impl::onSpeedSet(tr_direction dir, int KBps)
{
    gtr_core_set_pref_int(core_, dir == TR_UP ? TR_KEY_speed_limit_up : TR_KEY_speed_limit_down, KBps);
    gtr_core_set_pref_bool(core_, dir == TR_UP ? TR_KEY_speed_limit_up_enabled : TR_KEY_speed_limit_down_enabled, true);
}

Gtk::Menu* MainWindow::Impl::createSpeedMenu(tr_direction dir)
{
    static int const speeds_KBps[] = { 5, 10, 20, 30, 40, 50, 75, 100, 150, 200, 250, 500, 750 };

    auto* m = Gtk::make_managed<Gtk::Menu>();
    Gtk::RadioButtonGroup group;

    speedlimit_off_item_[dir] = Gtk::make_managed<Gtk::RadioMenuItem>(group, _("Unlimited"));
    speedlimit_off_item_[dir]->signal_toggled().connect([this, dir]()
                                                        { onSpeedToggled(speedlimit_off_item_[dir], dir, false); });
    m->append(*speedlimit_off_item_[dir]);

    speedlimit_on_item_[dir] = Gtk::make_managed<Gtk::RadioMenuItem>(group, "");
    speedlimit_on_item_[dir]->signal_toggled().connect([this, dir]() { onSpeedToggled(speedlimit_on_item_[dir], dir, true); });
    m->append(*speedlimit_on_item_[dir]);

    m->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    for (auto const speed : speeds_KBps)
    {
        char buf[128];
        tr_formatter_speed_KBps(buf, speed, sizeof(buf));
        auto* w = Gtk::make_managed<Gtk::MenuItem>(buf);
        w->signal_activate().connect([this, dir, speed]() { onSpeedSet(dir, speed); });
        m->append(*w);
    }

    return m;
}

/***
****  Speed limit menu
***/

void MainWindow::Impl::onRatioToggled(Gtk::CheckMenuItem* check, bool enabled)
{
    if (check->get_active())
    {
        gtr_core_set_pref_bool(core_, TR_KEY_ratio_limit_enabled, enabled);
    }
}

void MainWindow::Impl::onRatioSet(double ratio)
{
    gtr_core_set_pref_double(core_, TR_KEY_ratio_limit, ratio);
    gtr_core_set_pref_bool(core_, TR_KEY_ratio_limit_enabled, true);
}

Gtk::Menu* MainWindow::Impl::createRatioMenu()
{
    static double const stockRatios[] = { 0.25, 0.5, 0.75, 1, 1.5, 2, 3 };

    auto* m = Gtk::make_managed<Gtk::Menu>();
    Gtk::RadioButtonGroup group;

    ratio_off_item_ = Gtk::make_managed<Gtk::RadioMenuItem>(group, _("Seed Forever"));
    ratio_off_item_->signal_toggled().connect([this]() { onRatioToggled(ratio_off_item_, false); });
    m->append(*ratio_off_item_);

    ratio_on_item_ = Gtk::make_managed<Gtk::RadioMenuItem>(group, "");
    ratio_on_item_->signal_toggled().connect([this]() { onRatioToggled(ratio_on_item_, true); });
    m->append(*ratio_on_item_);

    m->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    for (auto const ratio : stockRatios)
    {
        char buf[128];
        tr_strlratio(buf, ratio, sizeof(buf));
        auto* w = Gtk::make_managed<Gtk::MenuItem>(buf);
        w->signal_activate().connect([this, ratio]() { onRatioSet(ratio); });
        m->append(*w);
    }

    return m;
}

/***
****  Option menu
***/

Gtk::Menu* MainWindow::Impl::createOptionsMenu()
{
    Gtk::MenuItem* m;
    auto* top = Gtk::make_managed<Gtk::Menu>();

    m = Gtk::make_managed<Gtk::MenuItem>(_("Limit Download Speed"));
    m->set_submenu(*createSpeedMenu(TR_DOWN));
    top->append(*m);

    m = Gtk::make_managed<Gtk::MenuItem>(_("Limit Upload Speed"));
    m->set_submenu(*createSpeedMenu(TR_UP));
    top->append(*m);

    top->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    m = Gtk::make_managed<Gtk::MenuItem>(_("Stop Seeding at Ratio"));
    m->set_submenu(*createRatioMenu());
    top->append(*m);

    top->show_all();
    return top;
}

void MainWindow::Impl::onOptionsClicked(Gtk::Button* button)
{
    char buf1[512];
    char buf2[512];

    tr_formatter_speed_KBps(buf1, gtr_pref_int_get(TR_KEY_speed_limit_down), sizeof(buf1));
    gtr_label_set_text(Glib::unwrap(static_cast<Gtk::Label*>(speedlimit_on_item_[TR_DOWN]->get_child())), buf1);

    (gtr_pref_flag_get(TR_KEY_speed_limit_down_enabled) ? speedlimit_on_item_[TR_DOWN] : speedlimit_off_item_[TR_DOWN])
        ->set_active(true);

    tr_formatter_speed_KBps(buf1, gtr_pref_int_get(TR_KEY_speed_limit_up), sizeof(buf1));
    gtr_label_set_text(Glib::unwrap(static_cast<Gtk::Label*>(speedlimit_on_item_[TR_UP]->get_child())), buf1);

    (gtr_pref_flag_get(TR_KEY_speed_limit_up_enabled) ? speedlimit_on_item_[TR_UP] : speedlimit_off_item_[TR_UP])
        ->set_active(true);

    tr_strlratio(buf1, gtr_pref_double_get(TR_KEY_ratio_limit), sizeof(buf1));
    g_snprintf(buf2, sizeof(buf2), _("Stop at Ratio (%s)"), buf1);
    gtr_label_set_text(Glib::unwrap(static_cast<Gtk::Label*>(ratio_on_item_->get_child())), buf2);

    (gtr_pref_flag_get(TR_KEY_ratio_limit_enabled) ? ratio_on_item_ : ratio_off_item_)->set_active(true);

#if GTK_CHECK_VERSION(3, 22, 0)
    options_menu_->popup_at_widget(button, Gdk::GRAVITY_NORTH_WEST, Gdk::GRAVITY_SOUTH_WEST, nullptr);
#else
    options_menu_->popup(0, gtk_get_current_event_time());
#endif
}

/***
****  PUBLIC
***/

std::unique_ptr<MainWindow> MainWindow::create(Gtk::Application& app, Glib::RefPtr<Gtk::UIManager> const& uim, TrCore* core)
{
    return std::unique_ptr<MainWindow>(new MainWindow(app, uim, core));
}

MainWindow::MainWindow(Gtk::Application& app, Glib::RefPtr<Gtk::UIManager> const& ui_mgr, TrCore* core)
    : Gtk::ApplicationWindow()
    , impl_(std::make_unique<Impl>(*this, ui_mgr, core))
{
    app.add_window(*this);
}

MainWindow::~MainWindow() = default;

MainWindow::Impl::Impl(MainWindow& window, Glib::RefPtr<Gtk::UIManager> const& ui_mgr, TrCore* core)
    : core_(core)
{
    static struct
    {
        char const* val;
        char const* i18n;
    } const stats_modes[] = {
        { "total-ratio", N_("Total Ratio") },
        { "session-ratio", N_("Session Ratio") },
        { "total-transfer", N_("Total Transfer") },
        { "session-transfer", N_("Session Transfer") },
    };

    /* make the window */
    window.set_title(Glib::get_application_name());
    window.set_role("tr-main");
    window.set_default_size(gtr_pref_int_get(TR_KEY_main_window_width), gtr_pref_int_get(TR_KEY_main_window_height));
    window.move(gtr_pref_int_get(TR_KEY_main_window_x), gtr_pref_int_get(TR_KEY_main_window_y));

    if (gtr_pref_flag_get(TR_KEY_main_window_is_maximized))
    {
        window.maximize();
    }

    window.add_accel_group(ui_mgr->get_accel_group());
    /* Add style provider to the window. */
    /* Please move it to separate .css file if you’re adding more styles here. */
    auto const* style = ".tr-workarea.frame {border-left-width: 0; border-right-width: 0; border-radius: 0;}";
    auto css_provider = Gtk::CssProvider::create();
    css_provider->load_from_data(style);
    Gtk::StyleContext::add_provider_for_screen(
        Gdk::Screen::get_default(),
        css_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* window's main container */
    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    window.add(*vbox);

    /* main menu */
    auto* mainmenu = Glib::wrap(GTK_MENU_BAR(gtr_action_get_widget("/main-window-menu")));
    Glib::wrap(GTK_MENU_ITEM(gtr_action_get_widget("/main-window-menu/torrent-menu/torrent-reannounce")))
        ->signal_query_tooltip()
        .connect(sigc::mem_fun(this, &Impl::onAskTrackerQueryTooltip));

    /* toolbar */
    toolbar_ = Glib::wrap(GTK_TOOLBAR(gtr_action_get_widget("/main-window-toolbar")));
    toolbar_->get_style_context()->add_class(GTK_STYLE_CLASS_PRIMARY_TOOLBAR);
    gtr_action_set_important("open-torrent-toolbar", true);
    gtr_action_set_important("show-torrent-properties", true);

    /* filter */
    filter_ = Gtk::make_managed<FilterBar>(gtr_core_session(core_), Glib::wrap(gtr_core_model(core_), true));
    filter_->set_border_width(GUI_PAD_SMALL);

    /* status menu */
    status_menu_ = Gtk::make_managed<Gtk::Menu>();
    Gtk::RadioButtonGroup stats_modes_group;
    char const* pch = gtr_pref_string_get(TR_KEY_statusbar_stats);

    for (auto const& mode : stats_modes)
    {
        auto* w = Gtk::make_managed<Gtk::RadioMenuItem>(stats_modes_group, _(mode.i18n));
        w->set_active(g_strcmp0(mode.val, pch) == 0);
        w->signal_toggled().connect([this, w, val = mode.val]() { status_menu_toggled_cb(w, val); });
        status_menu_->append(*w);
        w->show();
    }

    /**
    *** Statusbar
    **/

    status_ = Gtk::make_managed<Gtk::Grid>();
    status_->set_orientation(Gtk::ORIENTATION_HORIZONTAL);
    status_->set_border_width(GUI_PAD_SMALL);

    /* gear */
    auto* gear_button = Gtk::make_managed<Gtk::Button>();
    gear_button->add(*Gtk::make_managed<Gtk::Image>("utilities", Gtk::ICON_SIZE_MENU));
    gear_button->set_tooltip_text(_("Options"));
    gear_button->set_relief(Gtk::RELIEF_NONE);
    options_menu_ = createOptionsMenu();
    gear_button->signal_clicked().connect([this, gear_button]() { onOptionsClicked(gear_button); });
    status_->add(*gear_button);

    /* turtle */
    alt_speed_image_ = Gtk::make_managed<Gtk::Image>();
    alt_speed_button_ = Gtk::make_managed<Gtk::ToggleButton>();
    alt_speed_button_->set_image(*alt_speed_image_);
    alt_speed_button_->set_relief(Gtk::RELIEF_NONE);
    alt_speed_button_->signal_toggled().connect(sigc::mem_fun(this, &Impl::alt_speed_toggled_cb));
    status_->add(*alt_speed_button_);

    /* spacer */
    auto* w = Gtk::make_managed<Gtk::Fixed>();
    w->set_hexpand(true);
    status_->add(*w);

    /* download */
    dl_lb_ = Gtk::make_managed<Gtk::Label>();
    dl_lb_->set_single_line_mode(true);
    status_->add(*dl_lb_);

    /* upload */
    ul_lb_ = Gtk::make_managed<Gtk::Label>();
    ul_lb_->set_margin_left(GUI_PAD);
    ul_lb_->set_single_line_mode(true);
    status_->add(*ul_lb_);

    /* ratio */
    stats_lb_ = Gtk::make_managed<Gtk::Label>();
    stats_lb_->set_margin_left(GUI_PAD_BIG);
    stats_lb_->set_single_line_mode(true);
    status_->add(*stats_lb_);

    /* ratio selector */
    auto* ratio_button = Gtk::make_managed<Gtk::Button>();
    ratio_button->set_tooltip_text(_("Statistics"));
    ratio_button->add(*Gtk::make_managed<Gtk::Image>("ratio", Gtk::ICON_SIZE_MENU));
    ratio_button->set_relief(Gtk::RELIEF_NONE);
    ratio_button->signal_clicked().connect([this, ratio_button]() { onYinYangClicked(ratio_button); });
    status_->add(*ratio_button);

    /**
    *** Workarea
    **/

    view_ = makeview(filter_->get_filter_model());
    scroll_ = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll_->set_shadow_type(Gtk::SHADOW_OUT);
    scroll_->get_style_context()->add_class("tr-workarea");
    scroll_->add(*view_);

    /* lay out the widgets */
    vbox->pack_start(*mainmenu, false, false);
    vbox->pack_start(*toolbar_, false, false);
    vbox->pack_start(*filter_, false, false);
    vbox->pack_start(*scroll_, true, true);
    vbox->pack_start(*status_, false, false);

    {
        /* this is to determine the maximum width/height for the label */
        int width = 0;
        int height = 0;
        auto const pango_layout = ul_lb_->create_pango_layout("999.99 kB/s");
        pango_layout->get_pixel_size(width, height);
        ul_lb_->set_size_request(width, height);
        dl_lb_->set_size_request(width, height);
        ul_lb_->set_halign(Gtk::ALIGN_END);
        ul_lb_->set_valign(Gtk::ALIGN_CENTER);
        dl_lb_->set_halign(Gtk::ALIGN_END);
        dl_lb_->set_valign(Gtk::ALIGN_CENTER);
    }

    /* show all but the window */
    vbox->show_all();

    /* listen for prefs changes that affect the window */
    prefsChanged(TR_KEY_compact_view);
    prefsChanged(TR_KEY_show_filterbar);
    prefsChanged(TR_KEY_show_statusbar);
    prefsChanged(TR_KEY_statusbar_stats);
    prefsChanged(TR_KEY_show_toolbar);
    prefsChanged(TR_KEY_alt_speed_enabled);
    pref_handler_id_ = g_signal_connect(
        core_,
        "prefs-changed",
        G_CALLBACK(static_cast<void (*)(TrCore*, tr_quark, MainWindow::Impl*)>(
            [](TrCore* /*core*/, tr_quark key, MainWindow::Impl* impl) { impl->prefsChanged(key); })),
        this);

    tr_sessionSetAltSpeedFunc(
        gtr_core_session(core_),
        [](tr_session* /*s*/, bool /*isEnabled*/, bool /*byUser*/, void* p)
        { Glib::signal_idle().connect_once([p]() { static_cast<Impl*>(p)->onAltSpeedToggledIdle(); }); },
        this);

    refresh();
}

void MainWindow::Impl::updateStats()
{
    char up[32];
    char down[32];
    char ratio[32];
    Glib::ustring buf;
    tr_session_stats stats;
    auto const* const session = gtr_core_session(core_);

    /* update the stats */
    char const* pch = gtr_pref_string_get(TR_KEY_statusbar_stats);

    if (g_strcmp0(pch, "session-ratio") == 0)
    {
        tr_sessionGetStats(session, &stats);
        tr_strlratio(ratio, stats.ratio, sizeof(ratio));
        buf = Glib::ustring::sprintf(_("Ratio: %s"), ratio);
    }
    else if (g_strcmp0(pch, "session-transfer") == 0)
    {
        tr_sessionGetStats(session, &stats);
        tr_strlsize(up, stats.uploadedBytes, sizeof(up));
        tr_strlsize(down, stats.downloadedBytes, sizeof(down));
        /* Translators: "size|" is here for disambiguation. Please remove it from your translation.
           %1$s is the size of the data we've downloaded
           %2$s is the size of the data we've uploaded */
        buf = Glib::ustring::sprintf(Q_("Down: %1$s, Up: %2$s"), down, up);
    }
    else if (g_strcmp0(pch, "total-transfer") == 0)
    {
        tr_sessionGetCumulativeStats(session, &stats);
        tr_strlsize(up, stats.uploadedBytes, sizeof(up));
        tr_strlsize(down, stats.downloadedBytes, sizeof(down));
        /* Translators: "size|" is here for disambiguation. Please remove it from your translation.
           %1$s is the size of the data we've downloaded
           %2$s is the size of the data we've uploaded */
        buf = Glib::ustring::sprintf(Q_("size|Down: %1$s, Up: %2$s"), down, up);
    }
    else /* default is total-ratio */
    {
        tr_sessionGetCumulativeStats(session, &stats);
        tr_strlratio(ratio, stats.ratio, sizeof(ratio));
        buf = Glib::ustring::sprintf(_("Ratio: %s"), ratio);
    }

    stats_lb_->set_text(buf);
}

void MainWindow::Impl::updateSpeeds()
{
    auto const* const session = gtr_core_session(core_);

    if (session != nullptr)
    {
        char speed_str[128];
        double upSpeed = 0;
        double downSpeed = 0;
        int upCount = 0;
        int downCount = 0;
        auto const model = Glib::wrap(gtr_core_model(core_), true);

        for (auto const& row : model->children())
        {
            upSpeed += row.get_value(torrent_cols.speed_up);
            upCount += row.get_value(torrent_cols.active_peers_up);
            downSpeed += row.get_value(torrent_cols.speed_down);
            downCount += row.get_value(torrent_cols.active_peers_down);
        }

        tr_formatter_speed_KBps(speed_str, downSpeed, sizeof(speed_str));
        dl_lb_->set_text(Glib::ustring::sprintf("%s %s", speed_str, gtr_get_unicode_string(GTR_UNICODE_DOWN)));
        dl_lb_->set_visible(downCount > 0);

        tr_formatter_speed_KBps(speed_str, upSpeed, sizeof(speed_str));
        ul_lb_->set_text(Glib::ustring::sprintf("%s %s", speed_str, gtr_get_unicode_string(GTR_UNICODE_UP)));
        ul_lb_->set_visible(downCount > 0 || upCount > 0);
    }
}

void MainWindow::refresh()
{
    impl_->refresh();
}

void MainWindow::Impl::refresh()
{
    if (core_ != nullptr && gtr_core_session(core_) != nullptr)
    {
        updateSpeeds();
        updateStats();
    }
}

Glib::RefPtr<Gtk::TreeSelection> MainWindow::get_selection() const
{
    return impl_->get_selection();
}

Glib::RefPtr<Gtk::TreeSelection> MainWindow::Impl::get_selection() const
{
    return selection_;
}

void MainWindow::set_busy(bool isBusy)
{
    if (get_realized())
    {
        auto const display = get_display();
        auto const cursor = isBusy ? Gdk::Cursor::create(display, Gdk::WATCH) : Glib::RefPtr<Gdk::Cursor>();

        get_window()->set_cursor(cursor);
        display->flush();
    }
}
