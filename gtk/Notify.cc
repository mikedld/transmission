// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "Notify.h"

#include "GtkCompat.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

#include <giomm/asyncresult.h>
#include <giomm/dbusproxy.h>
#include <glibmm/error.h>
#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <glibmm/spawn.h>
#include <glibmm/ustring.h>
#include <glibmm/variant.h>

#include <fmt/core.h>

#include <map>
#include <utility>
#include <vector>

using namespace std::literals;

using StringVariantType = Glib::Variant<Glib::ustring>;
using StringListVariantType = Glib::Variant<std::vector<Glib::ustring>>;
using UInt32VariantType = Glib::Variant<guint32>;

namespace
{

auto const NotificationsDbusName = "org.freedesktop.Notifications"sv; // TODO(C++20): Use ""s
auto const NotificationsDbusCoreObject = "/org/freedesktop/Notifications"sv; // TODO(C++20): Use ""s
auto const NotificationsDbusCoreInterface = "org.freedesktop.Notifications"sv; // TODO(C++20): Use ""s

struct TrNotification
{
    Glib::RefPtr<Session> core;
    tr_torrent_id_t torrent_id = {};
};

Glib::RefPtr<Gio::DBus::Proxy> proxy;
std::map<guint32, TrNotification> active_notifications;
bool server_supports_actions = false;

template<typename... Ts>
Glib::VariantContainerBase make_variant_tuple(Ts&&... args)
{
    return Glib::VariantContainerBase::create_tuple(
        { Glib::Variant<std::remove_cv_t<std::remove_reference_t<Ts>>>::create(std::forward<Ts>(args))... });
}

void get_capabilities_callback(Glib::RefPtr<Gio::AsyncResult>& res)
{
    auto result = Glib::VariantContainerBase();

    try
    {
        result = proxy->call_finish(res);
    }
    catch (Glib::Error const&)
    {
        return;
    }

    if (!result || result.get_n_children() != 1 || !result.get_child(0).is_of_type(StringListVariantType::variant_type()))
    {
        return;
    }

    auto const caps = Glib::VariantBase::cast_dynamic<StringListVariantType>(result.get_child(0)).get();

    for (auto const& cap : caps)
    {
        if (cap == "actions")
        {
            server_supports_actions = true;
            break;
        }
    }
}

void g_signal_callback(
    Glib::ustring const& /*sender_name*/,
    Glib::ustring const& signal_name,
    Glib::VariantContainerBase params)
{
    g_return_if_fail(params.get_n_children() > 0 && params.get_child(0).is_of_type(UInt32VariantType::variant_type()));

    auto const id = Glib::VariantBase::cast_dynamic<UInt32VariantType>(params.get_child(0)).get();
    auto const n_it = active_notifications.find(id);

    if (n_it == active_notifications.end())
    {
        return;
    }

    auto const& n = n_it->second;

    if (signal_name == "NotificationClosed")
    {
        active_notifications.erase(n_it);
    }
    else if (
        signal_name == "ActionInvoked" && params.get_n_children() > 1 &&
        params.get_child(1).is_of_type(StringVariantType::variant_type()))
    {
        auto const* tor = n.core->find_torrent(n.torrent_id);
        if (tor == nullptr)
        {
            return;
        }

        auto const action = Glib::VariantBase::cast_dynamic<StringVariantType>(params.get_child(1)).get();

        if (action == "folder")
        {
            n.core->open_folder(n.torrent_id);
        }
        else if (action == "file")
        {
            char const* dir = tr_torrentGetDownloadDir(tor);
            auto const path = Glib::build_filename(dir, tr_torrentFile(tor, 0).name);
            gtr_open_file(path);
        }
        else if (action == "start-now")
        {
            n.core->start_now(n.torrent_id);
        }
    }
}

void dbus_proxy_ready_callback(Glib::RefPtr<Gio::AsyncResult>& res)
{
    try
    {
        proxy = Gio::DBus::Proxy::create_for_bus_finish(res);
    }
    catch (Glib::Error const& e)
    {
        gtr_warning(fmt::format(
            fmt::runtime(_("Couldn't create proxy for '{bus}': {error} ({error_code})")),
            fmt::arg("bus", NotificationsDbusName),
            fmt::arg("error", TR_GLIB_EXCEPTION_WHAT(e)),
            fmt::arg("error_code", e.code())));
        return;
    }

    proxy->signal_signal().connect(&g_signal_callback);
    proxy->call("GetCapabilities", &get_capabilities_callback);
}

} // namespace

void gtr_notify_init()
{
    Gio::DBus::Proxy::create_for_bus(
        TR_GIO_DBUS_BUS_TYPE(SESSION),
        std::string(NotificationsDbusName),
        std::string(NotificationsDbusCoreObject),
        std::string(NotificationsDbusCoreInterface),
        &dbus_proxy_ready_callback,
        {},
        TR_GIO_DBUS_PROXY_FLAGS(DO_NOT_LOAD_PROPERTIES));
}

namespace
{

void notify_callback(Glib::RefPtr<Gio::AsyncResult>& res, TrNotification const& n)
{
    auto result = Glib::VariantContainerBase();

    try
    {
        result = proxy->call_finish(res);
    }
    catch (Glib::Error const&)
    {
        return;
    }

    if (!result || result.get_n_children() != 1 || !result.get_child(0).is_of_type(UInt32VariantType::variant_type()))
    {
        return;
    }

    auto const id = Glib::VariantBase::cast_dynamic<UInt32VariantType>(result.get_child(0)).get();

    active_notifications.try_emplace(id, n);
}

} // namespace

void gtr_notify_torrent_completed(Glib::RefPtr<Session> const& core, tr_torrent_id_t tor_id)
{
    if (gtr_pref_flag_get(TR_KEY_torrent_complete_sound_enabled))
    {
        auto const argv = gtr_pref_strv_get(TR_KEY_torrent_complete_sound_command);

        try
        {
            Glib::spawn_async({}, argv, TR_GLIB_SPAWN_FLAGS(SEARCH_PATH));
        }
        catch (Glib::SpawnError const&)
        {
        }
    }

    if (!gtr_pref_flag_get(TR_KEY_torrent_complete_notification_enabled))
    {
        return;
    }

    g_return_if_fail(proxy != nullptr);

    auto const* const tor = core->find_torrent(tor_id);

    auto const n = TrNotification{ core, tor_id };

    std::vector<Glib::ustring> actions;
    if (server_supports_actions)
    {
        if (tr_torrentFileCount(tor) == 1)
        {
            actions.emplace_back("file");
            actions.emplace_back(_("Open File"));
        }
        else
        {
            actions.emplace_back("folder");
            actions.emplace_back(_("Open Folder"));
        }
    }

    std::map<Glib::ustring, Glib::VariantBase> hints;
    hints.try_emplace("category", StringVariantType::create("transfer.complete"));

    proxy->call(
        "Notify",
        [n](auto& res) { notify_callback(res, n); },
        make_variant_tuple(
            Glib::ustring("Transmission"),
            0U,
            Glib::ustring("transmission"),
            Glib::ustring(_("Torrent Complete")),
            Glib::ustring(tr_torrentName(tor)),
            actions,
            hints,
            -1));
}

void gtr_notify_torrent_added(Glib::RefPtr<Session> const& core, tr_torrent_id_t tor_id)
{
    g_return_if_fail(proxy != nullptr);

    if (!gtr_pref_flag_get(TR_KEY_torrent_added_notification_enabled))
    {
        return;
    }

    auto const* const tor = core->find_torrent(tor_id);

    std::vector<Glib::ustring> actions;
    if (server_supports_actions)
    {
        actions.emplace_back("start-now");
        actions.emplace_back(_("Start Now"));
    }

    auto const n = TrNotification{ core, tor_id };

    proxy->call(
        "Notify",
        [n](auto& res) { notify_callback(res, n); },
        make_variant_tuple(
            Glib::ustring("Transmission"),
            0U,
            Glib::ustring("transmission"),
            Glib::ustring(_("Torrent Added")),
            Glib::ustring(tr_torrentName(tor)),
            actions,
            std::map<Glib::ustring, Glib::VariantBase>(),
            -1));
}
