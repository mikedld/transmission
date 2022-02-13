// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <map>

#include <giomm.h>
#include <glibmm/i18n.h>

#include "Notify.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

using namespace std::literals;

using StringVariantType = Glib::Variant<Glib::ustring>;
using StringListVariantType = Glib::Variant<std::vector<Glib::ustring>>;
using UInt32VariantType = Glib::Variant<guint32>;

namespace
{

auto const NotificationsDbusName = Glib::ustring("org.freedesktop.Notifications"s);
auto const NotificationsDbusCoreObject = Glib::ustring("/org/freedesktop/Notifications"s);
auto const NotificationsDbusCoreInterface = Glib::ustring("org.freedesktop.Notifications"s);

struct TrNotification
{
    Glib::RefPtr<Session> core;
    int torrent_id = 0;
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
    auto result = proxy->call_finish(res);

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
    proxy = Gio::DBus::Proxy::create_for_bus_finish(res);

    if (proxy == nullptr)
    {
        g_warning("Failed to create proxy for %s", NotificationsDbusName.c_str());
        return;
    }

    proxy->signal_signal().connect(&g_signal_callback);
    proxy->call("GetCapabilities", &get_capabilities_callback);
}

} // namespace

void gtr_notify_init()
{
    Gio::DBus::Proxy::create_for_bus(
        Gio::DBus::BUS_TYPE_SESSION,
        NotificationsDbusName,
        NotificationsDbusCoreObject,
        NotificationsDbusCoreInterface,
        &dbus_proxy_ready_callback,
        {},
        Gio::DBus::PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES);
}

namespace
{

void notify_callback(Glib::RefPtr<Gio::AsyncResult>& res, TrNotification const& n)
{
    auto result = proxy->call_finish(res);

    if (!result || result.get_n_children() != 1 || !result.get_child(0).is_of_type(UInt32VariantType::variant_type()))
    {
        return;
    }

    auto const id = Glib::VariantBase::cast_dynamic<UInt32VariantType>(result.get_child(0)).get();

    active_notifications.try_emplace(id, n);
}

} // namespace

void gtr_notify_torrent_completed(Glib::RefPtr<Session> const& core, int torrent_id)
{
    if (gtr_pref_flag_get(TR_KEY_torrent_complete_sound_enabled))
    {
        auto const argv = gtr_pref_strv_get(TR_KEY_torrent_complete_sound_command);

        try
        {
            Glib::spawn_async({}, argv, Glib::SPAWN_SEARCH_PATH);
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

    auto const* const tor = core->find_torrent(torrent_id);

    auto const n = TrNotification{ core, torrent_id };

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
            0u,
            Glib::ustring("transmission"),
            Glib::ustring(_("Torrent Complete")),
            Glib::ustring(tr_torrentName(tor)),
            actions,
            hints,
            -1));
}

void gtr_notify_torrent_added(Glib::RefPtr<Session> const& core, int torrent_id)
{
    g_return_if_fail(proxy != nullptr);

    if (!gtr_pref_flag_get(TR_KEY_torrent_added_notification_enabled))
    {
        return;
    }

    auto const* const tor = core->find_torrent(torrent_id);

    std::vector<Glib::ustring> actions;
    if (server_supports_actions)
    {
        actions.emplace_back("start-now");
        actions.emplace_back(_("Start Now"));
    }

    auto const n = TrNotification{ core, torrent_id };

    proxy->call(
        "Notify",
        [n](auto& res) { notify_callback(res, n); },
        make_variant_tuple(
            Glib::ustring("Transmission"),
            0u,
            Glib::ustring("transmission"),
            Glib::ustring(_("Torrent Added")),
            Glib::ustring(tr_torrentName(tor)),
            actions,
            std::map<Glib::ustring, Glib::VariantBase>(),
            -1));
}
