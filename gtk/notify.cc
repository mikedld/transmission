/*
 * This file Copyright (C) 2008-2021 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <map>

#include <giomm.h>
#include <glibmm/i18n.h>

#include "conf.h"
#include "notify.h"
#include "tr-core.h"
#include "tr-prefs.h"
#include "util.h"

#define NOTIFICATIONS_DBUS_NAME "org.freedesktop.Notifications"
#define NOTIFICATIONS_DBUS_CORE_OBJECT "/org/freedesktop/Notifications"
#define NOTIFICATIONS_DBUS_CORE_INTERFACE "org.freedesktop.Notifications"

using StringVariantType = Glib::Variant<Glib::ustring>;
using StringListVariantType = Glib::Variant<std::vector<Glib::ustring>>;
using UInt32VariantType = Glib::Variant<guint32>;

namespace
{

class TrNotification
{
public:
    TrNotification() = default;

    TrNotification(TrCore* core, int torrent_id)
        : core_(core, [](TrCore* c) { g_object_unref(G_OBJECT(c)); })
        , torrent_id_(torrent_id)
    {
        g_object_ref(G_OBJECT(core_.get()));
    }

    TrCore* get_core() const
    {
        return core_.get();
    }

    int get_torrent_id() const
    {
        return torrent_id_;
    }

private:
    std::shared_ptr<TrCore> core_;
    int torrent_id_ = 0;
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
        auto const* tor = gtr_core_find_torrent(n.get_core(), n.get_torrent_id());
        if (tor == nullptr)
        {
            return;
        }

        auto const action = Glib::VariantBase::cast_dynamic<StringVariantType>(params.get_child(1)).get();

        if (action == "folder")
        {
            gtr_core_open_folder(n.get_core(), n.get_torrent_id());
        }
        else if (action == "file")
        {
            auto const* inf = tr_torrentInfo(tor);
            char const* dir = tr_torrentGetDownloadDir(tor);
            auto const path = Glib::build_filename(dir, inf->files[0].name);
            gtr_open_file(path);
        }
    }
}

void dbus_proxy_ready_callback(Glib::RefPtr<Gio::AsyncResult>& res)
{
    proxy = Gio::DBus::Proxy::create_for_bus_finish(res);

    if (proxy == nullptr)
    {
        g_warning("Failed to create proxy for %s", NOTIFICATIONS_DBUS_NAME);
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
        NOTIFICATIONS_DBUS_NAME,
        NOTIFICATIONS_DBUS_CORE_OBJECT,
        NOTIFICATIONS_DBUS_CORE_INTERFACE,
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

    active_notifications.emplace(id, n);
}

} // namespace

void gtr_notify_torrent_completed(TrCore* core, int torrent_id)
{
    if (gtr_pref_flag_get(TR_KEY_torrent_complete_sound_enabled))
    {
        auto const argv = Glib::ArrayHandle<std::string>(
            gtr_pref_strv_get(TR_KEY_torrent_complete_sound_command),
            Glib::OWNERSHIP_DEEP);

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

    auto const* const tor = gtr_core_find_torrent(core, torrent_id);

    auto const n = TrNotification(core, torrent_id);

    std::vector<Glib::ustring> actions;
    if (server_supports_actions)
    {
        auto const* inf = tr_torrentInfo(tor);

        if (inf->fileCount == 1)
        {
            actions.push_back("file");
            actions.push_back(_("Open File"));
        }
        else
        {
            actions.push_back("folder");
            actions.push_back(_("Open Folder"));
        }
    }

    std::map<Glib::ustring, Glib::VariantBase> hints;
    hints.emplace("category", StringVariantType::create("transfer.complete"));

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

void gtr_notify_torrent_added(Glib::ustring const& name)
{
    g_return_if_fail(proxy != nullptr);

    if (!gtr_pref_flag_get(TR_KEY_torrent_added_notification_enabled))
    {
        return;
    }

    proxy->call(
        "Notify",
        [](auto& res) { notify_callback(res, {}); },
        make_variant_tuple(
            Glib::ustring("Transmission"),
            0u,
            Glib::ustring("transmission"),
            Glib::ustring(_("Torrent Added")),
            name,
            std::vector<Glib::ustring>(),
            std::map<Glib::ustring, Glib::VariantBase>(),
            -1));
}
