// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

// _AppIndicatorClass::{fallback,unfallback} use deprecated GtkStatusIcon
#undef GTK_DISABLE_DEPRECATED
// We're using deprecated Gtk::StatusItem ourselves as well
#undef GTKMM_DISABLE_DEPRECATED

#include "SystemTrayIcon.h"

#include "Actions.h"
#include "Session.h"
#include "Utils.h"

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>

#include <glibmm/i18n.h>
#include <glibmm/ustring.h>

#if !GTKMM_CHECK_VERSION(4, 0, 0)
#include <giomm/menu.h>
#include <glibmm/miscutils.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/menu.h>
#if !defined(HAVE_APPINDICATOR)
#include <gtkmm/statusicon.h>
#endif
#endif

#include <string>

#ifdef HAVE_APPINDICATOR
#ifdef APPINDICATOR_IS_AYATANA
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif
#endif

#ifdef HAVE_APPINDICATOR
#define TR_SYS_TRAY_IMPL_APPINDICATOR
#elif !GTKMM_CHECK_VERSION(4, 0, 0)
#define TR_SYS_TRAY_IMPL_STATUS_ICON
#else
#define TR_SYS_TRAY_IMPL_NONE
#endif

using namespace std::literals;
using namespace libtransmission::Values;

namespace
{

#if !defined(TR_SYS_TRAY_IMPL_NONE)
char const* const TrayIconName = "transmission-tray-icon";
char const* const AppIconName = "transmission";
#endif

#if defined(TR_SYS_TRAY_IMPL_APPINDICATOR)
char const* const AppName = "transmission-gtk";
#endif

} // namespace

class SystemTrayIcon::Impl
{
public:
    Impl(Gtk::Window& main_window, Glib::RefPtr<Session> const& core);
    Impl(Impl&&) = delete;
    Impl(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    ~Impl();

    void refresh();

private:
    void activated();
    void popup(guint button, guint when);

    [[nodiscard]] std::string make_tooltip_text() const;

private:
    Glib::RefPtr<Session> const core_;

#if !defined(TR_SYS_TRAY_IMPL_NONE)
    Gtk::Menu* menu_;
#endif

#if defined(TR_SYS_TRAY_IMPL_APPINDICATOR)
    AppIndicator* indicator_;
#elif defined(TR_SYS_TRAY_IMPL_STATUS_ICON)
    Glib::RefPtr<Gtk::StatusIcon> icon_;
#endif
};

#if defined(TR_SYS_TRAY_IMPL_APPINDICATOR)

SystemTrayIcon::Impl::~Impl()
{
    g_object_unref(indicator_);
}

void SystemTrayIcon::Impl::refresh()
{
}

#elif defined(TR_SYS_TRAY_IMPL_STATUS_ICON)

SystemTrayIcon::Impl::~Impl() = default;

void SystemTrayIcon::Impl::activated()
{
    gtr_action_activate("toggle-main-window");
}

void SystemTrayIcon::Impl::popup(guint /*button*/, guint /*when*/)
{
    menu_->popup_at_pointer(nullptr);
}

void SystemTrayIcon::Impl::refresh()
{
    icon_->set_tooltip_text(make_tooltip_text());
}

#else

SystemTrayIcon::Impl::~Impl() = default;

#endif

namespace
{

#if !defined(TR_SYS_TRAY_IMPL_NONE)

Glib::ustring getIconName()
{
    // if the tray's icon is a 48x48 file, use it.
    // otherwise, use the fallback builtin icon.

    auto const icon = Gtk::IconTheme::get_default()->lookup_icon(TrayIconName, 48, Gtk::ICON_LOOKUP_USE_BUILTIN);
    return icon && !icon.get_filename().empty() ? TrayIconName : AppIconName;
}

#endif

} // namespace

SystemTrayIcon::SystemTrayIcon(Gtk::Window& main_window, Glib::RefPtr<Session> const& core)
    : impl_(std::make_unique<Impl>(main_window, core))
{
}

SystemTrayIcon::~SystemTrayIcon() = default;

void SystemTrayIcon::refresh()
{
#if !defined(TR_SYS_TRAY_IMPL_NONE)
    impl_->refresh();
#endif
}

bool SystemTrayIcon::is_available()
{
#if !defined(TR_SYS_TRAY_IMPL_NONE)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<SystemTrayIcon> SystemTrayIcon::create(Gtk::Window& main_window, Glib::RefPtr<Session> const& core)
{
    return is_available() ? std::make_unique<SystemTrayIcon>(main_window, core) : nullptr;
}

SystemTrayIcon::Impl::Impl([[maybe_unused]] Gtk::Window& main_window, Glib::RefPtr<Session> const& core)
    : core_(core)
{
#if !defined(TR_SYS_TRAY_IMPL_NONE)
    auto const icon_name = getIconName();
    menu_ = Gtk::make_managed<Gtk::Menu>(gtr_action_get_object<Gio::Menu>("icon-popup"));
    menu_->attach_to_widget(main_window);
#endif

#if defined(TR_SYS_TRAY_IMPL_APPINDICATOR)
    indicator_ = app_indicator_new(AppName, icon_name.c_str(), APP_INDICATOR_CATEGORY_SYSTEM_SERVICES);
    app_indicator_set_status(indicator_, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator_, Glib::unwrap(menu_));
    app_indicator_set_title(indicator_, Glib::get_application_name().c_str());
#elif defined(TR_SYS_TRAY_IMPL_STATUS_ICON)
    icon_ = Gtk::StatusIcon::create(icon_name);
    icon_->signal_activate().connect(sigc::mem_fun(*this, &Impl::activated));
    icon_->signal_popup_menu().connect(sigc::mem_fun(*this, &Impl::popup));
#endif
}

std::string SystemTrayIcon::Impl::make_tooltip_text() const
{
    auto const* const session = core_->get_session();
    return fmt::format(
        fmt::runtime(_("{upload_speed} ▲ {download_speed} ▼")),
        fmt::arg("upload_speed", Speed{ tr_sessionGetRawSpeed_KBps(session, TR_UP), Speed::Units::KByps }.to_string()),
        fmt::arg("download_speed", Speed{ tr_sessionGetRawSpeed_KBps(session, TR_DOWN), Speed::Units::KByps }.to_string()));
}
