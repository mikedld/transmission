// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "PrefsDialog.h"

#include "FreeSpaceLabel.h"
#include "GtkCompat.h"
#include "PathButton.h"
#include "Prefs.h"
#include "Session.h"
#include "SystemTrayIcon.h"
#include "Utils.h"

#include <libtransmission/transmission.h>
#include <libtransmission/web-utils.h>

#include <glibmm/date.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/timer.h>
#include <glibmm/ustring.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/cellrenderertext.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/combobox.h>
#include <gtkmm/editable.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/liststore.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/textview.h>
#include <gtkmm/treemodelcolumn.h>
#include <gtkmm/widget.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gtkmm/eventcontrollerfocus.h>
#endif

#include <fmt/core.h>

#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

using namespace libtransmission::Values;

/**
***
**/

class PrefsDialog::Impl
{
public:
    Impl(PrefsDialog& dialog, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~Impl() = default;

    TR_DISABLE_COPY_MOVE(Impl)

private:
    void response_cb(int response);

private:
    PrefsDialog& dialog_;
    Glib::RefPtr<Session> const core_;
};

/**
***
**/

void PrefsDialog::Impl::response_cb(int response)
{
    if (response == TR_GTK_RESPONSE_TYPE(HELP))
    {
        gtr_open_uri(gtr_get_help_uri() + "/html/preferences.html");
    }

    if (response == TR_GTK_RESPONSE_TYPE(CLOSE))
    {
        dialog_.close();
    }
}

namespace
{

class PageBase : public Gtk::Box
{
public:
    PageBase(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~PageBase() override;

    TR_DISABLE_COPY_MOVE(PageBase)

    Gtk::CheckButton* init_check_button(Glib::ustring const& name, tr_quark key);
    Gtk::SpinButton* init_spin_button(Glib::ustring const& name, tr_quark key, int low, int high, int step);
    Gtk::SpinButton* init_spin_button_double(Glib::ustring const& name, tr_quark key, double low, double high, double step);
    Gtk::Entry* init_entry(Glib::ustring const& name, tr_quark key);
    Gtk::TextView* init_text_view(Glib::ustring const& name, tr_quark key);
    PathButton* init_chooser_button(Glib::ustring const& name, tr_quark key);
    Gtk::ComboBox* init_encryption_combo(Glib::ustring const& name, tr_quark key);
    Gtk::ComboBox* init_time_combo(Glib::ustring const& name, tr_quark key);
    Gtk::ComboBox* init_week_combo(Glib::ustring const& name, tr_quark key);

    template<typename T>
    T* get_widget(Glib::ustring const& name) const
    {
        return gtr_get_widget<T>(builder_, name);
    }

    template<typename T, typename... ArgTs>
    T* get_widget_derived(Glib::ustring const& name, ArgTs&&... args) const
    {
        return gtr_get_widget_derived<T>(builder_, name, std::forward<ArgTs>(args)...);
    }

    template<typename T, typename... ArgTs>
    static void localize_label(T& widget, ArgTs&&... args)
    {
        widget.set_label(fmt::format(widget.get_label().raw(), std::forward<ArgTs>(args)...));
    }

private:
    bool spun_cb_idle(Gtk::SpinButton& spin, tr_quark key, bool isDouble);
    void spun_cb(Gtk::SpinButton& w, tr_quark key, bool isDouble);

    void entry_changed_cb(Gtk::Entry& w, tr_quark key);

    void chosen_cb(PathButton& w, tr_quark key);

    void onIntComboChanged(Gtk::ComboBox& combo_box, tr_quark key);

    static auto get_weekday_string(Glib::Date::Weekday weekday);

private:
    Glib::RefPtr<Gtk::Builder> const builder_;
    Glib::RefPtr<Session> const core_;

    std::map<tr_quark, std::pair<std::unique_ptr<Glib::Timer>, sigc::connection>> spin_timers_;
};

PageBase::PageBase(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core)
    : Gtk::Box(cast_item)
    , builder_(builder)
    , core_(core)
{
}

PageBase::~PageBase()
{
    for (auto& [key, info] : spin_timers_)
    {
        info.second.disconnect();
    }
}

Gtk::CheckButton* PageBase::init_check_button(Glib::ustring const& name, tr_quark const key)
{
    auto* button = get_widget<Gtk::CheckButton>(name);
    button->set_active(gtr_pref_flag_get(key));
    button->signal_toggled().connect([this, button, key]() { core_->set_pref(key, button->get_active()); });
    return button;
}

bool PageBase::spun_cb_idle(Gtk::SpinButton& spin, tr_quark const key, bool isDouble)
{
    auto const last_change_it = spin_timers_.find(key);
    g_assert(last_change_it != spin_timers_.end());

    /* has the user stopped making changes? */
    if (last_change_it->second.first->elapsed() < 0.33)
    {
        return true;
    }

    /* update the core */
    if (isDouble)
    {
        core_->set_pref(key, spin.get_value());
    }
    else
    {
        core_->set_pref(key, spin.get_value_as_int());
    }

    /* cleanup */
    spin_timers_.erase(last_change_it);
    return false;
}

void PageBase::spun_cb(Gtk::SpinButton& w, tr_quark const key, bool isDouble)
{
    /* user may be spinning through many values, so let's hold off
       for a moment to keep from flooding the core with changes */
    auto last_change_it = spin_timers_.find(key);
    if (last_change_it == spin_timers_.end())
    {
        auto timeout_tag = Glib::signal_timeout().connect_seconds(
            [this, &w, key, isDouble]() { return spun_cb_idle(w, key, isDouble); },
            1);
        last_change_it = spin_timers_.emplace(key, std::pair(std::make_unique<Glib::Timer>(), timeout_tag)).first;
    }

    last_change_it->second.first->start();
}

Gtk::SpinButton* PageBase::init_spin_button(Glib::ustring const& name, tr_quark const key, int low, int high, int step)
{
    auto* button = get_widget<Gtk::SpinButton>(name);
    button->set_adjustment(Gtk::Adjustment::create(gtr_pref_int_get(key), low, high, step));
    button->set_digits(0);
    button->signal_value_changed().connect([this, button, key]() { spun_cb(*button, key, false); });
    return button;
}

Gtk::SpinButton* PageBase::init_spin_button_double(
    Glib::ustring const& name,
    tr_quark const key,
    double low,
    double high,
    double step)
{
    auto* button = get_widget<Gtk::SpinButton>(name);
    button->set_adjustment(Gtk::Adjustment::create(gtr_pref_double_get(key), low, high, step));
    button->set_digits(2);
    button->signal_value_changed().connect([this, button, key]() { spun_cb(*button, key, true); });
    return button;
}

void PageBase::entry_changed_cb(Gtk::Entry& w, tr_quark const key)
{
    core_->set_pref(key, w.get_text());
}

Gtk::Entry* PageBase::init_entry(Glib::ustring const& name, tr_quark const key)
{
    auto* const entry = get_widget<Gtk::Entry>(name);

    if (auto const value = gtr_pref_string_get(key); !value.empty())
    {
        entry->set_text(value);
    }

    entry->signal_changed().connect([this, entry, key]() { entry_changed_cb(*entry, key); });
    return entry;
}

Gtk::TextView* PageBase::init_text_view(Glib::ustring const& name, tr_quark const key)
{
    auto* const view = get_widget<Gtk::TextView>(name);

    auto buffer = view->get_buffer();
    buffer->set_text(gtr_pref_string_get(key));

    auto const save_buffer = [this, buffer, key]()
    {
        core_->set_pref(key, buffer->get_text());
    };

#if GTKMM_CHECK_VERSION(4, 0, 0)
    auto focus_controller = Gtk::EventControllerFocus::create();
    focus_controller->signal_leave().connect(save_buffer);
    view->add_controller(focus_controller);
#else
    view->add_events(Gdk::FOCUS_CHANGE_MASK);
    view->signal_focus_out_event().connect_notify(sigc::hide<0>(save_buffer));
#endif

    return view;
}

void PageBase::chosen_cb(PathButton& w, tr_quark const key)
{
    core_->set_pref(key, w.get_filename());
}

PathButton* PageBase::init_chooser_button(Glib::ustring const& name, tr_quark const key)
{
    auto* const button = get_widget_derived<PathButton>(name);

    if (auto const path = gtr_pref_string_get(key); !path.empty())
    {
        button->set_filename(path);
    }

    button->signal_selection_changed().connect([this, button, key]() { chosen_cb(*button, key); });
    return button;
}

void PageBase::onIntComboChanged(Gtk::ComboBox& combo_box, tr_quark const key)
{
    core_->set_pref(key, gtr_combo_box_get_active_enum(combo_box));
}

Gtk::ComboBox* PageBase::init_encryption_combo(Glib::ustring const& name, tr_quark const key)
{
    auto* const combo = get_widget<Gtk::ComboBox>(name);
    gtr_combo_box_set_enum(
        *combo,
        {
            { _("Allow encryption"), TR_CLEAR_PREFERRED },
            { _("Prefer encryption"), TR_ENCRYPTION_PREFERRED },
            { _("Require encryption"), TR_ENCRYPTION_REQUIRED },
        });
    gtr_combo_box_set_active_enum(*combo, gtr_pref_int_get(key));
    combo->signal_changed().connect([this, combo, key]() { onIntComboChanged(*combo, key); });
    return combo;
}

Gtk::ComboBox* PageBase::init_time_combo(Glib::ustring const& name, tr_quark const key)
{
    class TimeModelColumns : public Gtk::TreeModelColumnRecord
    {
    public:
        TimeModelColumns() noexcept
        {
            add(offset);
            add(title);
        }

        Gtk::TreeModelColumn<int> offset;
        Gtk::TreeModelColumn<Glib::ustring> title;
    };

    static TimeModelColumns const time_cols;

    /* build a store at 15 minute intervals */
    auto store = Gtk::ListStore::create(time_cols);

    for (int i = 0; i < 60 * 24; i += 15)
    {
        auto const iter = store->append();
        (*iter)[time_cols.offset] = i;
        (*iter)[time_cols.title] = fmt::format("{:02}:{:02}", i / 60, i % 60);
    }

    /* build the widget */
    auto* const combo = get_widget<Gtk::ComboBox>(name);
    combo->set_model(store);
    auto* r = Gtk::make_managed<Gtk::CellRendererText>();
    combo->pack_start(*r, true);
    combo->add_attribute(r->property_text(), time_cols.title);
    combo->set_active(gtr_pref_int_get(key) / 15);
    combo->signal_changed().connect(
        [this, combo, key]()
        {
            if (auto const iter = combo->get_active(); iter)
            {
                core_->set_pref(key, iter->get_value(time_cols.offset));
            }
        });
    return combo;
}

auto PageBase::get_weekday_string(Glib::Date::Weekday weekday)
{
    auto date = Glib::Date{};
    date.set_time_current();
    date.add_days(static_cast<int>(weekday) - static_cast<int>(date.get_weekday()));
    return date.format_string("%A");
}

Gtk::ComboBox* PageBase::init_week_combo(Glib::ustring const& name, tr_quark const key)
{
    auto* const combo = get_widget<Gtk::ComboBox>(name);
    gtr_combo_box_set_enum(
        *combo,
        {
            { _("Every Day"), TR_SCHED_ALL },
            { _("Weekdays"), TR_SCHED_WEEKDAY },
            { _("Weekends"), TR_SCHED_WEEKEND },
            { get_weekday_string(Glib::Date::Weekday::MONDAY), TR_SCHED_MON },
            { get_weekday_string(Glib::Date::Weekday::TUESDAY), TR_SCHED_TUES },
            { get_weekday_string(Glib::Date::Weekday::WEDNESDAY), TR_SCHED_WED },
            { get_weekday_string(Glib::Date::Weekday::THURSDAY), TR_SCHED_THURS },
            { get_weekday_string(Glib::Date::Weekday::FRIDAY), TR_SCHED_FRI },
            { get_weekday_string(Glib::Date::Weekday::SATURDAY), TR_SCHED_SAT },
            { get_weekday_string(Glib::Date::Weekday::SUNDAY), TR_SCHED_SUN },
        });
    gtr_combo_box_set_active_enum(*combo, gtr_pref_int_get(key));
    combo->signal_changed().connect([this, combo, key]() { onIntComboChanged(*combo, key); });
    return combo;
}

/****
*****  Download Tab
****/

class DownloadingPage : public PageBase
{
public:
    DownloadingPage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~DownloadingPage() override;

    TR_DISABLE_COPY_MOVE(DownloadingPage)

private:
    void on_core_prefs_changed(tr_quark key);

private:
    Glib::RefPtr<Session> const core_;

    FreeSpaceLabel* freespace_label_ = nullptr;

    sigc::connection core_prefs_tag_;
};

void DownloadingPage::on_core_prefs_changed(tr_quark const key)
{
    if (key == TR_KEY_download_dir)
    {
        char const* downloadDir = tr_sessionGetDownloadDir(core_->get_session());
        freespace_label_->set_dir(downloadDir);
    }
}

DownloadingPage::~DownloadingPage()
{
    core_prefs_tag_.disconnect();
}

DownloadingPage::DownloadingPage(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Session> const& core)
    : PageBase(cast_item, builder, core)
    , core_(core)
    , freespace_label_(get_widget_derived<FreeSpaceLabel>("download_dir_stats_label", core))
{
    core_prefs_tag_ = core_->signal_prefs_changed().connect(sigc::mem_fun(*this, &DownloadingPage::on_core_prefs_changed));

    init_check_button("watch_dir_check", TR_KEY_watch_dir_enabled);
    init_chooser_button("watch_dir_chooser", TR_KEY_watch_dir);
    init_check_button("show_options_dialog_check", TR_KEY_show_options_window);
    init_check_button("start_on_add_check", TR_KEY_start_added_torrents);
    init_check_button("trash_on_add_check", TR_KEY_trash_original_torrent_files);
    init_chooser_button("download_dir_chooser", TR_KEY_download_dir);
    init_spin_button("max_active_downloads_spin", TR_KEY_download_queue_size, 0, std::numeric_limits<int>::max(), 1);
    init_spin_button("max_inactive_time_spin", TR_KEY_queue_stalled_minutes, 1, std::numeric_limits<int>::max(), 15);
    init_check_button("append_suffix_to_incomplete_check", TR_KEY_rename_partial_files);
    init_check_button("incomplete_dir_check", TR_KEY_incomplete_dir_enabled);
    init_chooser_button("incomplete_dir_chooser", TR_KEY_incomplete_dir);
    init_check_button("download_done_script_check", TR_KEY_script_torrent_done_enabled);
    init_chooser_button("download_done_script_chooser", TR_KEY_script_torrent_done_filename);

    on_core_prefs_changed(TR_KEY_download_dir);
}

/****
*****  Torrent Tab
****/

class SeedingPage : public PageBase
{
public:
    SeedingPage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~SeedingPage() override = default;

    TR_DISABLE_COPY_MOVE(SeedingPage)
};

SeedingPage::SeedingPage(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Session> const& core)
    : PageBase(cast_item, builder, core)
{
    init_check_button("stop_seeding_ratio_check", TR_KEY_ratio_limit_enabled);
    init_spin_button_double("stop_seeding_ratio_spin", TR_KEY_ratio_limit, 0, 1000, 0.05);
    init_check_button("stop_seeding_timeout_check", TR_KEY_idle_seeding_limit_enabled);
    init_spin_button("stop_seeding_timeout_spin", TR_KEY_idle_seeding_limit, 1, 40320, 5);
    init_check_button("seeding_done_script_check", TR_KEY_script_torrent_done_seeding_enabled);
    init_chooser_button("seeding_done_script_chooser", TR_KEY_script_torrent_done_seeding_filename);
}

/****
*****  Desktop Tab
****/

class DesktopPage : public PageBase
{
public:
    DesktopPage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~DesktopPage() override = default;

    TR_DISABLE_COPY_MOVE(DesktopPage)
};

DesktopPage::DesktopPage(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Session> const& core)
    : PageBase(cast_item, builder, core)
{
    init_check_button("inhibit_hibernation_check", TR_KEY_inhibit_desktop_hibernation);

    if (SystemTrayIcon::is_available())
    {
        init_check_button("show_systray_icon_check", TR_KEY_show_notification_area_icon);
    }
    else
    {
        get_widget<Gtk::CheckButton>("show_systray_icon_check")->hide();
    }

    init_check_button("notify_on_torrent_add_check", TR_KEY_torrent_added_notification_enabled);
    init_check_button("notify_on_torrent_finish_check", TR_KEY_torrent_complete_notification_enabled);
    init_check_button("ding_no_torrent_finish_check", TR_KEY_torrent_complete_sound_enabled);
}

/****
*****  Peer Tab
****/

class PrivacyPage : public PageBase
{
    static auto const BlocklistUpdateResultDisplayTimeoutInSeconds = 3U;

public:
    PrivacyPage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~PrivacyPage() override;

    TR_DISABLE_COPY_MOVE(PrivacyPage)

private:
    void updateBlocklistText();
    void onBlocklistUpdated(bool success);
    void onBlocklistUpdate();
    void on_blocklist_url_changed(Gtk::Editable* e);

private:
    Glib::RefPtr<Session> core_;

    Gtk::Button* updateBlocklistButton_ = nullptr;
    Gtk::Label* label_ = nullptr;
    Gtk::CheckButton* check_ = nullptr;

    sigc::connection updateBlocklistTag_;
    sigc::connection blocklist_update_result_tag_;
};

void PrivacyPage::updateBlocklistText()
{
    int const n = tr_blocklistGetRuleCount(core_->get_session());
    auto const msg = fmt::format(
        ngettext("Blocklist has {count:L} entry", "Blocklist has {count:L} entries", n),
        fmt::arg("count", n));
    label_->set_text(msg);
}

/* prefs dialog is being destroyed, so stop listening to blocklist updates */
PrivacyPage::~PrivacyPage()
{
    blocklist_update_result_tag_.disconnect();
    updateBlocklistTag_.disconnect();
}

/* core says the blocklist was updated */
void PrivacyPage::onBlocklistUpdated(bool success)
{
    updateBlocklistButton_->set_sensitive(true);
    label_->set_text(success ? _("Blocklist updated!") : _("Couldn't update blocklist"));

    blocklist_update_result_tag_ = Glib::signal_timeout().connect_seconds(
        sigc::bind_return(sigc::mem_fun(*this, &PrivacyPage::updateBlocklistText), false),
        BlocklistUpdateResultDisplayTimeoutInSeconds);
}

/* user pushed a button to update the blocklist */
void PrivacyPage::onBlocklistUpdate()
{
    updateBlocklistButton_->set_sensitive(false);

    label_->set_text(_("Getting new blocklist…"));
    blocklist_update_result_tag_.disconnect();

    core_->blocklist_update();
}

void PrivacyPage::on_blocklist_url_changed(Gtk::Editable* e)
{
    auto const url = e->get_chars(0, -1);
    updateBlocklistButton_->set_sensitive(tr_urlIsValid(url.c_str()));
}

PrivacyPage::PrivacyPage(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Session> const& core)
    : PageBase(cast_item, builder, core)
    , core_(core)
    , updateBlocklistButton_(get_widget<Gtk::Button>("update_blocklist_button"))
    , label_(get_widget<Gtk::Label>("blocklist_stats_label"))
    , check_(init_check_button("blocklist_check", TR_KEY_blocklist_enabled))
{
    init_encryption_combo("encryption_mode_combo", TR_KEY_encryption);

    auto* const blocklist_url_entry = init_entry("blocklist_url_entry", TR_KEY_blocklist_url);

    updateBlocklistText();
    updateBlocklistButton_->signal_clicked().connect([this]() { onBlocklistUpdate(); });
    updateBlocklistButton_->set_sensitive(check_->get_active());
    blocklist_url_entry->signal_changed().connect([this, blocklist_url_entry]()
                                                  { on_blocklist_url_changed(blocklist_url_entry); });
    on_blocklist_url_changed(blocklist_url_entry);

    init_check_button("blocklist_autoupdate_check", TR_KEY_blocklist_updates_enabled);

    updateBlocklistTag_ = core_->signal_blocklist_updated().connect(sigc::mem_fun(*this, &PrivacyPage::onBlocklistUpdated));
}

/****
*****  Remote Tab
****/

class RemotePage : public PageBase
{
    class WhitelistModelColumns : public Gtk::TreeModelColumnRecord
    {
    public:
        WhitelistModelColumns() noexcept
        {
            add(address);
        }

        Gtk::TreeModelColumn<Glib::ustring> address;
    };

    static WhitelistModelColumns const whitelist_cols;

public:
    RemotePage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~RemotePage() override = default;

    TR_DISABLE_COPY_MOVE(RemotePage)

private:
    void refreshWhitelist();
    void onAddressEdited(Glib::ustring const& path, Glib::ustring const& address);
    void onAddWhitelistClicked();
    void onRemoveWhitelistClicked();
    void refreshRPCSensitivity();

    static void onLaunchClutchCB();

    static Glib::RefPtr<Gtk::ListStore> whitelist_tree_model_new(std::string const& whitelist);

private:
    Glib::RefPtr<Session> core_;

    Gtk::TreeView* view_;
    Gtk::Button* remove_button_;
    Gtk::CheckButton* rpc_tb_;
    Gtk::CheckButton* auth_tb_;
    Gtk::CheckButton* whitelist_tb_;

    Glib::RefPtr<Gtk::ListStore> store_;
    std::vector<Gtk::Widget*> auth_widgets_;
    std::vector<Gtk::Widget*> whitelist_widgets_;
};

RemotePage::WhitelistModelColumns const RemotePage::whitelist_cols;

Glib::RefPtr<Gtk::ListStore> RemotePage::whitelist_tree_model_new(std::string const& whitelist)
{
    auto store = Gtk::ListStore::create(whitelist_cols);

    std::istringstream stream(whitelist);
    std::string s;

    while (std::getline(stream, s, ','))
    {
        s = gtr_str_strip(s);

        if (s.empty())
        {
            continue;
        }

        auto const iter = store->append();
        (*iter)[whitelist_cols.address] = s;
    }

    return store;
}

void RemotePage::refreshWhitelist()
{
    std::ostringstream gstr;

    for (auto const& row : store_->children())
    {
        gstr << row.get_value(whitelist_cols.address) << ",";
    }

    auto str = gstr.str();
    if (!str.empty())
    {
        str.resize(str.size() - 1); /* remove the trailing comma */
    }

    core_->set_pref(TR_KEY_rpc_whitelist, str);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void RemotePage::onAddressEdited(Glib::ustring const& path, Glib::ustring const& address)
{
    if (auto const iter = store_->get_iter(path); iter)
    {
        (*iter)[whitelist_cols.address] = address;
    }

    refreshWhitelist();
}

void RemotePage::onAddWhitelistClicked()
{
    auto const iter = store_->append();
    (*iter)[whitelist_cols.address] = "0.0.0.0";

    view_->set_cursor(store_->get_path(iter), *view_->get_column(0), true);
}

void RemotePage::onRemoveWhitelistClicked()
{
    auto const sel = view_->get_selection();

    if (auto const iter = sel->get_selected(); iter)
    {
        store_->erase(iter);
        refreshWhitelist();
    }
}

void RemotePage::refreshRPCSensitivity()
{
    bool const rpc_active = rpc_tb_->get_active();
    bool const auth_active = auth_tb_->get_active();
    bool const whitelist_active = whitelist_tb_->get_active();
    auto const sel = view_->get_selection();
    auto const have_addr = sel->get_selected();
    auto const n_rules = store_->children().size();

    for (auto* const widget : auth_widgets_)
    {
        widget->set_sensitive(rpc_active && auth_active);
    }

    for (auto* const widget : whitelist_widgets_)
    {
        widget->set_sensitive(rpc_active && whitelist_active);
    }

    remove_button_->set_sensitive(rpc_active && whitelist_active && have_addr && n_rules > 1);
}

void RemotePage::onLaunchClutchCB()
{
    gtr_open_uri(fmt::format("http://localhost:{}/", gtr_pref_int_get(TR_KEY_rpc_port)));
}

RemotePage::RemotePage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core)
    : PageBase(cast_item, builder, core)
    , core_(core)
    , view_(get_widget<Gtk::TreeView>("rpc_whitelist_view"))
    , remove_button_(get_widget<Gtk::Button>("remove_from_rpc_whistlist_button"))
    , rpc_tb_(init_check_button("enable_rpc_check", TR_KEY_rpc_enabled))
    , auth_tb_(init_check_button("enable_rpc_auth_check", TR_KEY_rpc_authentication_required))
    , whitelist_tb_(init_check_button("rpc_whitelist_check", TR_KEY_rpc_whitelist_enabled))
{
    rpc_tb_->signal_toggled().connect([this]() { refreshRPCSensitivity(); });
    auto* const open_button = get_widget<Gtk::Button>("open_web_client_button");
    open_button->signal_clicked().connect(&RemotePage::onLaunchClutchCB);

    init_spin_button("rpc_port_spin", TR_KEY_rpc_port, 0, std::numeric_limits<uint16_t>::max(), 1);

    auth_tb_->signal_toggled().connect([this]() { refreshRPCSensitivity(); });

    auto* const username_entry = init_entry("rpc_username_entry", TR_KEY_rpc_username);
    auth_widgets_.push_back(username_entry);
    auth_widgets_.push_back(get_widget<Gtk::Label>("rpc_username_label"));

    auto* const password_entry = init_entry("rpc_password_entry", TR_KEY_rpc_password);
    auth_widgets_.push_back(password_entry);
    auth_widgets_.push_back(get_widget<Gtk::Label>("rpc_password_label"));

    whitelist_tb_->signal_toggled().connect([this]() { refreshRPCSensitivity(); });

    {
        store_ = whitelist_tree_model_new(gtr_pref_string_get(TR_KEY_rpc_whitelist));

        view_->set_model(store_);
        setup_item_view_button_event_handling(
            *view_,
            {},
            [this](double view_x, double view_y) { return on_item_view_button_released(*view_, view_x, view_y); });

        whitelist_widgets_.push_back(view_);
        auto const sel = view_->get_selection();
        sel->signal_changed().connect([this]() { refreshRPCSensitivity(); });

        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        r->signal_edited().connect(sigc::mem_fun(*this, &RemotePage::onAddressEdited));
        r->property_editable() = true;
        auto* c = Gtk::make_managed<Gtk::TreeViewColumn>("", *r);
        c->add_attribute(r->property_text(), whitelist_cols.address);
        c->set_expand(true);
        view_->append_column(*c);

        whitelist_widgets_.push_back(get_widget<Gtk::Label>("rpc_whitelist_label"));

        remove_button_->signal_clicked().connect([this]() { onRemoveWhitelistClicked(); });
        refreshRPCSensitivity();
        auto* add_button = get_widget<Gtk::Button>("add_to_rpc_whitelist_button");
        whitelist_widgets_.push_back(add_button);
        add_button->signal_clicked().connect([this]() { onAddWhitelistClicked(); });
    }

    refreshRPCSensitivity();
}

/****
*****  Bandwidth Tab
****/

class SpeedPage : public PageBase
{
public:
    SpeedPage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~SpeedPage() override = default;

    TR_DISABLE_COPY_MOVE(SpeedPage)
};

SpeedPage::SpeedPage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core)
    : PageBase(cast_item, builder, core)
{
    auto const speed_units_kbyps_str = Speed::units().display_name(Speed::Units::KByps);

    localize_label(
        *init_check_button("upload_limit_check", TR_KEY_speed_limit_up_enabled),
        fmt::arg("speed_units", speed_units_kbyps_str));
    init_spin_button("upload_limit_spin", TR_KEY_speed_limit_up, 0, std::numeric_limits<int>::max(), 5);

    localize_label(
        *init_check_button("download_limit_check", TR_KEY_speed_limit_down_enabled),
        fmt::arg("speed_units", speed_units_kbyps_str));
    init_spin_button("download_limit_spin", TR_KEY_speed_limit_down, 0, std::numeric_limits<int>::max(), 5);

    localize_label(*get_widget<Gtk::Label>("alt_upload_limit_label"), fmt::arg("speed_units", speed_units_kbyps_str));
    init_spin_button("alt_upload_limit_spin", TR_KEY_alt_speed_up, 0, std::numeric_limits<int>::max(), 5);

    localize_label(*get_widget<Gtk::Label>("alt_download_limit_label"), fmt::arg("speed_units", speed_units_kbyps_str));
    init_spin_button("alt_download_limit_spin", TR_KEY_alt_speed_down, 0, std::numeric_limits<int>::max(), 5);

    init_time_combo("alt_speed_start_time_combo", TR_KEY_alt_speed_time_begin);
    init_time_combo("alt_speed_end_time_combo", TR_KEY_alt_speed_time_end);
    init_check_button("alt_schedule_time_check", TR_KEY_alt_speed_time_enabled);
    init_week_combo("alt_speed_days_combo", TR_KEY_alt_speed_time_day);
}

/****
*****  Network Tab
****/

class NetworkPage : public PageBase
{
public:
    NetworkPage(BaseObjectType* cast_item, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~NetworkPage() override;

    TR_DISABLE_COPY_MOVE(NetworkPage)

private:
    enum PortTestStatus : uint8_t
    {
        PORT_TEST_UNKNOWN = 0U,
        PORT_TEST_CHECKING,
        PORT_TEST_OPEN,
        PORT_TEST_CLOSED,
        PORT_TEST_ERROR
    };

    void portTestSetSensitive();
    void updatePortStatusText();
    void onCorePrefsChanged(tr_quark key);
    void onPortTested(std::optional<bool> result, Session::PortTestIpProtocol ip_protocol);
    void onPortTest();

    static std::string_view getPortStatusText(PortTestStatus status) noexcept;

private:
    Glib::RefPtr<Session> core_;

    Gtk::Label* portLabel_ = nullptr;
    Gtk::Button* portButton_ = nullptr;
    Gtk::SpinButton* portSpin_ = nullptr;

    sigc::connection portTag_;
    sigc::connection prefsTag_;

    std::array<PortTestStatus, Session::NUM_PORT_TEST_IP_PROTOCOL> portTestStatus_ = {};
};

void NetworkPage::onCorePrefsChanged(tr_quark const key)
{
    if (key == TR_KEY_peer_port)
    {
        portTestStatus_[Session::PORT_TEST_IPV4] = PORT_TEST_UNKNOWN;
        portTestStatus_[Session::PORT_TEST_IPV6] = PORT_TEST_UNKNOWN;
        updatePortStatusText();
        portTestSetSensitive();
    }
}

NetworkPage::~NetworkPage()
{
    prefsTag_.disconnect();
    portTag_.disconnect();
}

std::string_view NetworkPage::getPortStatusText(PortTestStatus const status) noexcept
{
    switch (status)
    {
    case PORT_TEST_UNKNOWN:
        return C_("Port test status", "unknown");
    case PORT_TEST_CHECKING:
        return C_("Port test status", "checking…");
    case PORT_TEST_OPEN:
        return C_("Port test status", "open");
    case PORT_TEST_CLOSED:
        return C_("Port test status", "closed");
    case PORT_TEST_ERROR:
        return C_("Port test status", "error");
    default:
        return {};
    }
}

void NetworkPage::updatePortStatusText()
{
    auto const status_ipv4 = getPortStatusText(portTestStatus_[Session::PORT_TEST_IPV4]);
    auto const status_ipv6 = getPortStatusText(portTestStatus_[Session::PORT_TEST_IPV6]);

    portLabel_->set_markup(
        portTestStatus_[Session::PORT_TEST_IPV4] == portTestStatus_[Session::PORT_TEST_IPV6] ?
            fmt::format(_("Status: <b>{status}</b>"), fmt::arg("status", status_ipv4)) :
            fmt::format(
                _("Status: <b>{status_ipv4}</b> (IPv4), <b>{status_ipv6}</b> (IPv6)"),
                fmt::arg("status_ipv4", status_ipv4),
                fmt::arg("status_ipv6", status_ipv6)));
}

void NetworkPage::portTestSetSensitive()
{
    // Depend on the RPC call status instead of the UI status, so that the widgets
    // won't be enabled even if the port peer port changed while we have port_test
    // RPC call(s) in-flight.
    auto const sensitive = !core_->port_test_pending(Session::PORT_TEST_IPV4) &&
        !core_->port_test_pending(Session::PORT_TEST_IPV6);
    portButton_->set_sensitive(sensitive);
    portSpin_->set_sensitive(sensitive);
}

void NetworkPage::onPortTested(std::optional<bool> const result, Session::PortTestIpProtocol const ip_protocol)
{
    auto constexpr ResultToStatus = [](std::optional<bool> const res)
    {
        if (!res)
        {
            return PORT_TEST_ERROR;
        }
        if (!*res)
        {
            return PORT_TEST_CLOSED;
        }
        return PORT_TEST_OPEN;
    };

    // Only update the UI if the current status is "checking", so that
    // we won't show the port test results for the old peer port if it
    // changed while we have port_test RPC call(s) in-flight.
    if (auto& status = portTestStatus_[ip_protocol]; status == PORT_TEST_CHECKING)
    {
        status = ResultToStatus(result);
        updatePortStatusText();
    }
    portTestSetSensitive();
}

void NetworkPage::onPortTest()
{
    portTestStatus_[Session::PORT_TEST_IPV4] = PORT_TEST_CHECKING;
    portTestStatus_[Session::PORT_TEST_IPV6] = PORT_TEST_CHECKING;
    updatePortStatusText();

    if (!portTag_.connected())
    {
        portTag_ = core_->signal_port_tested().connect(
            [this](std::optional<bool> status, Session::PortTestIpProtocol ip_protocol) { onPortTested(status, ip_protocol); });
    }

    core_->port_test(Session::PORT_TEST_IPV4);
    core_->port_test(Session::PORT_TEST_IPV6);

    portTestSetSensitive();
}

NetworkPage::NetworkPage(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Session> const& core)
    : PageBase(cast_item, builder, core)
    , core_(core)
    , portLabel_(get_widget<Gtk::Label>("listening_port_status_label"))
    , portButton_(get_widget<Gtk::Button>("test_listening_port_button"))
    , portSpin_(init_spin_button("listening_port_spin", TR_KEY_peer_port, 1, std::numeric_limits<uint16_t>::max(), 1))
{
    portButton_->signal_clicked().connect([this]() { onPortTest(); });
    updatePortStatusText();

    prefsTag_ = core_->signal_prefs_changed().connect([this](auto key) { onCorePrefsChanged(key); });

    init_check_button("pick_random_listening_port_at_start_check", TR_KEY_peer_port_random_on_start);
    init_check_button("enable_listening_port_forwarding_check", TR_KEY_port_forwarding_enabled);
    init_spin_button("max_torrent_peers_spin", TR_KEY_peer_limit_per_torrent, 1, INT_MAX, 5);
    init_spin_button("max_total_peers_spin", TR_KEY_peer_limit_global, 1, INT_MAX, 5);

#ifdef WITH_UTP
    init_check_button("enable_utp_check", TR_KEY_utp_enabled);
#else
    get_widget<Gtk::CheckButton>("enable_utp_check")->hide();
#endif

    init_check_button("enable_pex_check", TR_KEY_pex_enabled);
    init_check_button("enable_dht_check", TR_KEY_dht_enabled);
    init_check_button("enable_lpd_check", TR_KEY_lpd_enabled);
    init_text_view("default_trackers_view", TR_KEY_default_trackers);
}

} // namespace

/****
*****
****/

std::unique_ptr<PrefsDialog> PrefsDialog::create(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("PrefsDialog.ui"));
    return std::unique_ptr<PrefsDialog>(gtr_get_widget_derived<PrefsDialog>(builder, "PrefsDialog", parent, core));
}

PrefsDialog::PrefsDialog(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core)
    : Gtk::Dialog(cast_item)
    , impl_(std::make_unique<Impl>(*this, builder, core))
{
    set_transient_for(parent);
}

PrefsDialog::~PrefsDialog() = default;

PrefsDialog::Impl::Impl(PrefsDialog& dialog, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core)
    : dialog_(dialog)
    , core_(core)
{
    gtr_get_widget_derived<SpeedPage>(builder, "speed_page_layout", core_);
    gtr_get_widget_derived<DownloadingPage>(builder, "downloading_page_layout", core_);
    gtr_get_widget_derived<SeedingPage>(builder, "seeding_page_layout", core_);
    gtr_get_widget_derived<PrivacyPage>(builder, "privacy_page_layout", core_);
    gtr_get_widget_derived<NetworkPage>(builder, "network_page_layout", core_);
    gtr_get_widget_derived<DesktopPage>(builder, "desktop_page_layout", core_);
    gtr_get_widget_derived<RemotePage>(builder, "remote_page_layout", core_);

    dialog_.signal_response().connect(sigc::mem_fun(*this, &Impl::response_cb));
}
