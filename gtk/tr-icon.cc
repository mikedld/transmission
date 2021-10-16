/*
 * This file Copyright (C) 2007-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <glibmm.h>
#include <glibmm/i18n.h>
#include <gtkmm.h>

#ifdef HAVE_LIBAPPINDICATOR
#include <libappindicator/app-indicator.h>
#endif

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>

#include "actions.h"
#include "tr-core.h"
#include "tr-icon.h"
#include "util.h"

#define ICON_NAME "transmission"

class SystemTrayIcon::Impl
{
public:
    Impl(TrCore* core);
    ~Impl();

    void refresh();

private:
    void activated();
    void popup(guint button, guint when);

private:
    TrCore* const core_;

    Gtk::Menu* menu_;

#ifdef HAVE_LIBAPPINDICATOR
    AppIndicator* indicator_;
#else
    Glib::RefPtr<Gtk::StatusIcon> icon_;
#endif
};

#ifdef HAVE_LIBAPPINDICATOR

SystemTrayIcon::Impl::~Impl()
{
    g_object_unref(indicator_);
}

void SystemTrayIcon::Impl::refresh()
{
}

#else

SystemTrayIcon::Impl::~Impl() = default;

void SystemTrayIcon::Impl::activated()
{
    gtr_action_activate("toggle-main-window");
}

void SystemTrayIcon::Impl::popup([[maybe_unused]] guint button, [[maybe_unused]] guint when)
{
#if GTK_CHECK_VERSION(3, 22, 0)
    menu_->popup_at_pointer(nullptr);
#else
    menu_->popup(button, when);
#endif
}

void SystemTrayIcon::Impl::refresh()
{
    double KBps;
    double limit;
    char up[64];
    Glib::ustring upLimit;
    char down[64];
    Glib::ustring downLimit;
    char const* idle = _("Idle");
    tr_session* session = gtr_core_session(core_);

    /* up */
    KBps = tr_sessionGetRawSpeed_KBps(session, TR_UP);

    if (KBps < 0.001)
    {
        g_strlcpy(up, idle, sizeof(up));
    }
    else
    {
        tr_formatter_speed_KBps(up, KBps, sizeof(up));
    }

    /* up limit */
    if (tr_sessionGetActiveSpeedLimit_KBps(session, TR_UP, &limit))
    {
        char buf[64];
        tr_formatter_speed_KBps(buf, limit, sizeof(buf));
        upLimit = Glib::ustring::sprintf(_(" (Limit: %s)"), buf);
    }

    /* down */
    KBps = tr_sessionGetRawSpeed_KBps(session, TR_DOWN);

    if (KBps < 0.001)
    {
        g_strlcpy(down, idle, sizeof(down));
    }
    else
    {
        tr_formatter_speed_KBps(down, KBps, sizeof(down));
    }

    /* down limit */
    if (tr_sessionGetActiveSpeedLimit_KBps(session, TR_DOWN, &limit))
    {
        char buf[64];
        tr_formatter_speed_KBps(buf, limit, sizeof(buf));
        downLimit = Glib::ustring::sprintf(_(" (Limit: %s)"), buf);
    }

    /* %1$s: current upload speed
     * %2$s: current upload limit, if any
     * %3$s: current download speed
     * %4$s: current download limit, if any */
    auto const tip = Glib::ustring::sprintf(_("Transmission\nUp: %1$s %2$s\nDown: %3$s %4$s"), up, upLimit, down, downLimit);

    icon_->set_tooltip_text(tip);
}

#endif

namespace
{

std::string getIconName()
{
    std::string icon_name;

    auto theme = Gtk::IconTheme::get_default();

    // if the tray's icon is a 48x48 file, use it.
    // otherwise, use the fallback builtin icon.
    if (!theme->has_icon(TRAY_ICON))
    {
        icon_name = ICON_NAME;
    }
    else
    {
        auto const icon_info = theme->lookup_icon(TRAY_ICON, 48, Gtk::ICON_LOOKUP_USE_BUILTIN);
        bool const icon_is_builtin = icon_info.get_filename().empty();

        icon_name = icon_is_builtin ? ICON_NAME : TRAY_ICON;
    }

    return icon_name;
}

} // namespace

SystemTrayIcon::SystemTrayIcon(TrCore* core)
    : impl_(std::make_unique<Impl>(core))
{
}

SystemTrayIcon::~SystemTrayIcon() = default;

void SystemTrayIcon::refresh()
{
    impl_->refresh();
}

SystemTrayIcon::Impl::Impl(TrCore* core)
    : core_(core)
{
    auto const icon_name = getIconName();
    menu_ = gtr_action_get_widget<Gtk::Menu>("/icon-popup");

#ifdef HAVE_LIBAPPINDICATOR

    indicator_ = app_indicator_new(ICON_NAME, icon_name.c_str(), APP_INDICATOR_CATEGORY_SYSTEM_SERVICES);
    app_indicator_set_status(indicator_, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator_, Glib::unwrap(menu_));
    app_indicator_set_title(indicator_, Glib::get_application_name().c_str());

#else

    icon_ = Gtk::StatusIcon::create(icon_name);
    icon_->signal_activate().connect(sigc::mem_fun(this, &Impl::activated));
    icon_->signal_popup_menu().connect(sigc::mem_fun(this, &Impl::popup));

#endif
}
