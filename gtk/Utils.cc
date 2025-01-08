// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "Utils.h"

#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"

#include <libtransmission/transmission.h> /* TR_RATIO_NA, TR_RATIO_INF */
#include <libtransmission/error.h>
#include <libtransmission/torrent-metainfo.h>
#include <libtransmission/utils.h> /* tr_strratio() */
#include <libtransmission/values.h>
#include <libtransmission/version.h> /* SHORT_VERSION_STRING */
#include <libtransmission/web-utils.h>

#include <gdkmm/display.h>
#include <giomm/appinfo.h>
#include <giomm/asyncresult.h>
#include <giomm/file.h>
#include <glibmm/error.h>
#include <glibmm/i18n.h>
#include <glibmm/quark.h>
#include <glibmm/spawn.h>
#include <gtkmm/cellrenderertext.h>
#include <gtkmm/liststore.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/treemodel.h>
#include <gtkmm/treemodelcolumn.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gdkmm/clipboard.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/gesture.h>
#include <gtkmm/gestureclick.h>
#else
#include <gdkmm/window.h>
#include <gtkmm/clipboard.h>
#endif

#include <fmt/core.h>

#include <functional>
#include <memory>
#include <stack>
#include <stdexcept>
#include <utility>

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#if GTK_CHECK_VERSION(4, 0, 0) && defined(GDK_WINDOWING_X11)
#include <optional>

#include <gdk/x11/gdkx.h>
#endif

using namespace std::literals;

using namespace libtransmission::Values;

/***
****
***/

void gtr_message(std::string const& message)
{
    // NOLINTNEXTLINE(*-vararg)
    g_message("%s", message.c_str());
}

void gtr_warning(std::string const& message)
{
    // NOLINTNEXTLINE(*-vararg)
    g_warning("%s", message.c_str());
}

void gtr_error(std::string const& message)
{
    // NOLINTNEXTLINE(*-vararg)
    g_error("%s", message.c_str());
}

/***
****
***/

Glib::ustring gtr_get_unicode_string(GtrUnicode uni)
{
    switch (uni)
    {
    case GtrUnicode::Up:
        return "\xE2\x96\xB4";

    case GtrUnicode::Down:
        return "\xE2\x96\xBE";

    case GtrUnicode::Inf:
        return "\xE2\x88\x9E";

    case GtrUnicode::Bullet:
        return "\xE2\x88\x99";

    default:
        return "err";
    }
}

Glib::ustring tr_strlratio(double ratio)
{
    return tr_strratio(ratio, gtr_get_unicode_string(GtrUnicode::Inf).c_str());
}

Glib::ustring tr_strlsize(libtransmission::Values::Storage const& storage)
{
    return storage.is_zero() ? Q_("None") : storage.to_string();
}

Glib::ustring tr_strlsize(guint64 n_bytes)
{
    return tr_strlsize(Storage{ n_bytes, Storage::Units::Bytes });
}

namespace
{

std::string tr_format_future_time(time_t seconds)
{
    if (auto const days_from_now = seconds / 86400U; days_from_now > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{days_from_now:L} day from now", "{days_from_now:L} days from now", days_from_now)),
            fmt::arg("days_from_now", days_from_now));
    }

    if (auto const hours_from_now = (seconds % 86400U) / 3600U; hours_from_now > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{hours_from_now:L} hour from now", "{hours_from_now:L} hours from now", hours_from_now)),
            fmt::arg("hours_from_now", hours_from_now));
    }

    if (auto const minutes_from_now = (seconds % 3600U) / 60U; minutes_from_now > 0U)
    {
        return fmt::format(
            fmt::runtime(
                ngettext("{minutes_from_now:L} minute from now", "{minutes_from_now:L} minutes from now", minutes_from_now)),
            fmt::arg("minutes_from_now", minutes_from_now));
    }

    if (auto const seconds_from_now = seconds % 60U; seconds_from_now > 0U)
    {
        return fmt::format(
            fmt::runtime(
                ngettext("{seconds_from_now:L} second from now", "{seconds_from_now:L} seconds from now", seconds_from_now)),
            fmt::arg("seconds_from_now", seconds_from_now));
    }

    return _("now");
}

std::string tr_format_past_time(time_t seconds)
{
    if (auto const days_ago = seconds / 86400U; days_ago > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{days_ago:L} day ago", "{days_ago:L} days ago", days_ago)),
            fmt::arg("days_ago", days_ago));
    }

    if (auto const hours_ago = (seconds % 86400U) / 3600U; hours_ago > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{hours_ago:L} hour ago", "{hours_ago:L} hours ago", hours_ago)),
            fmt::arg("hours_ago", hours_ago));
    }

    if (auto const minutes_ago = (seconds % 3600U) / 60U; minutes_ago > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{minutes_ago:L} minute ago", "{minutes_ago:L} minutes ago", minutes_ago)),
            fmt::arg("minutes_ago", minutes_ago));
    }

    if (auto const seconds_ago = seconds % 60U; seconds_ago > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{seconds_ago:L} second ago", "{seconds_ago:L} seconds ago", seconds_ago)),
            fmt::arg("seconds_ago", seconds_ago));
    }

    return _("now");
}

} // namespace

std::string tr_format_time(time_t timestamp)
{
    if (auto const days = timestamp / 86400U; days > 0U)
    {
        return fmt::format(fmt::runtime(ngettext("{days:L} day", "{days:L} days", days)), fmt::arg("days", days));
    }

    if (auto const hours = (timestamp % 86400U) / 3600U; hours > 0U)
    {
        return fmt::format(fmt::runtime(ngettext("{hours:L} hour", "{hours:L} hours", hours)), fmt::arg("hours", hours));
    }

    if (auto const minutes = (timestamp % 3600U) / 60U; minutes > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{minutes:L} minute", "{minutes:L} minutes", minutes)),
            fmt::arg("minutes", minutes));
    }

    if (auto const seconds = timestamp % 60U; seconds > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{seconds:L} second", "{seconds:L} seconds", seconds)),
            fmt::arg("seconds", seconds));
    }

    return _("now");
}

std::string tr_format_time_left(time_t timestamp)
{
    if (auto const days_left = timestamp / 86400U; days_left > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{days_left:L} day left", "{days_left:L} days left", days_left)),
            fmt::arg("days_left", days_left));
    }

    if (auto const hours_left = (timestamp % 86400U) / 3600U; hours_left > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{hours_left:L} hour left", "{hours_left:L} hours left", hours_left)),
            fmt::arg("hours_left", hours_left));
    }

    if (auto const minutes_left = (timestamp % 3600U) / 60U; minutes_left > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{minutes_left:L} minute left", "{minutes_left:L} minutes left", minutes_left)),
            fmt::arg("minutes_left", minutes_left));
    }

    if (auto const seconds_left = timestamp % 60U; seconds_left > 0U)
    {
        return fmt::format(
            fmt::runtime(ngettext("{seconds_left:L} second left", "{seconds_left:L} seconds left", seconds_left)),
            fmt::arg("seconds_left", seconds_left));
    }

    return _("now");
}

std::string tr_format_time_relative(time_t timestamp, time_t origin)
{
    return timestamp < origin ? tr_format_future_time(origin - timestamp) : tr_format_past_time(timestamp - origin);
}

void gtr_add_torrent_error_dialog(Gtk::Widget& child, tr_torrent* duplicate_torrent, std::string const& filename)
{
    Glib::ustring secondary;

    if (duplicate_torrent != nullptr)
    {
        secondary = fmt::format(
            fmt::runtime(_("The torrent file '{path}' is already in use by '{torrent_name}'.")),
            fmt::arg("path", filename),
            fmt::arg("torrent_name", tr_torrentName(duplicate_torrent)));
    }
    else
    {
        secondary = fmt::format(fmt::runtime(_("Couldn't add torrent file '{path}'")), fmt::arg("path", filename));
    }

    auto w = std::make_shared<Gtk::MessageDialog>(
        gtr_widget_get_window(child),
        _("Couldn't open torrent"),
        false,
        TR_GTK_MESSAGE_TYPE(ERROR),
        TR_GTK_BUTTONS_TYPE(CLOSE));
    w->set_secondary_text(secondary);
    w->signal_response().connect([w](int /*response*/) mutable { w.reset(); });
    w->show();
}

/* pop up the context menu if a user right-clicks.
   if the row they right-click on isn't selected, select it. */
bool on_item_view_button_pressed(
    Gtk::TreeView& view,
    double event_x,
    double event_y,
    bool context_menu_requested,
    std::function<void(double, double)> const& callback)
{
    if (context_menu_requested)
    {
        Gtk::TreeModel::Path path;

        if (auto const selection = view.get_selection();
            view.get_path_at_pos(static_cast<int>(event_x), static_cast<int>(event_y), path) && !selection->is_selected(path))
        {
            selection->unselect_all();
            selection->select(path);
        }

        if (callback)
        {
            callback(event_x, event_y);
        }

        return true;
    }

    return false;
}

#if GTKMM_CHECK_VERSION(4, 0, 0)

namespace
{

// NOTE: Estimated position (`get_position_from_allocation` vfunc is private)
std::optional<guint> get_position_from_allocation(Gtk::ListView& view, double view_x, double view_y)
{
    auto* child = view.pick(view_x, view_y);
    while (child != nullptr && child->get_css_name() != "row")
    {
        child = child->get_parent();
    }

    if (child == nullptr)
    {
        return {};
    }

    double top_x = 0;
    double top_y = 0;
    child->translate_coordinates(view, 0, 0, top_x, top_y);
    return static_cast<guint>((top_y + view.get_vadjustment()->get_value()) / child->get_allocated_height());
}

} // namespace

bool on_item_view_button_pressed(
    Gtk::ListView& view,
    double event_x,
    double event_y,
    bool context_menu_requested,
    std::function<void(double, double)> const& callback)
{
    if (context_menu_requested)
    {
        if (auto const position = get_position_from_allocation(view, event_x, event_y); position.has_value())
        {
            if (auto const selection_model = view.get_model(); !selection_model->is_selected(position.value()))
            {
                selection_model->select_item(position.value(), true);
            }
        }

        if (callback)
        {
            callback(event_x, event_y);
        }

        return true;
    }

    return false;
}

#endif

/* if the user clicked in an empty area of the list,
 * clear all the selections. */
bool on_item_view_button_released(Gtk::TreeView& view, double event_x, double event_y)
{
    if (Gtk::TreeModel::Path path; !view.get_path_at_pos(static_cast<int>(event_x), static_cast<int>(event_y), path))
    {
        view.get_selection()->unselect_all();
    }

    return false;
}

#if GTKMM_CHECK_VERSION(4, 0, 0)

bool on_item_view_button_released(Gtk::ListView& view, double event_x, double event_y)
{
    if (!get_position_from_allocation(view, event_x, event_y).has_value())
    {
        view.get_model()->unselect_all();
    }

    return false;
}

#endif

namespace
{

#if GTKMM_CHECK_VERSION(4, 0, 0)

std::pair<int, int> convert_widget_to_bin_window_coords(Gtk::TreeView const& view, int view_x, int view_y)
{
    int event_x = 0;
    int event_y = 0;
    view.convert_widget_to_bin_window_coords(view_x, view_y, event_x, event_y);
    return { event_x, event_y };
}

std::pair<int, int> convert_widget_to_bin_window_coords(Gtk::ListView const& /*view*/, int view_x, int view_y)
{
    return { view_x, view_y };
}

#endif

template<typename T>
void setup_item_view_button_event_handling_impl(
    T& view,
    std::function<bool(guint, TrGdkModifierType, double, double, bool)> const& press_callback,
    std::function<bool(double, double)> const& release_callback)
{
#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto controller = Gtk::GestureClick::create();
    controller->set_button(0);
    controller->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    if (press_callback)
    {
        controller->signal_pressed().connect(
            [&view, press_callback, controller](int /*n_press*/, double view_x, double view_y)
            {
                auto const [event_x, event_y] = convert_widget_to_bin_window_coords(
                    view,
                    static_cast<int>(view_x),
                    static_cast<int>(view_y));

                auto* const sequence = controller->get_current_sequence();
                auto const event = controller->get_last_event(sequence);

                if (event->get_event_type() == TR_GDK_EVENT_TYPE(BUTTON_PRESS) &&
                    press_callback(
                        event->get_button(),
                        event->get_modifier_state(),
                        event_x,
                        event_y,
                        event->triggers_context_menu()))
                {
                    controller->set_sequence_state(sequence, Gtk::EventSequenceState::CLAIMED);
                }
            },
            false);
    }
    if (release_callback)
    {
        controller->signal_released().connect(
            [&view, release_callback, controller](int /*n_press*/, double view_x, double view_y)
            {
                auto const [event_x, event_y] = convert_widget_to_bin_window_coords(
                    view,
                    static_cast<int>(view_x),
                    static_cast<int>(view_y));

                auto* const sequence = controller->get_current_sequence();
                auto const event = controller->get_last_event(sequence);

                if (event->get_event_type() == TR_GDK_EVENT_TYPE(BUTTON_RELEASE) && release_callback(event_x, event_y))
                {
                    controller->set_sequence_state(sequence, Gtk::EventSequenceState::CLAIMED);
                }
            });
    }
    view.add_controller(controller);
#else
    if (press_callback)
    {
        view.signal_button_press_event().connect(
            [press_callback](GdkEventButton* event)
            { return press_callback(event->button, event->state, event->x, event->y, event->button == GDK_BUTTON_SECONDARY); },
            false);
    }
    if (release_callback)
    {
        view.signal_button_release_event().connect([release_callback](GdkEventButton* event)
                                                   { return release_callback(event->x, event->y); });
    }
#endif
}

} // namespace

void setup_item_view_button_event_handling(
    Gtk::TreeView& view,
    std::function<bool(guint, TrGdkModifierType, double, double, bool)> const& press_callback,
    std::function<bool(double, double)> const& release_callback)
{
    setup_item_view_button_event_handling_impl(view, press_callback, release_callback);
}

#if GTKMM_CHECK_VERSION(4, 0, 0)

void setup_item_view_button_event_handling(
    Gtk::ListView& view,
    std::function<bool(guint, TrGdkModifierType, double, double, bool)> const& press_callback,
    std::function<bool(double, double)> const& release_callback)
{
    setup_item_view_button_event_handling_impl(view, press_callback, release_callback);
}

#endif

bool gtr_file_trash_or_remove(std::string const& filename, tr_error* error)
{
    g_return_val_if_fail(!filename.empty(), false);

    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    auto const file = Gio::File::create_for_path(filename);
    bool trashed = false;

    if (gtr_pref_flag_get(TR_KEY_trash_can_enabled))
    {
        try
        {
            trashed = file->trash();
        }
        catch (Glib::Error const& e)
        {
            error->set(e.code(), TR_GLIB_EXCEPTION_WHAT(e));
            gtr_message(fmt::format(
                fmt::runtime(_("Couldn't move '{path}' to trash: {error} ({error_code})")),
                fmt::arg("path", filename),
                fmt::arg("error", error->message()),
                fmt::arg("error_code", error->code())));
        }
    }

    bool result = true;
    if (!trashed)
    {
        try
        {
            file->remove();
        }
        catch (Glib::Error const& e)
        {
            error->set(e.code(), TR_GLIB_EXCEPTION_WHAT(e));
            gtr_message(fmt::format(
                fmt::runtime(_("Couldn't remove '{path}': {error} ({error_code})")),
                fmt::arg("path", filename),
                fmt::arg("error", error->message()),
                fmt::arg("error_code", error->code())));
            result = false;
        }
    }

    return result;
}

namespace
{

void object_signal_notify_callback(GObject* object, GParamSpec* /*param_spec*/, gpointer data)
{
    if (object != nullptr && data != nullptr)
    {
        if (auto const* const slot = Glib::SignalProxyBase::data_to_slot(data); slot != nullptr)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            (*static_cast<sigc::slot<TrObjectSignalNotifyCallback> const*>(slot))(Glib::wrap(object, true));
        }
    }
}

} // namespace

Glib::SignalProxy<TrObjectSignalNotifyCallback> gtr_object_signal_notify(Glib::ObjectBase& object)
{
    static auto const object_signal_notify_info = Glib::SignalProxyInfo{
        .signal_name = "notify",
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        .callback = reinterpret_cast<GCallback>(&object_signal_notify_callback),
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        .notify_callback = reinterpret_cast<GCallback>(&object_signal_notify_callback),
    };

    return { &object, &object_signal_notify_info };
}

void gtr_object_notify_emit(Glib::ObjectBase& object)
{
    // NOLINTNEXTLINE(*-vararg)
    g_signal_emit_by_name(object.gobj(), "notify", nullptr);
}

Glib::ustring gtr_get_help_uri()
{
    static auto const uri = fmt::format("https://transmissionbt.com/help/gtk/{}.{}x", MAJOR_VERSION, MINOR_VERSION / 10);
    return uri;
}

void gtr_open_file(std::string const& path)
{
    gtr_open_uri(Gio::File::create_for_path(path)->get_uri());
}

void gtr_open_uri(Glib::ustring const& uri)
{
    if (!uri.empty())
    {
        bool opened = false;

        if (!opened)
        {
            try
            {
                opened = Gio::AppInfo::launch_default_for_uri(uri);
            }
            catch (Glib::Error const&)
            {
            }
        }

        if (!opened)
        {
            try
            {
                Glib::spawn_async({}, std::vector<std::string>{ "xdg-open", uri }, TR_GLIB_SPAWN_FLAGS(SEARCH_PATH));
                opened = true;
            }
            catch (Glib::SpawnError const&)
            {
            }
        }

        if (!opened)
        {
            gtr_message(fmt::format(fmt::runtime(_("Couldn't open '{url}'")), fmt::arg("url", uri)));
        }
    }
}

// ---

namespace
{

class EnumComboModelColumns : public Gtk::TreeModelColumnRecord
{
public:
    EnumComboModelColumns() noexcept
    {
        add(value);
        add(label);
    }

    Gtk::TreeModelColumn<int> value;
    Gtk::TreeModelColumn<Glib::ustring> label;
};

EnumComboModelColumns const enum_combo_cols;

} // namespace

void gtr_combo_box_set_active_enum(Gtk::ComboBox& combo, int value)
{
    auto const& column = enum_combo_cols.value;

    /* do the value and current value match? */
    if (auto const iter = combo.get_active(); iter)
    {
        if (iter->get_value(column) == value)
        {
            return;
        }
    }

    /* find the one to select */
    for (auto const& row : combo.get_model()->children())
    {
        if (row.get_value(column) == value)
        {
            combo.set_active(TR_GTK_TREE_MODEL_CHILD_ITER(row));
            return;
        }
    }
}

void gtr_combo_box_set_enum(Gtk::ComboBox& combo, std::vector<std::pair<Glib::ustring, int>> const& items)
{
    auto store = Gtk::ListStore::create(enum_combo_cols);

    for (auto const& [label, value] : items)
    {
        auto const iter = store->append();
        (*iter)[enum_combo_cols.value] = value;
        (*iter)[enum_combo_cols.label] = label;
    }

    combo.clear();
    combo.set_model(store);

    auto* r = Gtk::make_managed<Gtk::CellRendererText>();
    combo.pack_start(*r, true);
    combo.add_attribute(r->property_text(), enum_combo_cols.label);
}

int gtr_combo_box_get_active_enum(Gtk::ComboBox const& combo)
{
    int value = 0;

    if (auto const iter = combo.get_active(); iter)
    {
        iter->get_value(0, value);
    }

    return value;
}

void gtr_priority_combo_init(Gtk::ComboBox& combo)
{
    gtr_combo_box_set_enum(
        combo,
        {
            { _("High"), TR_PRI_HIGH },
            { _("Normal"), TR_PRI_NORMAL },
            { _("Low"), TR_PRI_LOW },
        });
}

// ---

void gtr_widget_set_visible(Gtk::Widget& widget, bool is_visible)
{
    static auto const ChildHiddenKey = Glib::Quark("gtr-child-hidden");

    auto* const widget_as_window = dynamic_cast<Gtk::Window*>(&widget);
    if (widget_as_window == nullptr)
    {
        widget.set_visible(is_visible);
        return;
    }

    /* toggle the transient children, too */
    auto windows = std::stack<Gtk::Window*>();
    windows.push(widget_as_window);

    while (!windows.empty())
    {
        auto* const window = windows.top();
        bool transient_child_found = false;

        for (auto* const top_level_window : Gtk::Window::list_toplevels())
        {
#if !GTKMM_CHECK_VERSION(4, 0, 0)
            if (top_level_window->get_window_type() != Gtk::WINDOW_TOPLEVEL)
            {
                continue;
            }
#endif

            if (top_level_window->get_transient_for() != window || top_level_window->get_visible() == is_visible)
            {
                continue;
            }

            windows.push(top_level_window);
            transient_child_found = true;
            break;
        }

        if (transient_child_found)
        {
            continue;
        }

        if (is_visible && window->get_data(ChildHiddenKey) != nullptr)
        {
            window->steal_data(ChildHiddenKey);
            window->set_visible(true);
        }
        else if (!is_visible)
        {
            window->set_data(ChildHiddenKey, GINT_TO_POINTER(1));
            window->set_visible(false);
        }

        windows.pop();
    }
}

Gtk::Window& gtr_widget_get_window(Gtk::Widget& widget)
{
    if (auto* const window = dynamic_cast<Gtk::Window*>(TR_GTK_WIDGET_GET_ROOT(widget)); window != nullptr)
    {
        return *window;
    }

#if defined(G_DISABLE_ASSERT)
    throw std::logic_error("Supplied widget doesn't have a window");
#else
    g_assert_not_reached();
#endif
}

void gtr_window_set_skip_taskbar_hint([[maybe_unused]] Gtk::Window& window, [[maybe_unused]] bool value)
{
#if GTK_CHECK_VERSION(4, 0, 0)
#if defined(GDK_WINDOWING_X11)
    if (auto* const surface = Glib::unwrap(window.get_surface()); GDK_IS_X11_SURFACE(surface))
    {
        gdk_x11_surface_set_skip_taskbar_hint(surface, value ? TRUE : FALSE);
    }
#endif
#else
    window.set_skip_taskbar_hint(value);
#endif
}

void gtr_window_set_urgency_hint([[maybe_unused]] Gtk::Window& window, [[maybe_unused]] bool value)
{
#if GTK_CHECK_VERSION(4, 0, 0)
#if defined(GDK_WINDOWING_X11)
    if (auto* const surface = Glib::unwrap(window.get_surface()); GDK_IS_X11_SURFACE(surface))
    {
        gdk_x11_surface_set_urgency_hint(surface, value ? TRUE : FALSE);
    }
#endif
#else
    window.set_urgency_hint(value);
#endif
}

void gtr_window_raise([[maybe_unused]] Gtk::Window& window)
{
#if !GTKMM_CHECK_VERSION(4, 0, 0)
    window.get_window()->raise();
#endif
}

// ---

void gtr_unrecognized_url_dialog(Gtk::Widget& parent, Glib::ustring const& url)
{
    Glib::ustring gstr;

    auto w = std::make_shared<Gtk::MessageDialog>(
        gtr_widget_get_window(parent),
        fmt::format(fmt::runtime(_("Unsupported URL: '{url}'")), fmt::arg("url", url)),
        false /*use markup*/,
        TR_GTK_MESSAGE_TYPE(ERROR),
        TR_GTK_BUTTONS_TYPE(CLOSE),
        true /*modal*/);

    gstr += fmt::format(fmt::runtime(_("Transmission doesn't know how to use '{url}'")), fmt::arg("url", url));

    if (tr_magnet_metainfo{}.parseMagnet(url.raw()))
    {
        gstr += "\n \n";
        gstr += _("This magnet link appears to be intended for something other than BitTorrent.");
    }

    w->set_secondary_text(gstr);
    w->signal_response().connect([w](int /*response*/) mutable { w.reset(); });
    w->show();
}

/***
****
***/

void gtr_paste_clipboard_url_into_entry(Gtk::Entry& entry)
{
    auto const process = [&entry](Glib::ustring const& text)
    {
        if (auto const sv = tr_strv_strip(text.raw());
            !sv.empty() && (tr_urlIsValid(sv) || tr_magnet_metainfo{}.parseMagnet(sv)))
        {
            entry.set_text(text);
            return true;
        }
        return false;
    };

#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto const request = [](Glib::RefPtr<Gdk::Clipboard> const& clipboard, auto&& callback)
    {
        clipboard->read_text_async([clipboard, callback](Glib::RefPtr<Gio::AsyncResult>& result)
                                   { callback(clipboard->read_text_finish(result)); });
    };

    request(
        Gdk::Display::get_default()->get_primary_clipboard(),
        [request, process](Glib::ustring const& text)
        {
            if (!process(text))
            {
                request(Gdk::Display::get_default()->get_clipboard(), process);
            }
        });
#else
    for (auto const& str : { Gtk::Clipboard::get(GDK_SELECTION_PRIMARY)->wait_for_text(),
                             Gtk::Clipboard::get(GDK_SELECTION_CLIPBOARD)->wait_for_text() })
    {
        if (process(str))
        {
            break;
        }
    }
#endif
}

/***
****
***/

void gtr_label_set_text(Gtk::Label& lb, Glib::ustring const& text)
{
    if (lb.get_text() != text)
    {
        lb.set_text(text);
    }
}

std::string gtr_get_full_resource_path(std::string const& rel_path)
{
    static auto const BasePath = "/com/transmissionbt/transmission/"s;
    return BasePath + rel_path;
}

/***
****
***/

size_t const max_recent_dirs = size_t{ 4 };

std::list<std::string> gtr_get_recent_dirs(std::string const& pref)
{
    std::list<std::string> list;

    for (size_t i = 0; i < max_recent_dirs; ++i)
    {
        auto const key = fmt::format("recent-{}-dir-{}", pref, i + 1);

        if (auto const val = gtr_pref_string_get(tr_quark_new(key)); !val.empty())
        {
            list.push_back(val);
        }
    }

    return list;
}

void gtr_save_recent_dir(std::string const& pref, Glib::RefPtr<Session> const& core, std::string const& dir)
{
    if (dir.empty())
    {
        return;
    }

    auto list = gtr_get_recent_dirs(pref);

    /* if it was already in the list, remove it */
    list.remove(dir);

    /* add it to the front of the list */
    list.push_front(dir);

    /* save the first max_recent_dirs directories */
    list.resize(max_recent_dirs);
    int i = 0;
    for (auto const& d : list)
    {
        auto const key = fmt::format("recent-{}-dir-{}", pref, ++i);
        gtr_pref_string_set(tr_quark_new(key), d);
    }

    gtr_pref_save(core->get_session());
}
