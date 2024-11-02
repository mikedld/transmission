// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include "MainWindow.h"

#include "Actions.h"
#include "FilterBar.h"
#include "GtkCompat.h"
#include "ListModelAdapter.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Torrent.h"
#include "Utils.h"

#if !GTKMM_CHECK_VERSION(4, 0, 0)
#include "TorrentCellRenderer.h"
#endif

#include <libtransmission/transmission.h>
#include <libtransmission/values.h>

#include <gdkmm/cursor.h>
#include <gdkmm/rectangle.h>
#include <giomm/menu.h>
#include <giomm/menuitem.h>
#include <giomm/menumodel.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/ustring.h>
#include <glibmm/variant.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/treemodel.h>
#include <gtkmm/treeview.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gtkmm/listitemfactory.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/popovermenu.h>
#else
#include <gdkmm/display.h>
#include <gdkmm/window.h>
#include <gtkmm/menu.h>
#include <gtkmm/treeselection.h>
#include <gtkmm/treeviewcolumn.h>
#endif

#include <array>
#include <memory>
#include <string>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace libtransmission::Values;

using VariantInt = Glib::Variant<int>;
using VariantDouble = Glib::Variant<double>;
using VariantString = Glib::Variant<Glib::ustring>;

namespace
{

auto constexpr OptionsMenuActionGroupName = "options-menu"sv;
auto constexpr StatsMenuActionGroupName = "stats-menu"sv;

} // namespace

class MainWindow::Impl
{
    struct OptionMenuInfo
    {
        Glib::RefPtr<Gio::SimpleAction> action;
        Glib::RefPtr<Gio::MenuItem> on_item;
        Glib::RefPtr<Gio::Menu> section;
    };

    using TorrentView = IF_GTKMM4(Gtk::ListView, Gtk::TreeView);
    using TorrentViewSelection = IF_GTKMM4(Gtk::MultiSelection, Gtk::TreeSelection);

public:
    Impl(
        MainWindow& window,
        Glib::RefPtr<Gtk::Builder> const& builder,
        Glib::RefPtr<Gio::ActionGroup> const& actions,
        Glib::RefPtr<Session> const& core);
    ~Impl();

    TR_DISABLE_COPY_MOVE(Impl)

    [[nodiscard]] Glib::RefPtr<TorrentViewSelection> get_selection() const;

    void refresh();

    void prefsChanged(tr_quark key);

    auto& signal_selection_changed()
    {
        return signal_selection_changed_;
    }

private:
    void init_view(TorrentView* view, Glib::RefPtr<FilterBar::Model> const& model);

    Glib::RefPtr<Gio::MenuModel> createOptionsMenu();
    Glib::RefPtr<Gio::MenuModel> createSpeedMenu(Glib::RefPtr<Gio::SimpleActionGroup> const& actions, tr_direction dir);
    Glib::RefPtr<Gio::MenuModel> createRatioMenu(Glib::RefPtr<Gio::SimpleActionGroup> const& actions);

    Glib::RefPtr<Gio::MenuModel> createStatsMenu();

    void on_popup_menu(double event_x, double event_y);

    void onSpeedToggled(std::string const& action_name, tr_direction dir, bool enabled);
    void onSpeedSet(tr_direction dir, int KBps);

    void onRatioToggled(std::string const& action_name, bool enabled);
    void onRatioSet(double ratio);

    void updateStats();
    void updateSpeeds();

    void syncAltSpeedButton();

    void status_menu_toggled_cb(std::string const& action_name, Glib::ustring const& val);
    void onOptionsClicked();
    void alt_speed_toggled_cb();
    void onAltSpeedToggledIdle();

private:
    MainWindow& window_;
    Glib::RefPtr<Session> const core_;

    sigc::signal<void()> signal_selection_changed_;

    Glib::RefPtr<Gio::ActionGroup> options_actions_;
    Glib::RefPtr<Gio::ActionGroup> stats_actions_;

    std::array<OptionMenuInfo, 2> speed_menu_info_;
    OptionMenuInfo ratio_menu_info_;

#if GTKMM_CHECK_VERSION(4, 0, 0)
    Glib::RefPtr<Gtk::ListItemFactory> item_factory_compact_;
    Glib::RefPtr<Gtk::ListItemFactory> item_factory_full_;
    Glib::RefPtr<Gtk::MultiSelection> selection_;
#else
    TorrentCellRenderer* renderer_ = nullptr;
    Gtk::TreeViewColumn* column_ = nullptr;
#endif

    Gtk::ScrolledWindow* scroll_ = nullptr;
    TorrentView* view_ = nullptr;
    Gtk::Widget* toolbar_ = nullptr;
    FilterBar* filter_;
    Gtk::Widget* status_ = nullptr;
    Gtk::Label* ul_lb_ = nullptr;
    Gtk::Label* dl_lb_ = nullptr;
    Gtk::Label* stats_lb_ = nullptr;
    Gtk::Image* alt_speed_image_ = nullptr;
    Gtk::ToggleButton* alt_speed_button_ = nullptr;
    sigc::connection pref_handler_id_;
    IF_GTKMM4(Gtk::PopoverMenu*, Gtk::Menu*) popup_menu_ = nullptr;
};

/***
****
***/

void MainWindow::Impl::on_popup_menu([[maybe_unused]] double event_x, [[maybe_unused]] double event_y)
{
    if (popup_menu_ == nullptr)
    {
        auto const menu = gtr_action_get_object<Gio::Menu>("main-window-popup");

#if GTKMM_CHECK_VERSION(4, 0, 0)
        popup_menu_ = Gtk::make_managed<Gtk::PopoverMenu>(menu, Gtk::PopoverMenu::Flags::NESTED);
        popup_menu_->set_parent(*view_);
        popup_menu_->set_has_arrow(false);
        popup_menu_->set_halign(view_->get_direction() == Gtk::TextDirection::RTL ? Gtk::Align::END : Gtk::Align::START);

        view_->signal_destroy().connect(
            [this]()
            {
                popup_menu_->unparent();
                popup_menu_ = nullptr;
            });
#else
        popup_menu_ = Gtk::make_managed<Gtk::Menu>(menu);
        popup_menu_->attach_to_widget(window_);
#endif
    }

#if GTKMM_CHECK_VERSION(4, 0, 0)
    popup_menu_->set_pointing_to({ static_cast<int>(event_x), static_cast<int>(event_y), 1, 1 });
    popup_menu_->popup();
#else
    popup_menu_->popup_at_pointer(nullptr);
#endif
}

namespace
{

#if GTKMM_CHECK_VERSION(4, 0, 0)

class GtrStrvBuilderDeleter
{
public:
    void operator()(GStrvBuilder* builder) const
    {
        if (builder != nullptr)
        {
            g_strv_builder_unref(builder);
        }
    }
};

using GtrStrvBuilderPtr = std::unique_ptr<GStrvBuilder, GtrStrvBuilderDeleter>;

GStrv gtr_strv_join(GObject* /*object*/, GStrv lhs, GStrv rhs)
{
    auto const builder = GtrStrvBuilderPtr(g_strv_builder_new());
    if (builder == nullptr)
    {
        return nullptr;
    }

    g_strv_builder_addv(builder.get(), const_cast<char const**>(lhs)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    g_strv_builder_addv(builder.get(), const_cast<char const**>(rhs)); // NOLINT(cppcoreguidelines-pro-type-const-cast)

    return g_strv_builder_end(builder.get());
}

#else

bool tree_view_search_equal_func(
    Glib::RefPtr<Gtk::TreeModel> const& /*model*/,
    int /*column*/,
    Glib::ustring const& key,
    Gtk::TreeModel::const_iterator const& iter)
{
    static auto const& self_col = Torrent::get_columns().self;

    auto const name = iter->get_value(self_col)->get_name_collated();
    return name.find(key.lowercase()) == Glib::ustring::npos;
}

#endif

} // namespace

void MainWindow::Impl::init_view(TorrentView* view, Glib::RefPtr<FilterBar::Model> const& model)
{
#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto const create_builder_list_item_factory = [](std::string const& filename)
    {
        auto builder_scope = Glib::wrap(G_OBJECT(gtk_builder_cscope_new()));
        gtk_builder_cscope_add_callback(GTK_BUILDER_CSCOPE(builder_scope->gobj()), gtr_strv_join);

        return Glib::wrap(gtk_builder_list_item_factory_new_from_resource(
            GTK_BUILDER_SCOPE(builder_scope->gobj()),
            gtr_get_full_resource_path(filename).c_str()));
    };

    item_factory_compact_ = create_builder_list_item_factory("TorrentListItemCompact.ui"s);
    item_factory_full_ = create_builder_list_item_factory("TorrentListItemFull.ui"s);

    view->signal_activate().connect([](guint /*position*/) { gtr_action_activate("show-torrent-properties"); });

    selection_ = Gtk::MultiSelection::create(model);
    selection_->signal_selection_changed().connect([this](guint /*position*/, guint /*n_items*/)
                                                   { signal_selection_changed_.emit(); });

    view->set_factory(gtr_pref_flag_get(TR_KEY_compact_view) ? item_factory_compact_ : item_factory_full_);
    view->set_model(selection_);
#else
    static auto const& torrent_cols = Torrent::get_columns();

    view->set_search_column(torrent_cols.name_collated);
    view->set_search_equal_func(&tree_view_search_equal_func);

    column_ = view->get_column(0);

    renderer_ = Gtk::make_managed<TorrentCellRenderer>();
    column_->pack_start(*renderer_, false);
    column_->add_attribute(renderer_->property_torrent(), torrent_cols.self);

    view->signal_popup_menu().connect_notify([this]() { on_popup_menu(0, 0); });
    view->signal_row_activated().connect([](auto const& /*path*/, auto* /*column*/)
                                         { gtr_action_activate("show-torrent-properties"); });

    view->set_model(model);

    view->get_selection()->signal_changed().connect([this]() { signal_selection_changed_.emit(); });
#endif

    setup_item_view_button_event_handling(
        *view,
        [this, view](guint /*button*/, TrGdkModifierType /*state*/, double view_x, double view_y, bool context_menu_requested)
        {
            return on_item_view_button_pressed(
                *view,
                view_x,
                view_y,
                context_menu_requested,
                sigc::mem_fun(*this, &Impl::on_popup_menu));
        },
        [view](double view_x, double view_y) { return on_item_view_button_released(*view, view_x, view_y); });
}

void MainWindow::Impl::prefsChanged(tr_quark const key)
{
    switch (key)
    {
    case TR_KEY_compact_view:
#if GTKMM_CHECK_VERSION(4, 0, 0)
        view_->set_factory(gtr_pref_flag_get(key) ? item_factory_compact_ : item_factory_full_);
#else
        renderer_->property_compact() = gtr_pref_flag_get(key);
        /* since the cell size has changed, we need gtktreeview to revalidate
         * its fixed-height mode values. Unfortunately there's not an API call
         * for that, but this seems to work */
        view_->set_fixed_height_mode(false);
        view_->set_row_separator_func({});
        view_->unset_row_separator_func();
        view_->set_fixed_height_mode(true);
#endif
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
    pref_handler_id_.disconnect();
}

void MainWindow::Impl::status_menu_toggled_cb(std::string const& action_name, Glib::ustring const& val)
{
    stats_actions_->change_action_state(action_name, VariantString::create(val));
    core_->set_pref(TR_KEY_statusbar_stats, val.raw());
}

void MainWindow::Impl::syncAltSpeedButton()
{
    bool const b = gtr_pref_flag_get(TR_KEY_alt_speed_enabled);
    alt_speed_button_->set_active(b);
    alt_speed_button_->set_tooltip_text(fmt::format(
        b ? _("Click to disable Alternative Speed Limits\n ({download_speed} down, {upload_speed} up)") :
            _("Click to enable Alternative Speed Limits\n ({download_speed} down, {upload_speed} up)"),
        fmt::arg("download_speed", Speed{ gtr_pref_int_get(TR_KEY_alt_speed_down), Speed::Units::KByps }.to_string()),
        fmt::arg("upload_speed", Speed{ gtr_pref_int_get(TR_KEY_alt_speed_up), Speed::Units::KByps }.to_string())));
}

void MainWindow::Impl::alt_speed_toggled_cb()
{
    core_->set_pref(TR_KEY_alt_speed_enabled, alt_speed_button_->get_active());
}

/***
****  FILTER
***/

void MainWindow::Impl::onAltSpeedToggledIdle()
{
    core_->set_pref(TR_KEY_alt_speed_enabled, tr_sessionUsesAltSpeed(core_->get_session()));
}

/***
****  Speed limit menu
***/

void MainWindow::Impl::onSpeedToggled(std::string const& action_name, tr_direction dir, bool enabled)
{
    options_actions_->change_action_state(action_name, VariantInt::create(enabled ? 1 : 0));
    core_->set_pref(dir == TR_UP ? TR_KEY_speed_limit_up_enabled : TR_KEY_speed_limit_down_enabled, enabled);
}

void MainWindow::Impl::onSpeedSet(tr_direction dir, int KBps)
{
    core_->set_pref(dir == TR_UP ? TR_KEY_speed_limit_up : TR_KEY_speed_limit_down, KBps);
    core_->set_pref(dir == TR_UP ? TR_KEY_speed_limit_up_enabled : TR_KEY_speed_limit_down_enabled, true);
}

Glib::RefPtr<Gio::MenuModel> MainWindow::Impl::createSpeedMenu(
    Glib::RefPtr<Gio::SimpleActionGroup> const& actions,
    tr_direction dir)
{
    auto& info = speed_menu_info_.at(dir);

    auto m = Gio::Menu::create();

    auto const action_name = fmt::format("speed-limit-{}", dir == TR_UP ? "up" : "down");
    auto const full_action_name = fmt::format("{}.{}", OptionsMenuActionGroupName, action_name);
    info.action = actions->add_action_radio_integer(
        action_name,
        [this, action_name, dir](int value) { onSpeedToggled(action_name, dir, value != 0); },
        0);

    info.section = Gio::Menu::create();

    auto speedlimit_off_item = Gio::MenuItem::create(_("Unlimited"), full_action_name);
    speedlimit_off_item->set_action_and_target(full_action_name, VariantInt::create(0));
    info.section->append_item(speedlimit_off_item);

    info.on_item = Gio::MenuItem::create("...", full_action_name);
    info.on_item->set_action_and_target(full_action_name, VariantInt::create(1));
    info.section->append_item(info.on_item);

    m->append_section(info.section);
    auto section = Gio::Menu::create();

    auto const stock_action_name = fmt::format("{}-stock", action_name);
    auto const full_stock_action_name = fmt::format("{}.{}", OptionsMenuActionGroupName, stock_action_name);
    actions->add_action_with_parameter(
        stock_action_name,
        VariantInt::variant_type(),
        [this, dir](Glib::VariantBase const& value)
        { onSpeedSet(dir, Glib::VariantBase::cast_dynamic<VariantInt>(value).get()); });

    for (auto const KBps : { 50, 100, 250, 500, 1000, 2500, 5000, 10000 })
    {
        auto item = Gio::MenuItem::create(Speed{ KBps, Speed::Units::KByps }.to_string(), full_stock_action_name);
        item->set_action_and_target(full_stock_action_name, VariantInt::create(KBps));
        section->append_item(item);
    }

    m->append_section(section);
    return m;
}

/***
****  Speed limit menu
***/

void MainWindow::Impl::onRatioToggled(std::string const& action_name, bool enabled)
{
    options_actions_->change_action_state(action_name, VariantInt::create(enabled ? 1 : 0));
    core_->set_pref(TR_KEY_ratio_limit_enabled, enabled);
}

void MainWindow::Impl::onRatioSet(double ratio)
{
    core_->set_pref(TR_KEY_ratio_limit, ratio);
    core_->set_pref(TR_KEY_ratio_limit_enabled, true);
}

Glib::RefPtr<Gio::MenuModel> MainWindow::Impl::createRatioMenu(Glib::RefPtr<Gio::SimpleActionGroup> const& actions)
{
    static auto const stockRatios = std::array<double, 7>({ 0.25, 0.5, 0.75, 1, 1.5, 2, 3 });

    auto& info = ratio_menu_info_;

    auto m = Gio::Menu::create();

    auto const action_name = "ratio-limit"s;
    auto const full_action_name = fmt::format("{}.{}", OptionsMenuActionGroupName, action_name);
    info.action = actions->add_action_radio_integer(
        action_name,
        [this, action_name](int value) { onRatioToggled(action_name, value != 0); },
        0);

    info.section = Gio::Menu::create();

    auto ratio_off_item = Gio::MenuItem::create(_("Seed Forever"), full_action_name);
    ratio_off_item->set_action_and_target(full_action_name, VariantInt::create(0));
    info.section->append_item(ratio_off_item);

    info.on_item = Gio::MenuItem::create("...", full_action_name);
    info.on_item->set_action_and_target(full_action_name, VariantInt::create(1));
    info.section->append_item(info.on_item);

    m->append_section(info.section);
    auto section = Gio::Menu::create();

    auto const stock_action_name = fmt::format("{}-stock", action_name);
    auto const full_stock_action_name = fmt::format("{}.{}", OptionsMenuActionGroupName, stock_action_name);
    actions->add_action_with_parameter(
        stock_action_name,
        VariantDouble::variant_type(),
        [this](Glib::VariantBase const& value) { onRatioSet(Glib::VariantBase::cast_dynamic<VariantDouble>(value).get()); });

    for (auto const ratio : stockRatios)
    {
        auto item = Gio::MenuItem::create(tr_strlratio(ratio), full_stock_action_name);
        item->set_action_and_target(full_stock_action_name, VariantDouble::create(ratio));
        section->append_item(item);
    }

    m->append_section(section);
    return m;
}

/***
****  Option menu
***/

Glib::RefPtr<Gio::MenuModel> MainWindow::Impl::createOptionsMenu()
{
    auto top = Gio::Menu::create();
    auto actions = Gio::SimpleActionGroup::create();

    auto section = Gio::Menu::create();
    section->append_submenu(_("Limit Download Speed"), createSpeedMenu(actions, TR_DOWN));
    section->append_submenu(_("Limit Upload Speed"), createSpeedMenu(actions, TR_UP));
    top->append_section(section);

    section = Gio::Menu::create();
    section->append_submenu(_("Stop Seeding at Ratio"), createRatioMenu(actions));
    top->append_section(section);

    window_.insert_action_group(std::string(OptionsMenuActionGroupName), actions);
    options_actions_ = actions;

    return top;
}

void MainWindow::Impl::onOptionsClicked()
{
    static auto const update_menu = [](OptionMenuInfo& info, Glib::ustring const& new_on_label, tr_quark on_off_key)
    {
        if (auto on_label = Glib::VariantBase::cast_dynamic<VariantString>(info.on_item->get_attribute_value("label")).get();
            on_label != new_on_label)
        {
            info.on_item->set_label(new_on_label);

            // Items aren't refed by menu on insert but their attributes copied instead, so need to replace.
            // (see https://docs.gtk.org/gio/method.Menu.insert_item.html)
            info.section->remove(info.section->get_n_items() - 1);
            info.section->append_item(info.on_item);
        }

        info.action->change_state(gtr_pref_flag_get(on_off_key) ? 1 : 0);
    };

    update_menu(
        speed_menu_info_[TR_DOWN],
        Speed{ gtr_pref_int_get(TR_KEY_speed_limit_down), Speed::Units::KByps }.to_string(),
        TR_KEY_speed_limit_down_enabled);

    update_menu(
        speed_menu_info_[TR_UP],
        Speed{ gtr_pref_int_get(TR_KEY_speed_limit_up), Speed::Units::KByps }.to_string(),
        TR_KEY_speed_limit_up_enabled);

    update_menu(
        ratio_menu_info_,
        fmt::format(_("Stop at Ratio ({ratio})"), fmt::arg("ratio", tr_strlratio(gtr_pref_double_get(TR_KEY_ratio_limit)))),
        TR_KEY_ratio_limit_enabled);
}

Glib::RefPtr<Gio::MenuModel> MainWindow::Impl::createStatsMenu()
{
    struct StatsModeInfo
    {
        char const* val;
        char const* i18n;
    };

    static auto const stats_modes = std::array<StatsModeInfo, 4>({ {
        { "total-ratio", N_("Total Ratio") },
        { "session-ratio", N_("Session Ratio") },
        { "total-transfer", N_("Total Transfer") },
        { "session-transfer", N_("Session Transfer") },
    } });

    auto top = Gio::Menu::create();
    auto actions = Gio::SimpleActionGroup::create();

    auto const action_name = "stats-mode"s;
    auto const full_action_name = fmt::format("{}.{}", StatsMenuActionGroupName, action_name);
    auto stats_mode_action = actions->add_action_radio_string(
        action_name,
        [this, action_name](Glib::ustring const& value) { status_menu_toggled_cb(action_name, value); },
        gtr_pref_string_get(TR_KEY_statusbar_stats));

    for (auto const& mode : stats_modes)
    {
        auto item = Gio::MenuItem::create(_(mode.i18n), full_action_name);
        item->set_action_and_target(full_action_name, VariantString::create(mode.val));
        top->append_item(item);
    }

    window_.insert_action_group(std::string(StatsMenuActionGroupName), actions);
    stats_actions_ = actions;

    return top;
}

/***
****  PUBLIC
***/

std::unique_ptr<MainWindow> MainWindow::create(
    Gtk::Application& app,
    Glib::RefPtr<Gio::ActionGroup> const& actions,
    Glib::RefPtr<Session> const& core)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("MainWindow.ui"));
    return std::unique_ptr<MainWindow>(gtr_get_widget_derived<MainWindow>(builder, "MainWindow", app, actions, core));
}

MainWindow::MainWindow(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Gtk::Application& app,
    Glib::RefPtr<Gio::ActionGroup> const& actions,
    Glib::RefPtr<Session> const& core)
    : Gtk::ApplicationWindow(cast_item)
    , impl_(std::make_unique<Impl>(*this, builder, actions, core))
{
    app.add_window(*this);
}

MainWindow::~MainWindow() = default;

MainWindow::Impl::Impl(
    MainWindow& window,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Gio::ActionGroup> const& actions,
    Glib::RefPtr<Session> const& core)
    : window_(window)
    , core_(core)
    , scroll_(gtr_get_widget<Gtk::ScrolledWindow>(builder, "torrents_view_scroll"))
    , view_(gtr_get_widget<TorrentView>(builder, "torrents_view"))
    , toolbar_(gtr_get_widget<Gtk::Widget>(builder, "toolbar"))
    , filter_(gtr_get_widget_derived<FilterBar>(builder, "filterbar", core_))
    , status_(gtr_get_widget<Gtk::Widget>(builder, "statusbar"))
    , ul_lb_(gtr_get_widget<Gtk::Label>(builder, "upload_speed_label"))
    , dl_lb_(gtr_get_widget<Gtk::Label>(builder, "download_speed_label"))
    , stats_lb_(gtr_get_widget<Gtk::Label>(builder, "statistics_label"))
    , alt_speed_image_(gtr_get_widget<Gtk::Image>(builder, "alt_speed_button_image"))
    , alt_speed_button_(gtr_get_widget<Gtk::ToggleButton>(builder, "alt_speed_button"))
{
    /* make the window */
    window.set_title(Glib::get_application_name());
    window.set_default_size(gtr_pref_int_get(TR_KEY_main_window_width), gtr_pref_int_get(TR_KEY_main_window_height));
#if !GTKMM_CHECK_VERSION(4, 0, 0)
    window.move(gtr_pref_int_get(TR_KEY_main_window_x), gtr_pref_int_get(TR_KEY_main_window_y));
#endif

    if (gtr_pref_flag_get(TR_KEY_main_window_is_maximized))
    {
        window.maximize();
    }

    window.insert_action_group("win", actions);

    /**
    *** Statusbar
    **/

    /* gear */
    auto* gear_button = gtr_get_widget<Gtk::MenuButton>(builder, "gear_button");
    gear_button->set_menu_model(createOptionsMenu());
#if GTKMM_CHECK_VERSION(4, 0, 0)
    for (auto* child = gear_button->get_first_child(); child != nullptr; child = child->get_next_sibling())
    {
        if (auto* popover = dynamic_cast<Gtk::Popover*>(child); popover != nullptr)
        {
            popover->signal_show().connect([this]() { onOptionsClicked(); }, false);
            break;
        }
    }
#else
    gear_button->signal_clicked().connect([this]() { onOptionsClicked(); }, false);
#endif

    /* turtle */
    alt_speed_button_->signal_toggled().connect(sigc::mem_fun(*this, &Impl::alt_speed_toggled_cb));

    /* ratio selector */
    auto* ratio_button = gtr_get_widget<Gtk::MenuButton>(builder, "ratio_button");
    ratio_button->set_menu_model(createStatsMenu());

    /**
    *** Workarea
    **/

    init_view(view_, filter_->get_filter_model());

    {
        /* this is to determine the maximum width/height for the label */
        int width = 0;
        int height = 0;
        auto const pango_layout = ul_lb_->create_pango_layout("999.99 kB/s");
        pango_layout->get_pixel_size(width, height);
        ul_lb_->set_size_request(width, height);
        dl_lb_->set_size_request(width, height);
    }

    /* listen for prefs changes that affect the window */
    prefsChanged(TR_KEY_compact_view);
    prefsChanged(TR_KEY_show_filterbar);
    prefsChanged(TR_KEY_show_statusbar);
    prefsChanged(TR_KEY_statusbar_stats);
    prefsChanged(TR_KEY_show_toolbar);
    prefsChanged(TR_KEY_alt_speed_enabled);
    pref_handler_id_ = core_->signal_prefs_changed().connect(sigc::mem_fun(*this, &Impl::prefsChanged));

    tr_sessionSetAltSpeedFunc(
        core_->get_session(),
        [](tr_session* /*s*/, bool /*isEnabled*/, bool /*byUser*/, gpointer p)
        { Glib::signal_idle().connect_once([p]() { static_cast<Impl*>(p)->onAltSpeedToggledIdle(); }); },
        this);

    refresh();

#if !GTKMM_CHECK_VERSION(4, 0, 0)
    /* prevent keyboard events being sent to the window first */
    window.signal_key_press_event().connect(
        [this](GdkEventKey* event) { return gtk_window_propagate_key_event(static_cast<Gtk::Window&>(window_).gobj(), event); },
        false);
    window.signal_key_release_event().connect(
        [this](GdkEventKey* event) { return gtk_window_propagate_key_event(static_cast<Gtk::Window&>(window_).gobj(), event); },
        false);
#endif
}

void MainWindow::Impl::updateStats()
{
    Glib::ustring buf;
    auto const* const session = core_->get_session();

    /* update the stats */
    if (auto const pch = gtr_pref_string_get(TR_KEY_statusbar_stats); pch == "session-ratio")
    {
        auto const stats = tr_sessionGetStats(session);
        buf = fmt::format(_("Ratio: {ratio}"), fmt::arg("ratio", tr_strlratio(stats.ratio)));
    }
    else if (pch == "session-transfer")
    {
        auto const stats = tr_sessionGetStats(session);
        buf = fmt::format(
            C_("current session totals", "Down: {downloaded_size}, Up: {uploaded_size}"),
            fmt::arg("downloaded_size", tr_strlsize(stats.downloadedBytes)),
            fmt::arg("uploaded_size", tr_strlsize(stats.uploadedBytes)));
    }
    else if (pch == "total-transfer")
    {
        auto const stats = tr_sessionGetCumulativeStats(session);
        buf = fmt::format(
            C_("all-time totals", "Down: {downloaded_size}, Up: {uploaded_size}"),
            fmt::arg("downloaded_size", tr_strlsize(stats.downloadedBytes)),
            fmt::arg("uploaded_size", tr_strlsize(stats.uploadedBytes)));
    }
    else /* default is total-ratio */
    {
        auto const stats = tr_sessionGetCumulativeStats(session);
        buf = fmt::format(_("Ratio: {ratio}"), fmt::arg("ratio", tr_strlratio(stats.ratio)));
    }

    stats_lb_->set_text(buf);
}

void MainWindow::Impl::updateSpeeds()
{
    auto const* const session = core_->get_session();

    if (session != nullptr)
    {
        auto dn_count = int{};
        auto dn_speed = Speed{};
        auto up_count = int{};
        auto up_speed = Speed{};

        auto const model = core_->get_model();
        for (auto i = 0U, count = model->get_n_items(); i < count; ++i)
        {
            auto const torrent = gtr_ptr_dynamic_cast<Torrent>(model->get_object(i));
            dn_count += torrent->get_active_peers_down();
            dn_speed += torrent->get_speed_down();
            up_count += torrent->get_active_peers_up();
            up_speed += torrent->get_speed_up();
        }

        dl_lb_->set_text(fmt::format(fmt::runtime(_("{download_speed} ▼")), fmt::arg("download_speed", dn_speed.to_string())));
        dl_lb_->set_visible(dn_count > 0);

        ul_lb_->set_text(fmt::format(fmt::runtime(_("{upload_speed} ▲")), fmt::arg("upload_speed", up_speed.to_string())));
        ul_lb_->set_visible(dn_count > 0 || up_count > 0);
    }
}

void MainWindow::refresh()
{
    impl_->refresh();
}

void MainWindow::Impl::refresh()
{
    if (core_ != nullptr && core_->get_session() != nullptr)
    {
        updateSpeeds();
        updateStats();
    }
}

Glib::RefPtr<MainWindow::Impl::TorrentViewSelection> MainWindow::Impl::get_selection() const
{
    return IF_GTKMM4(selection_, view_->get_selection());
}

void MainWindow::for_each_selected_torrent(std::function<void(Glib::RefPtr<Torrent> const&)> const& callback) const
{
    for_each_selected_torrent_until(sigc::bind_return(callback, false));
}

bool MainWindow::for_each_selected_torrent_until(std::function<bool(Glib::RefPtr<Torrent> const&)> const& callback) const
{
    auto const selection = impl_->get_selection();
    auto const model = selection->get_model();
    bool result = false;

#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto const selected_items = selection->get_selection(); // TODO(C++20): Move into the `for`
    for (auto const position : *selected_items)
    {
        if (callback(gtr_ptr_dynamic_cast<Torrent>(model->get_object(position))))
        {
            result = true;
            break;
        }
    }
#else
    static auto const& self_col = Torrent::get_columns().self;

    for (auto const& path : selection->get_selected_rows())
    {
        auto const torrent = Glib::make_refptr_for_instance(model->get_iter(path)->get_value(self_col));
        torrent->reference();
        if (callback(torrent))
        {
            result = true;
            break;
        }
    }
#endif

    return result;
}

void MainWindow::select_all()
{
    impl_->get_selection()->select_all();
}

void MainWindow::unselect_all()
{
    impl_->get_selection()->unselect_all();
}

void MainWindow::set_busy(bool isBusy)
{
    if (get_realized())
    {
#if GTKMM_CHECK_VERSION(4, 0, 0)
        auto const cursor = isBusy ? Gdk::Cursor::create(Glib::ustring("wait")) : Glib::RefPtr<Gdk::Cursor>();
        set_cursor(cursor);
#else
        auto const display = get_display();
        auto const cursor = isBusy ? Gdk::Cursor::create(display, Gdk::WATCH) : Glib::RefPtr<Gdk::Cursor>();
        get_window()->set_cursor(cursor);
        display->flush();
#endif
    }
}

sigc::signal<void()>& MainWindow::signal_selection_changed()
{
    return impl_->signal_selection_changed();
}
