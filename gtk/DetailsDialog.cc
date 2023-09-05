// This file Copyright © 2007-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "DetailsDialog.h"

#include "Actions.h"
#include "FileList.h"
#include "GtkCompat.h"
#include "HigWorkarea.h" // GUI_PAD, GUI_PAD_BIG, GUI_PAD_SMALL
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

#include <libtransmission/utils.h>
#include <libtransmission/web-utils.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/quark.h>
#include <glibmm/ustring.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/cellrendererpixbuf.h>
#include <gtkmm/cellrendererprogress.h>
#include <gtkmm/cellrenderertext.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/combobox.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/liststore.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/textbuffer.h>
#include <gtkmm/textview.h>
#include <gtkmm/tooltip.h>
#include <gtkmm/treemodel.h>
#include <gtkmm/treemodelfilter.h>
#include <gtkmm/treemodelsort.h>
#include <gtkmm/treerowreference.h>
#include <gtkmm/treeview.h>

#include <fmt/chrono.h>
#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib> // abort()
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

using namespace std::literals;

class DetailsDialog::Impl
{
public:
    Impl(DetailsDialog& dialog, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~Impl();

    TR_DISABLE_COPY_MOVE(Impl)

    void set_torrents(std::vector<tr_torrent_id_t> const& torrent_ids);
    void refresh();

private:
    void info_page_init(Glib::RefPtr<Gtk::Builder> const& builder);
    void peer_page_init(Glib::RefPtr<Gtk::Builder> const& builder);
    void tracker_page_init(Glib::RefPtr<Gtk::Builder> const& builder);
    void options_page_init(Glib::RefPtr<Gtk::Builder> const& builder);

    void on_details_window_size_allocated();

    bool onPeerViewQueryTooltip(int x, int y, bool keyboard_tip, Glib::RefPtr<Gtk::Tooltip> const& tooltip);
    void onMorePeerInfoToggled();

    bool trackerVisibleFunc(Gtk::TreeModel::const_iterator const& iter);
    void on_tracker_list_selection_changed();

    void on_tracker_list_add_button_clicked();
    void on_edit_trackers();
    void on_tracker_list_remove_button_clicked();
    void onScrapeToggled();
    void onBackupToggled();

    void torrent_set_bool(tr_quark key, bool value);
    void torrent_set_int(tr_quark key, int value);
    void torrent_set_real(tr_quark key, double value);

    void refreshInfo(std::vector<tr_torrent*> const& torrents);
    void refreshPeers(std::vector<tr_torrent*> const& torrents);
    void refreshTracker(std::vector<tr_torrent*> const& torrents);
    void refreshFiles(std::vector<tr_torrent*> const& torrents);
    void refreshOptions(std::vector<tr_torrent*> const& torrents);

    void refreshPeerList(std::vector<tr_torrent*> const& torrents);
    void refreshWebseedList(std::vector<tr_torrent*> const& torrents);

    tr_torrent_id_t tracker_list_get_current_torrent_id() const;
    tr_torrent* tracker_list_get_current_torrent() const;

    std::vector<tr_torrent*> getTorrents() const;

private:
    DetailsDialog& dialog_;
    Glib::RefPtr<Session> const core_;

    Gtk::CheckButton* honor_limits_check_ = nullptr;
    Gtk::CheckButton* up_limited_check_ = nullptr;
    Gtk::SpinButton* up_limit_sping_ = nullptr;
    Gtk::CheckButton* down_limited_check_ = nullptr;
    Gtk::SpinButton* down_limit_spin_ = nullptr;
    Gtk::ComboBox* bandwidth_combo_ = nullptr;

    Gtk::ComboBox* ratio_combo_ = nullptr;
    Gtk::SpinButton* ratio_spin_ = nullptr;
    Gtk::ComboBox* idle_combo_ = nullptr;
    Gtk::SpinButton* idle_spin_ = nullptr;
    Gtk::SpinButton* max_peers_spin_ = nullptr;

    sigc::connection honor_limits_check_tag_;
    sigc::connection up_limited_check_tag_;
    sigc::connection down_limited_check_tag_;
    sigc::connection down_limit_spin_tag_;
    sigc::connection up_limit_spin_tag_;
    sigc::connection bandwidth_combo_tag_;
    sigc::connection ratio_combo_tag_;
    sigc::connection ratio_spin_tag_;
    sigc::connection idle_combo_tag_;
    sigc::connection idle_spin_tag_;
    sigc::connection max_peers_spin_tag_;

    Gtk::Label* added_lb_ = nullptr;
    Gtk::Label* size_lb_ = nullptr;
    Gtk::Label* state_lb_ = nullptr;
    Gtk::Label* have_lb_ = nullptr;
    Gtk::Label* dl_lb_ = nullptr;
    Gtk::Label* ul_lb_ = nullptr;
    Gtk::Label* error_lb_ = nullptr;
    Gtk::Label* date_started_lb_ = nullptr;
    Gtk::Label* eta_lb_ = nullptr;
    Gtk::Label* last_activity_lb_ = nullptr;

    Gtk::Label* hash_lb_ = nullptr;
    Gtk::Label* privacy_lb_ = nullptr;
    Gtk::Label* origin_lb_ = nullptr;
    Gtk::Label* destination_lb_ = nullptr;
    Glib::RefPtr<Gtk::TextBuffer> comment_buffer_;

    std::unordered_map<std::string, Gtk::TreeRowReference> peer_hash_;
    std::unordered_map<std::string, Gtk::TreeRowReference> webseed_hash_;
    Glib::RefPtr<Gtk::ListStore> peer_store_;
    Glib::RefPtr<Gtk::ListStore> webseed_store_;
    Gtk::ScrolledWindow* webseed_view_ = nullptr;
    Gtk::TreeView* peer_view_ = nullptr;
    Gtk::CheckButton* more_peer_details_check_ = nullptr;

    Glib::RefPtr<Gtk::ListStore> tracker_store_;
    std::unordered_map<std::string, Gtk::TreeRowReference> tracker_hash_;
    Glib::RefPtr<Gtk::TreeModelFilter> trackers_filtered_;
    Gtk::Button* add_tracker_button_ = nullptr;
    Gtk::Button* edit_trackers_button_ = nullptr;
    Gtk::Button* remove_tracker_button_ = nullptr;
    Gtk::TreeView* tracker_view_ = nullptr;
    Gtk::CheckButton* scrape_check_ = nullptr;
    Gtk::CheckButton* all_check_ = nullptr;

    FileList* file_list_ = nullptr;
    Gtk::Label* file_label_ = nullptr;

    std::vector<tr_torrent_id_t> ids_;
    sigc::connection periodic_refresh_tag_;

    Glib::Quark const TORRENT_ID_KEY = Glib::Quark("tr-torrent-id-key");
    Glib::Quark const TEXT_BUFFER_KEY = Glib::Quark("tr-text-buffer-key");
    Glib::Quark const URL_ENTRY_KEY = Glib::Quark("tr-url-entry-key");

    static guint last_page_;
};

guint DetailsDialog::Impl::last_page_ = 0;

std::vector<tr_torrent*> DetailsDialog::Impl::getTorrents() const
{
    std::vector<tr_torrent*> torrents;
    torrents.reserve(ids_.size());

    for (auto const id : ids_)
    {
        if (auto* torrent = core_->find_torrent(id); torrent != nullptr)
        {
            torrents.push_back(torrent);
        }
    }

    return torrents;
}

/****
*****
*****  OPTIONS TAB
*****
****/

namespace
{

void set_togglebutton_if_different(Gtk::CheckButton* toggle, sigc::connection& tag, bool value)
{
    bool const currentValue = toggle->get_active();

    if (currentValue != value)
    {
        tag.block();
        toggle->set_active(value);
        tag.unblock();
    }
}

void set_int_spin_if_different(Gtk::SpinButton* spin, sigc::connection& tag, int value)
{
    int const currentValue = spin->get_value_as_int();

    if (currentValue != value)
    {
        tag.block();
        spin->set_value(value);
        tag.unblock();
    }
}

void set_double_spin_if_different(Gtk::SpinButton* spin, sigc::connection& tag, double value)
{
    double const currentValue = spin->get_value();

    if ((int)(currentValue * 100) != (int)(value * 100))
    {
        tag.block();
        spin->set_value(value);
        tag.unblock();
    }
}

void unset_combo(Gtk::ComboBox* combobox, sigc::connection& tag)
{
    tag.block();
    combobox->set_active(-1);
    tag.unblock();
}

} // namespace

void DetailsDialog::Impl::refreshOptions(std::vector<tr_torrent*> const& torrents)
{
    /***
    ****  Options Page
    ***/

    /* honor_limits_check */
    if (!torrents.empty())
    {
        bool const baseline = tr_torrentUsesSessionLimits(torrents.front());
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [baseline](auto const* torrent) { return baseline == tr_torrentUsesSessionLimits(torrent); });

        if (is_uniform)
        {
            set_togglebutton_if_different(honor_limits_check_, honor_limits_check_tag_, baseline);
        }
    }

    /* down_limited_check */
    if (!torrents.empty())
    {
        bool const baseline = tr_torrentUsesSpeedLimit(torrents.front(), TR_DOWN);
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [baseline](auto const* torrent) { return baseline == tr_torrentUsesSpeedLimit(torrent, TR_DOWN); });

        if (is_uniform)
        {
            set_togglebutton_if_different(down_limited_check_, down_limited_check_tag_, baseline);
        }
    }

    /* down_limit_spin */
    if (!torrents.empty())
    {
        auto const baseline = tr_torrentGetSpeedLimit_KBps(torrents.front(), TR_DOWN);
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [baseline](auto const* torrent) { return baseline == tr_torrentGetSpeedLimit_KBps(torrent, TR_DOWN); });

        if (is_uniform)
        {
            set_int_spin_if_different(down_limit_spin_, down_limit_spin_tag_, baseline);
        }
    }

    /* up_limited_check */
    if (!torrents.empty())
    {
        bool const baseline = tr_torrentUsesSpeedLimit(torrents.front(), TR_UP);
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [baseline](auto const* torrent) { return baseline == tr_torrentUsesSpeedLimit(torrent, TR_UP); });

        if (is_uniform)
        {
            set_togglebutton_if_different(up_limited_check_, up_limited_check_tag_, baseline);
        }
    }

    /* up_limit_sping */
    if (!torrents.empty())
    {
        auto const baseline = tr_torrentGetSpeedLimit_KBps(torrents.front(), TR_UP);
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [baseline](auto const* torrent) { return baseline == tr_torrentGetSpeedLimit_KBps(torrent, TR_UP); });

        if (is_uniform)
        {
            set_int_spin_if_different(up_limit_sping_, up_limit_spin_tag_, baseline);
        }
    }

    /* bandwidth_combo */
    if (!torrents.empty())
    {
        auto const baseline = tr_torrentGetPriority(torrents.front());
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [baseline](auto const* torrent) { return baseline == tr_torrentGetPriority(torrent); });

        if (is_uniform)
        {
            bandwidth_combo_tag_.block();
            gtr_combo_box_set_active_enum(*bandwidth_combo_, baseline);
            bandwidth_combo_tag_.unblock();
        }
        else
        {
            unset_combo(bandwidth_combo_, bandwidth_combo_tag_);
        }
    }

    /* ratio_combo */
    if (!torrents.empty())
    {
        auto const baseline = tr_torrentGetRatioMode(torrents.front());
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [baseline](auto const* torrent) { return baseline == tr_torrentGetRatioMode(torrent); });

        if (is_uniform)
        {
            ratio_combo_tag_.block();
            gtr_combo_box_set_active_enum(*ratio_combo_, baseline);
            gtr_widget_set_visible(*ratio_spin_, baseline == TR_RATIOLIMIT_SINGLE);
            ratio_combo_tag_.unblock();
        }
    }

    /* ratio_spin */
    if (!torrents.empty())
    {
        auto const baseline = tr_torrentGetRatioLimit(torrents.front());
        set_double_spin_if_different(ratio_spin_, ratio_spin_tag_, baseline);
    }

    /* idle_combo */
    if (!torrents.empty())
    {
        auto const baseline = tr_torrentGetIdleMode(torrents.front());
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [baseline](auto const* torrent) { return baseline == tr_torrentGetIdleMode(torrent); });

        if (is_uniform)
        {
            idle_combo_tag_.block();
            gtr_combo_box_set_active_enum(*idle_combo_, baseline);
            gtr_widget_set_visible(*idle_spin_, baseline == TR_IDLELIMIT_SINGLE);
            idle_combo_tag_.unblock();
        }
    }

    /* idle_spin */
    if (!torrents.empty())
    {
        auto const baseline = tr_torrentGetIdleLimit(torrents.front());
        set_int_spin_if_different(idle_spin_, idle_spin_tag_, baseline);
    }

    /* max_peers_spin */
    if (!torrents.empty())
    {
        auto const baseline = tr_torrentGetPeerLimit(torrents.front());
        set_int_spin_if_different(max_peers_spin_, max_peers_spin_tag_, baseline);
    }
}

void DetailsDialog::Impl::torrent_set_bool(tr_quark key, bool value)
{
    auto top = tr_variant::make_map(2U);
    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-set"sv);
    tr_variant* const args = tr_variantDictAddDict(&top, TR_KEY_arguments, 2);
    tr_variantDictAddBool(args, key, value);
    tr_variant* const ids = tr_variantDictAddList(args, TR_KEY_ids, ids_.size());

    for (auto const id : ids_)
    {
        tr_variantListAddInt(ids, id);
    }

    core_->exec(&top);
}

void DetailsDialog::Impl::torrent_set_int(tr_quark key, int value)
{
    auto top = tr_variant::make_map(2U);
    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-set"sv);
    tr_variant* const args = tr_variantDictAddDict(&top, TR_KEY_arguments, 2);
    tr_variantDictAddInt(args, key, value);
    tr_variant* const ids = tr_variantDictAddList(args, TR_KEY_ids, ids_.size());

    for (auto const id : ids_)
    {
        tr_variantListAddInt(ids, id);
    }

    core_->exec(&top);
}

void DetailsDialog::Impl::torrent_set_real(tr_quark key, double value)
{
    auto top = tr_variant::make_map(2U);
    tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-set"sv);
    tr_variant* const args = tr_variantDictAddDict(&top, TR_KEY_arguments, 2);
    tr_variantDictAddReal(args, key, value);
    tr_variant* const ids = tr_variantDictAddList(args, TR_KEY_ids, ids_.size());

    for (auto const id : ids_)
    {
        tr_variantListAddInt(ids, id);
    }

    core_->exec(&top);
}

void DetailsDialog::Impl::options_page_init(Glib::RefPtr<Gtk::Builder> const& /*builder*/)
{
    honor_limits_check_tag_ = honor_limits_check_->signal_toggled().connect(
        [this]() { torrent_set_bool(TR_KEY_honorsSessionLimits, honor_limits_check_->get_active()); });

    down_limited_check_->set_label(fmt::format(down_limited_check_->get_label().raw(), fmt::arg("speed_units", speed_K_str)));
    down_limited_check_tag_ = down_limited_check_->signal_toggled().connect(
        [this]() { torrent_set_bool(TR_KEY_downloadLimited, down_limited_check_->get_active()); });

    down_limit_spin_->set_adjustment(Gtk::Adjustment::create(0, 0, std::numeric_limits<int>::max(), 5));
    down_limit_spin_tag_ = down_limit_spin_->signal_value_changed().connect(
        [this]() { torrent_set_int(TR_KEY_downloadLimit, down_limit_spin_->get_value_as_int()); });

    up_limited_check_->set_label(fmt::format(up_limited_check_->get_label().raw(), fmt::arg("speed_units", speed_K_str)));
    up_limited_check_tag_ = up_limited_check_->signal_toggled().connect(
        [this]() { torrent_set_bool(TR_KEY_uploadLimited, up_limited_check_->get_active()); });

    up_limit_sping_->set_adjustment(Gtk::Adjustment::create(0, 0, std::numeric_limits<int>::max(), 5));
    up_limit_spin_tag_ = up_limit_sping_->signal_value_changed().connect(
        [this]() { torrent_set_int(TR_KEY_uploadLimit, up_limit_sping_->get_value_as_int()); });

    gtr_priority_combo_init(*bandwidth_combo_);
    bandwidth_combo_tag_ = bandwidth_combo_->signal_changed().connect(
        [this]() { torrent_set_int(TR_KEY_bandwidthPriority, gtr_combo_box_get_active_enum(*bandwidth_combo_)); });

    gtr_combo_box_set_enum(
        *ratio_combo_,
        {
            { _("Use global settings"), TR_RATIOLIMIT_GLOBAL },
            { _("Seed regardless of ratio"), TR_RATIOLIMIT_UNLIMITED },
            { _("Stop seeding at ratio:"), TR_RATIOLIMIT_SINGLE },
        });
    ratio_combo_tag_ = ratio_combo_->signal_changed().connect(
        [this]()
        {
            torrent_set_int(TR_KEY_seedRatioMode, gtr_combo_box_get_active_enum(*ratio_combo_));
            refresh();
        });
    ratio_spin_->set_adjustment(Gtk::Adjustment::create(0, 0, 1000, .05));
    ratio_spin_->set_width_chars(7);
    ratio_spin_tag_ = ratio_spin_->signal_value_changed().connect(
        [this]() { torrent_set_real(TR_KEY_seedRatioLimit, ratio_spin_->get_value()); });

    gtr_combo_box_set_enum(
        *idle_combo_,
        {
            { _("Use global settings"), TR_IDLELIMIT_GLOBAL },
            { _("Seed regardless of activity"), TR_IDLELIMIT_UNLIMITED },
            { _("Stop seeding if idle for N minutes:"), TR_IDLELIMIT_SINGLE },
        });
    idle_combo_tag_ = idle_combo_->signal_changed().connect(
        [this]()
        {
            torrent_set_int(TR_KEY_seedIdleMode, gtr_combo_box_get_active_enum(*idle_combo_));
            refresh();
        });
    idle_spin_->set_adjustment(Gtk::Adjustment::create(1, 1, 40320, 5));
    idle_spin_tag_ = idle_spin_->signal_value_changed().connect(
        [this]() { torrent_set_int(TR_KEY_seedIdleLimit, idle_spin_->get_value_as_int()); });

    max_peers_spin_->set_adjustment(Gtk::Adjustment::create(1, 1, 3000, 5));
    max_peers_spin_tag_ = max_peers_spin_->signal_value_changed().connect(
        [this]() { torrent_set_int(TR_KEY_peer_limit, max_peers_spin_->get_value_as_int()); });
}

/****
*****
*****  INFO TAB
*****
****/

namespace
{

Glib::ustring activityString(int activity, bool finished)
{
    switch (activity)
    {
    case TR_STATUS_CHECK_WAIT:
        return _("Queued for verification");

    case TR_STATUS_CHECK:
        return _("Verifying local data");

    case TR_STATUS_DOWNLOAD_WAIT:
        return _("Queued for download");

    case TR_STATUS_DOWNLOAD:
        return C_("Verb", "Downloading");

    case TR_STATUS_SEED_WAIT:
        return _("Queued for seeding");

    case TR_STATUS_SEED:
        return C_("Verb", "Seeding");

    case TR_STATUS_STOPPED:
        return finished ? _("Finished") : _("Paused");

    default:
        g_assert_not_reached();
    }

    return {};
}

/* Only call gtk_text_buffer_set_text () if the new text differs from the old.
 * This way if the user has text selected, refreshing won't deselect it */
void gtr_text_buffer_set_text(Glib::RefPtr<Gtk::TextBuffer> const& b, Glib::ustring const& str)
{
    if (b->get_text() != str)
    {
        b->set_text(str);
    }
}

[[nodiscard]] std::string get_date_string(time_t t)
{
    return t == 0 ? _("N/A") : fmt::format(FMT_STRING("{:%x}"), fmt::localtime(t));
}

[[nodiscard]] std::string get_date_time_string(time_t t)
{
    return t == 0 ? _("N/A") : fmt::format(FMT_STRING("{:%c}"), fmt::localtime(t));
}

} // namespace

void DetailsDialog::Impl::refreshInfo(std::vector<tr_torrent*> const& torrents)
{
    auto const now = time(nullptr);
    Glib::ustring str;
    Glib::ustring const mixed = _("Mixed");
    Glib::ustring const no_torrent = _("No Torrents Selected");
    Glib::ustring stateString;
    uint64_t sizeWhenDone = 0;
    std::vector<tr_stat const*> stats;
    std::vector<tr_torrent_view> infos;

    stats.reserve(torrents.size());
    infos.reserve(torrents.size());
    for (auto* const torrent : torrents)
    {
        stats.push_back(tr_torrentStatCached(torrent));
        infos.push_back(tr_torrentView(torrent));
    }

    /* privacy_lb */
    if (infos.empty())
    {
        str = no_torrent;
    }
    else
    {
        bool const baseline = infos.front().is_private;
        bool const is_uniform = std::all_of(
            infos.begin(),
            infos.end(),
            [baseline](auto const& info) { return info.is_private == baseline; });

        if (is_uniform)
        {
            str = baseline ? _("Private to this tracker -- DHT and PEX disabled") : _("Public torrent");
        }
        else
        {
            str = mixed;
        }
    }

    privacy_lb_->set_text(str);

    /* added_lb */
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        auto const baseline = stats.front()->addedDate;
        bool const is_uniform = std::all_of(
            stats.begin(),
            stats.end(),
            [baseline](auto const* stat) { return stat->addedDate == baseline; });

        if (is_uniform)
        {
            str = get_date_time_string(baseline);
        }
        else
        {
            str = mixed;
        }
    }

    added_lb_->set_text(str);

    /* origin_lb */
    if (infos.empty())
    {
        str = no_torrent;
    }
    else
    {
        auto const creator = tr_strv_strip(infos.front().creator != nullptr ? infos.front().creator : "");
        auto const date = infos.front().date_created;
        auto const datestr = get_date_string(date);
        bool const mixed_creator = std::any_of(
            infos.begin(),
            infos.end(),
            [&creator](auto const& info) { return creator != (info.creator != nullptr ? info.creator : ""); });
        bool const mixed_date = std::any_of(
            infos.begin(),
            infos.end(),
            [date](auto const& info) { return date != info.date_created; });

        bool const empty_creator = std::empty(creator);
        bool const empty_date = date == 0;

        if (mixed_creator || mixed_date)
        {
            str = mixed;
        }
        else if (!empty_creator && !empty_date)
        {
            str = fmt::format(_("Created by {creator} on {date}"), fmt::arg("creator", creator), fmt::arg("date", datestr));
        }
        else if (!empty_creator)
        {
            str = fmt::format(_("Created by {creator}"), fmt::arg("creator", creator));
        }
        else if (!empty_date)
        {
            str = fmt::format(_("Created on {date}"), fmt::arg("date", datestr));
        }
        else
        {
            str = _("N/A");
        }
    }

    origin_lb_->set_text(str);

    /* comment_buffer */
    if (infos.empty())
    {
        str.clear();
    }
    else
    {
        auto const baseline = Glib::ustring(infos.front().comment != nullptr ? infos.front().comment : "");
        bool const is_uniform = std::all_of(
            infos.begin(),
            infos.end(),
            [&baseline](auto const& info) { return baseline == (info.comment != nullptr ? info.comment : ""); });

        str = is_uniform ? baseline : mixed;
    }

    gtr_text_buffer_set_text(comment_buffer_, str);

    /* destination_lb */
    if (torrents.empty())
    {
        str = no_torrent;
    }
    else
    {
        auto const baseline = Glib::ustring(tr_torrentGetDownloadDir(torrents.front()));
        bool const is_uniform = std::all_of(
            torrents.begin(),
            torrents.end(),
            [&baseline](auto const* torrent) { return baseline == tr_torrentGetDownloadDir(torrent); });

        str = is_uniform ? baseline : mixed;
    }

    destination_lb_->set_text(str);

    /* state_lb */
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        auto const activity = stats.front()->activity;
        bool const is_uniform = std::all_of(
            stats.begin(),
            stats.end(),
            [activity](auto const* st) { return activity == st->activity; });
        bool const allFinished = std::all_of(stats.begin(), stats.end(), [](auto const* st) { return st->finished; });

        str = is_uniform ? activityString(activity, allFinished) : mixed;
    }

    stateString = str;
    state_lb_->set_text(str);

    /* date started */
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        time_t const baseline = stats.front()->startDate;
        bool const is_uniform = std::all_of(
            stats.begin(),
            stats.end(),
            [baseline](auto const* st) { return baseline == st->startDate; });

        if (!is_uniform)
        {
            str = mixed;
        }
        else if (baseline <= 0 || stats[0]->activity == TR_STATUS_STOPPED)
        {
            str = stateString;
        }
        else
        {
            str = tr_format_time(now - baseline);
        }
    }

    date_started_lb_->set_text(str);

    /* eta */
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        auto const baseline = stats.front()->eta;
        auto const is_uniform = std::all_of(
            stats.begin(),
            stats.end(),
            [baseline](auto const* st) { return baseline == st->eta; });

        if (!is_uniform)
        {
            str = mixed;
        }
        else if (baseline < 0)
        {
            str = _("Unknown");
        }
        else
        {
            str = tr_format_time_left(baseline);
        }
    }

    eta_lb_->set_text(str);

    /* size_lb */
    {
        auto const piece_count = std::accumulate(
            std::begin(infos),
            std::end(infos),
            uint64_t{},
            [](auto sum, auto const& info) { return sum + info.n_pieces; });

        if (piece_count == 0)
        {
            str.clear();
        }
        else
        {
            auto const total_size = std::accumulate(
                std::begin(infos),
                std::end(infos),
                uint64_t{},
                [](auto sum, auto const& info) { return sum + info.total_size; });

            auto const file_count = std::accumulate(
                std::begin(torrents),
                std::end(torrents),
                std::size_t{},
                [](auto sum, auto const* tor) { return sum + tr_torrentFileCount(tor); });

            str = fmt::format(
                ngettext("{total_size} in {file_count:L} file", "{total_size} in {file_count:L} files", file_count),
                fmt::arg("total_size", tr_strlsize(total_size)),
                fmt::arg("file_count", file_count));

            auto const piece_size = std::empty(infos) ? uint32_t{} : infos.front().piece_size;
            auto const piece_size_is_uniform = std::all_of(
                std::begin(infos),
                std::end(infos),
                [piece_size](auto const& info) { return info.piece_size == piece_size; });

            if (piece_size_is_uniform)
            {
                str += ' ';
                str += fmt::format(
                    ngettext(
                        "({piece_count} BitTorrent piece @ {piece_size})",
                        "({piece_count} BitTorrent pieces @ {piece_size})",
                        piece_count),
                    fmt::arg("piece_count", piece_count),
                    fmt::arg("piece_size", tr_formatter_mem_B(piece_size)));
            }
        }

        size_lb_->set_text(str);
    }

    /* have_lb */
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        uint64_t leftUntilDone = 0;
        uint64_t haveUnchecked = 0;
        uint64_t haveValid = 0;
        uint64_t available = 0;

        for (auto const* const st : stats)
        {
            haveUnchecked += st->haveUnchecked;
            haveValid += st->haveValid;
            sizeWhenDone += st->sizeWhenDone;
            leftUntilDone += st->leftUntilDone;
            available += st->sizeWhenDone - st->leftUntilDone + st->haveUnchecked + st->desiredAvailable;
        }

        {
            double const d = sizeWhenDone != 0 ? (100.0 * available) / sizeWhenDone : 0;
            double const ratio = 100.0 * (sizeWhenDone != 0 ? (haveValid + haveUnchecked) / (double)sizeWhenDone : 1);

            auto const avail = tr_strpercent(d);
            auto const buf2 = tr_strpercent(ratio);
            auto const total = tr_strlsize(haveUnchecked + haveValid);
            auto const unver = tr_strlsize(haveUnchecked);

            if (haveUnchecked == 0 && leftUntilDone == 0)
            {
                str = fmt::format(
                    _("{current_size} ({percent_done}%)"),
                    fmt::arg("current_size", total),
                    fmt::arg("percent_done", buf2));
            }
            else if (haveUnchecked == 0)
            {
                str = fmt::format(
                    // xgettext:no-c-format
                    _("{current_size} ({percent_done}% of {percent_available}% available)"),
                    fmt::arg("current_size", total),
                    fmt::arg("percent_done", buf2),
                    fmt::arg("percent_available", avail));
            }
            else
            {
                str = fmt::format(
                    // xgettext:no-c-format
                    _("{current_size} ({percent_done}% of {percent_available}% available; {unverified_size} unverified)"),
                    fmt::arg("current_size", total),
                    fmt::arg("percent_done", buf2),
                    fmt::arg("percent_available", avail),
                    fmt::arg("unverified_size", unver));
            }
        }
    }

    have_lb_->set_text(str);

    // dl_lb
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        auto const downloaded_str = tr_strlsize(std::accumulate(
            std::begin(stats),
            std::end(stats),
            uint64_t{ 0 },
            [](auto sum, auto const* st) { return sum + st->downloadedEver; }));

        auto const failed = std::accumulate(
            std::begin(stats),
            std::end(stats),
            uint64_t{ 0 },
            [](auto sum, auto const* st) { return sum + st->corruptEver; });

        if (failed != 0)
        {
            str = fmt::format(
                _("{downloaded_size} (+{discarded_size} discarded after failed checksum)"),
                fmt::arg("downloaded_size", downloaded_str),
                fmt::arg("discarded_size", tr_strlsize(failed)));
        }
        else
        {
            str = downloaded_str;
        }
    }

    dl_lb_->set_text(str);

    /* ul_lb */
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        auto const uploaded = std::accumulate(
            std::begin(stats),
            std::end(stats),
            uint64_t{},
            [](auto sum, auto const* st) { return sum + st->uploadedEver; });
        auto const denominator = std::accumulate(
            std::begin(stats),
            std::end(stats),
            uint64_t{},
            [](auto sum, auto const* st) { return sum + st->sizeWhenDone; });
        str = fmt::format(
            _("{uploaded_size} (Ratio: {ratio})"),
            fmt::arg("uploaded_size", tr_strlsize(uploaded)),
            fmt::arg("ratio", tr_strlratio(tr_getRatio(uploaded, denominator))));
    }

    ul_lb_->set_text(str);

    /* hash_lb */
    if (infos.empty())
    {
        str = no_torrent;
    }
    else if (infos.size() == 1)
    {
        str = infos.front().hash_string;
    }
    else
    {
        str = mixed;
    }

    hash_lb_->set_text(str);

    /* error */
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        auto const baseline = Glib::ustring(stats.front()->errorString);
        bool const is_uniform = std::all_of(
            stats.begin(),
            stats.end(),
            [&baseline](auto const* st) { return baseline == st->errorString; });

        str = is_uniform ? baseline : mixed;
    }

    if (str.empty())
    {
        str = _("No errors");
    }

    error_lb_->set_text(str);

    /* activity date */
    if (stats.empty())
    {
        str = no_torrent;
    }
    else
    {
        time_t const latest = (*std::max_element(
                                   stats.begin(),
                                   stats.end(),
                                   [](auto const* lhs, auto const* rhs) { return lhs->activityDate < rhs->activityDate; }))
                                  ->activityDate;

        if (latest <= 0)
        {
            str = _("Never");
        }
        else if ((now - latest) < 5)
        {
            str = _("Active now");
        }
        else
        {
            str = tr_format_time_relative(now, latest);
        }
    }

    last_activity_lb_->set_text(str);
}

void DetailsDialog::Impl::info_page_init(Glib::RefPtr<Gtk::Builder> const& builder)
{
    comment_buffer_ = Gtk::TextBuffer::create();
    auto* tw = gtr_get_widget<Gtk::TextView>(builder, "comment_value_view");
    tw->set_buffer(comment_buffer_);
}

/****
*****
*****  PEERS TAB
*****
****/

namespace
{

class WebseedModelColumns : public Gtk::TreeModelColumnRecord
{
public:
    WebseedModelColumns() noexcept
    {
        add(key);
        add(was_updated);
        add(url);
        add(download_rate_double);
        add(download_rate_string);
    }

    Gtk::TreeModelColumn<std::string> key;
    Gtk::TreeModelColumn<bool> was_updated;
    Gtk::TreeModelColumn<Glib::ustring> url;
    Gtk::TreeModelColumn<double> download_rate_double;
    Gtk::TreeModelColumn<Glib::ustring> download_rate_string;
};

WebseedModelColumns const webseed_cols;

class PeerModelColumns : public Gtk::TreeModelColumnRecord
{
public:
    PeerModelColumns() noexcept
    {
        add(key);
        add(was_updated);
        add(address);
        add(address_collated);
        add(download_rate_double);
        add(download_rate_string);
        add(upload_rate_double);
        add(upload_rate_string);
        add(client);
        add(progress);
        add(upload_request_count_number);
        add(upload_request_count_string);
        add(download_request_count_number);
        add(download_request_count_string);
        add(blocks_downloaded_count_number);
        add(blocks_downloaded_count_string);
        add(blocks_uploaded_count_number);
        add(blocks_uploaded_count_string);
        add(reqs_cancelled_by_client_count_number);
        add(reqs_cancelled_by_client_count_string);
        add(reqs_cancelled_by_peer_count_number);
        add(reqs_cancelled_by_peer_count_string);
        add(encryption_stock_id);
        add(flags);
        add(torrent_name);
    }

    Gtk::TreeModelColumn<std::string> key;
    Gtk::TreeModelColumn<bool> was_updated;
    Gtk::TreeModelColumn<Glib::ustring> address;
    Gtk::TreeModelColumn<Glib::ustring> address_collated;
    Gtk::TreeModelColumn<double> download_rate_double;
    Gtk::TreeModelColumn<Glib::ustring> download_rate_string;
    Gtk::TreeModelColumn<double> upload_rate_double;
    Gtk::TreeModelColumn<Glib::ustring> upload_rate_string;
    Gtk::TreeModelColumn<Glib::ustring> client;
    Gtk::TreeModelColumn<int> progress;
    Gtk::TreeModelColumn<decltype(tr_peer_stat::activeReqsToClient)> upload_request_count_number;
    Gtk::TreeModelColumn<Glib::ustring> upload_request_count_string;
    Gtk::TreeModelColumn<decltype(tr_peer_stat::activeReqsToPeer)> download_request_count_number;
    Gtk::TreeModelColumn<Glib::ustring> download_request_count_string;
    Gtk::TreeModelColumn<decltype(tr_peer_stat::blocksToClient)> blocks_downloaded_count_number;
    Gtk::TreeModelColumn<Glib::ustring> blocks_downloaded_count_string;
    Gtk::TreeModelColumn<decltype(tr_peer_stat::blocksToPeer)> blocks_uploaded_count_number;
    Gtk::TreeModelColumn<Glib::ustring> blocks_uploaded_count_string;
    Gtk::TreeModelColumn<decltype(tr_peer_stat::cancelsToPeer)> reqs_cancelled_by_client_count_number;
    Gtk::TreeModelColumn<Glib::ustring> reqs_cancelled_by_client_count_string;
    Gtk::TreeModelColumn<decltype(tr_peer_stat::cancelsToClient)> reqs_cancelled_by_peer_count_number;
    Gtk::TreeModelColumn<Glib::ustring> reqs_cancelled_by_peer_count_string;
    Gtk::TreeModelColumn<Glib::ustring> encryption_stock_id;
    Gtk::TreeModelColumn<Glib::ustring> flags;
    Gtk::TreeModelColumn<Glib::ustring> torrent_name;
};

PeerModelColumns const peer_cols;

void initPeerRow(
    Gtk::TreeModel::iterator const& iter,
    std::string_view key,
    std::string_view torrent_name,
    tr_peer_stat const* peer)
{
    g_return_if_fail(peer != nullptr);

    char const* client = peer->client;
    if (client == nullptr || g_strcmp0(client, "Unknown Client") == 0)
    {
        client = "";
    }

    auto peer_addr4 = in_addr();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* const peer_addr4_octets = reinterpret_cast<uint8_t const*>(&peer_addr4.s_addr);
    auto const collated_name = inet_pton(AF_INET, std::data(peer->addr), &peer_addr4) != 1 ?
        std::data(peer->addr) :
        fmt::format(
            "{:03}",
            fmt::join(
                peer_addr4_octets,
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                peer_addr4_octets + sizeof(peer_addr4.s_addr), // TODO(C++20): Use std::span
                "."));

    (*iter)[peer_cols.address] = std::data(peer->addr);
    (*iter)[peer_cols.address_collated] = collated_name;
    (*iter)[peer_cols.client] = client;
    (*iter)[peer_cols.encryption_stock_id] = peer->isEncrypted ? "lock" : "";
    (*iter)[peer_cols.key] = std::string(key);
    (*iter)[peer_cols.torrent_name] = std::string(torrent_name);
}

void refreshPeerRow(Gtk::TreeModel::iterator const& iter, tr_peer_stat const* peer)
{
    std::string up_speed;
    std::string down_speed;
    std::string up_count;
    std::string down_count;
    std::string blocks_to_peer;
    std::string blocks_to_client;
    std::string cancelled_by_peer;
    std::string cancelled_by_client;

    g_return_if_fail(peer != nullptr);

    if (peer->rateToPeer_KBps > 0.01)
    {
        up_speed = tr_formatter_speed_KBps(peer->rateToPeer_KBps);
    }

    if (peer->rateToClient_KBps > 0)
    {
        down_speed = tr_formatter_speed_KBps(peer->rateToClient_KBps);
    }

    if (peer->activeReqsToPeer > 0)
    {
        down_count = std::to_string(peer->activeReqsToPeer);
    }

    if (peer->activeReqsToClient > 0)
    {
        up_count = std::to_string(peer->activeReqsToClient);
    }

    if (peer->blocksToPeer > 0)
    {
        blocks_to_peer = std::to_string(peer->blocksToPeer);
    }

    if (peer->blocksToClient > 0)
    {
        blocks_to_client = std::to_string(peer->blocksToClient);
    }

    if (peer->cancelsToPeer > 0)
    {
        cancelled_by_client = std::to_string(peer->cancelsToPeer);
    }

    if (peer->cancelsToClient > 0)
    {
        cancelled_by_peer = std::to_string(peer->cancelsToClient);
    }

    (*iter)[peer_cols.progress] = static_cast<int>(100.0 * peer->progress);
    (*iter)[peer_cols.upload_request_count_number] = peer->activeReqsToClient;
    (*iter)[peer_cols.upload_request_count_string] = up_count;
    (*iter)[peer_cols.download_request_count_number] = peer->activeReqsToPeer;
    (*iter)[peer_cols.download_request_count_string] = down_count;
    (*iter)[peer_cols.download_rate_double] = peer->rateToClient_KBps;
    (*iter)[peer_cols.download_rate_string] = down_speed;
    (*iter)[peer_cols.upload_rate_double] = peer->rateToPeer_KBps;
    (*iter)[peer_cols.upload_rate_string] = up_speed;
    (*iter)[peer_cols.flags] = std::data(peer->flagStr);
    (*iter)[peer_cols.was_updated] = true;
    (*iter)[peer_cols.blocks_downloaded_count_number] = peer->blocksToClient;
    (*iter)[peer_cols.blocks_downloaded_count_string] = blocks_to_client;
    (*iter)[peer_cols.blocks_uploaded_count_number] = peer->blocksToPeer;
    (*iter)[peer_cols.blocks_uploaded_count_string] = blocks_to_peer;
    (*iter)[peer_cols.reqs_cancelled_by_client_count_number] = peer->cancelsToPeer;
    (*iter)[peer_cols.reqs_cancelled_by_client_count_string] = cancelled_by_client;
    (*iter)[peer_cols.reqs_cancelled_by_peer_count_number] = peer->cancelsToClient;
    (*iter)[peer_cols.reqs_cancelled_by_peer_count_string] = cancelled_by_peer;
}

} // namespace

void DetailsDialog::Impl::refreshPeerList(std::vector<tr_torrent*> const& torrents)
{
    auto& hash = peer_hash_;
    auto const& store = peer_store_;

    /* step 1: get all the peers */
    std::vector<tr_peer_stat*> peers;
    std::vector<size_t> peerCount;

    peers.reserve(torrents.size());
    peerCount.reserve(torrents.size());
    for (auto const* const torrent : torrents)
    {
        size_t count = 0;
        peers.push_back(tr_torrentPeers(torrent, &count));
        peerCount.push_back(count);
    }

    /* step 2: mark all the peers in the list as not-updated */
    for (auto& row : store->children())
    {
        row[peer_cols.was_updated] = false;
    }

    auto make_key = [](tr_torrent const* tor, tr_peer_stat const* ps)
    {
        return fmt::format(FMT_STRING("{:d}.{:s}"), tr_torrentId(tor), ps->addr);
    };

    /* step 3: add any new peers */
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        auto const* tor = torrents.at(i);

        for (size_t j = 0; j < peerCount[i]; ++j)
        {
            auto const* s = &peers.at(i)[j];
            auto const key = make_key(tor, s);

            if (hash.find(key) == hash.end())
            {
                auto const iter = store->append();
                initPeerRow(iter, key, tr_torrentName(tor), s);
                hash.try_emplace(key, Gtk::TreeRowReference(store, store->get_path(iter)));
            }
        }
    }

    /* step 4: update the peers */
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        auto const* tor = torrents.at(i);

        for (size_t j = 0; j < peerCount[i]; ++j)
        {
            auto const* s = &peers.at(i)[j];
            auto const key = make_key(tor, s);
            refreshPeerRow(store->get_iter(hash.at(key).get_path()), s);
        }
    }

    /* step 5: remove peers that have disappeared */
    if (auto iter = store->children().begin(); iter)
    {
        while (iter)
        {
            if (iter->get_value(peer_cols.was_updated))
            {
                ++iter;
            }
            else
            {
                auto const key = iter->get_value(peer_cols.key);
                hash.erase(key);
                iter = store->erase(iter);
            }
        }
    }

    /* step 6: cleanup */
    for (size_t i = 0; i < peers.size(); ++i)
    {
        tr_torrentPeersFree(peers[i], peerCount[i]);
    }
}

void DetailsDialog::Impl::refreshWebseedList(std::vector<tr_torrent*> const& torrents)
{
    auto has_any_webseeds = bool{ false };
    auto& hash = webseed_hash_;
    auto const& store = webseed_store_;

    auto make_key = [](tr_torrent const* tor, char const* url)
    {
        return fmt::format(FMT_STRING("{:d}.{:s}"), tr_torrentId(tor), url);
    };

    /* step 1: mark all webseeds as not-updated */
    for (auto& row : store->children())
    {
        row[webseed_cols.was_updated] = false;
    }

    /* step 2: add any new webseeds */
    for (auto const* const tor : torrents)
    {
        for (size_t j = 0, n = tr_torrentWebseedCount(tor); j < n; ++j)
        {
            has_any_webseeds = true;

            auto const* const url = tr_torrentWebseed(tor, j).url;
            auto const key = make_key(tor, url);

            if (hash.find(key) == hash.end())
            {
                auto const iter = store->append();
                (*iter)[webseed_cols.url] = url;
                (*iter)[webseed_cols.key] = key;
                hash.try_emplace(key, Gtk::TreeRowReference(store, store->get_path(iter)));
            }
        }
    }

    /* step 3: update the webseeds */
    for (auto const* const tor : torrents)
    {
        for (size_t j = 0, n = tr_torrentWebseedCount(tor); j < n; ++j)
        {
            auto const webseed = tr_torrentWebseed(tor, j);
            auto const key = make_key(tor, webseed.url);
            auto const iter = store->get_iter(hash.at(key).get_path());

            auto const KBps = double(webseed.download_bytes_per_second) / speed_K;
            auto const buf = webseed.is_downloading ? tr_formatter_speed_KBps(KBps) : std::string();

            (*iter)[webseed_cols.download_rate_double] = KBps;
            (*iter)[webseed_cols.download_rate_string] = buf;
            (*iter)[webseed_cols.was_updated] = true;
        }
    }

    /* step 4: remove webseeds that have disappeared */
    if (auto iter = store->children().begin(); iter)
    {
        while (iter)
        {
            if (iter->get_value(webseed_cols.was_updated))
            {
                ++iter;
            }
            else
            {
                auto const key = iter->get_value(webseed_cols.key);
                hash.erase(key);
                iter = store->erase(iter);
            }
        }
    }

    /* most of the time there are no webseeds...
       don't waste space showing an empty list */
    webseed_view_->set_visible(has_any_webseeds);
}

void DetailsDialog::Impl::refreshPeers(std::vector<tr_torrent*> const& torrents)
{
    refreshPeerList(torrents);
    refreshWebseedList(torrents);
}

bool DetailsDialog::Impl::onPeerViewQueryTooltip(int x, int y, bool keyboard_tip, Glib::RefPtr<Gtk::Tooltip> const& tooltip)
{
    Gtk::TreeModel::iterator iter;
    bool show_tip = false;

    if (peer_view_->get_tooltip_context_iter(x, y, keyboard_tip, iter))
    {
        auto const name = iter->get_value(peer_cols.torrent_name);
        auto const addr = iter->get_value(peer_cols.address);
        auto const flagstr = iter->get_value(peer_cols.flags);

        std::ostringstream gstr;
        gstr << "<b>" << Glib::Markup::escape_text(name) << "</b>\n" << addr << "\n \n";

        for (char const ch : flagstr)
        {
            char const* s = nullptr;

            switch (ch)
            {
            case 'O':
                s = _("Optimistic unchoke");
                break;

            case 'D':
                s = _("Downloading from this peer");
                break;

            case 'd':
                s = _("We would download from this peer if they would let us");
                break;

            case 'U':
                s = _("Uploading to peer");
                break;

            case 'u':
                s = _("We would upload to this peer if they asked");
                break;

            case 'K':
                s = _("Peer has unchoked us, but we're not interested");
                break;

            case '?':
                s = _("We unchoked this peer, but they're not interested");
                break;

            case 'E':
                s = _("Encrypted connection");
                break;

            case 'X':
                s = _("Peer was found through Peer Exchange (PEX)");
                break;

            case 'H':
                s = _("Peer was found through DHT");
                break;

            case 'I':
                s = _("Peer is an incoming connection");
                break;

            case 'T':
                s = _("Peer is connected over µTP");
                break;

            default:
                g_assert_not_reached();
            }

            if (s != nullptr)
            {
                gstr << ch << ": " << s << '\n';
            }
        }

        auto str = gstr.str();
        if (!str.empty()) /* remove the last linefeed */
        {
            str.resize(str.size() - 1);
        }

        tooltip->set_markup(str);

        show_tip = true;
    }

    return show_tip;
}

namespace
{

void setPeerViewColumns(Gtk::TreeView* peer_view)
{
    std::vector<Gtk::TreeModelColumnBase const*> view_columns;
    Gtk::TreeViewColumn* c = nullptr;
    bool const more = gtr_pref_flag_get(TR_KEY_show_extra_peer_details);

    view_columns.push_back(&peer_cols.encryption_stock_id);
    view_columns.push_back(&peer_cols.upload_rate_string);

    if (more)
    {
        view_columns.push_back(&peer_cols.upload_request_count_string);
    }

    view_columns.push_back(&peer_cols.download_rate_string);

    if (more)
    {
        view_columns.push_back(&peer_cols.download_request_count_string);
    }

    if (more)
    {
        view_columns.push_back(&peer_cols.blocks_downloaded_count_string);
    }

    if (more)
    {
        view_columns.push_back(&peer_cols.blocks_uploaded_count_string);
    }

    if (more)
    {
        view_columns.push_back(&peer_cols.reqs_cancelled_by_client_count_string);
    }

    if (more)
    {
        view_columns.push_back(&peer_cols.reqs_cancelled_by_peer_count_string);
    }

    view_columns.push_back(&peer_cols.progress);
    view_columns.push_back(&peer_cols.flags);
    view_columns.push_back(&peer_cols.address);
    view_columns.push_back(&peer_cols.client);

    /* remove any existing columns */
    peer_view->remove_all_columns();

    for (auto const* const col : view_columns)
    {
        auto const* sort_col = col;

        if (*col == peer_cols.address)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Address"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.address_collated;
        }
        else if (*col == peer_cols.progress)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererProgress>();
            // % is percent done
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("%"), *r);
            c->add_attribute(r->property_text(), *col);
        }
        else if (*col == peer_cols.encryption_stock_id)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererPixbuf>();
            r->property_xalign() = 0.0F;
            r->property_yalign() = 0.5F;
            c = Gtk::make_managed<Gtk::TreeViewColumn>(Glib::ustring(), *r);
            c->add_attribute(r->property_icon_name(), *col);
            c->set_sizing(TR_GTK_TREE_VIEW_COLUMN_SIZING(FIXED));
            c->set_fixed_width(20);
        }
        else if (*col == peer_cols.download_request_count_string)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Dn Reqs"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.download_request_count_number;
        }
        else if (*col == peer_cols.upload_request_count_string)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Up Reqs"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.upload_request_count_number;
        }
        else if (*col == peer_cols.blocks_downloaded_count_string)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Dn Blocks"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.blocks_downloaded_count_number;
        }
        else if (*col == peer_cols.blocks_uploaded_count_string)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Up Blocks"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.blocks_uploaded_count_number;
        }
        else if (*col == peer_cols.reqs_cancelled_by_client_count_string)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("We Cancelled"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.reqs_cancelled_by_client_count_number;
        }
        else if (*col == peer_cols.reqs_cancelled_by_peer_count_string)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("They Cancelled"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.reqs_cancelled_by_peer_count_number;
        }
        else if (*col == peer_cols.download_rate_string)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            r->property_xalign() = 1.0F;
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Down"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.download_rate_double;
        }
        else if (*col == peer_cols.upload_rate_string)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            r->property_xalign() = 1.0F;
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Up"), *r);
            c->add_attribute(r->property_text(), *col);
            sort_col = &peer_cols.upload_rate_double;
        }
        else if (*col == peer_cols.client)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Client"), *r);
            c->add_attribute(r->property_text(), *col);
        }
        else if (*col == peer_cols.flags)
        {
            auto* r = Gtk::make_managed<Gtk::CellRendererText>();
            c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Flags"), *r);
            c->add_attribute(r->property_text(), *col);
        }
        else
        {
            std::abort();
        }

        c->set_resizable(false);
        c->set_sort_column(*sort_col);
        peer_view->append_column(*c);
    }

    /* the 'expander' column has a 10-pixel margin on the left
       that doesn't look quite correct in any of these columns...
       so create a non-visible column and assign it as the
       'expander column. */
    c = Gtk::make_managed<Gtk::TreeViewColumn>();
    c->set_visible(false);
    peer_view->append_column(*c);
    peer_view->set_expander_column(*c);
}

} // namespace

void DetailsDialog::Impl::onMorePeerInfoToggled()
{
    tr_quark const key = TR_KEY_show_extra_peer_details;
    bool const value = more_peer_details_check_->get_active();
    core_->set_pref(key, value);
    setPeerViewColumns(peer_view_);
}

void DetailsDialog::Impl::peer_page_init(Glib::RefPtr<Gtk::Builder> const& builder)
{
    /* webseeds */

    webseed_store_ = Gtk::ListStore::create(webseed_cols);
    auto* v = gtr_get_widget<Gtk::TreeView>(builder, "webseeds_view");
    v->set_model(webseed_store_);
    setup_item_view_button_event_handling(
        *v,
        {},
        [v](double view_x, double view_y) { return on_item_view_button_released(*v, view_x, view_y); });

    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        r->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(END);
        auto* c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Web Seeds"), *r);
        c->add_attribute(r->property_text(), webseed_cols.url);
        c->set_expand(true);
        c->set_sort_column(webseed_cols.url);
        v->append_column(*c);
    }

    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        auto* c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Down"), *r);
        c->add_attribute(r->property_text(), webseed_cols.download_rate_string);
        c->set_sort_column(webseed_cols.download_rate_double);
        v->append_column(*c);
    }

    /* peers */

    peer_store_ = Gtk::ListStore::create(peer_cols);
    auto m = Gtk::TreeModelSort::create(peer_store_);
    m->set_sort_column(peer_cols.progress, TR_GTK_SORT_TYPE(DESCENDING));

    peer_view_->set_model(m);
    peer_view_->set_has_tooltip(true);
    peer_view_->signal_query_tooltip().connect(sigc::mem_fun(*this, &Impl::onPeerViewQueryTooltip), false);
    setup_item_view_button_event_handling(
        *peer_view_,
        {},
        [this](double view_x, double view_y) { return on_item_view_button_released(*peer_view_, view_x, view_y); });

    setPeerViewColumns(peer_view_);

    more_peer_details_check_->set_active(gtr_pref_flag_get(TR_KEY_show_extra_peer_details));
    more_peer_details_check_->signal_toggled().connect(sigc::mem_fun(*this, &Impl::onMorePeerInfoToggled));
}

/****
*****
*****  TRACKER
*****
****/

namespace
{

auto constexpr ErrMarkupBegin = "<span color='red'>"sv;
auto constexpr ErrMarkupEnd = "</span>"sv;
auto constexpr TimeoutMarkupBegin = "<span color='#246'>"sv;
auto constexpr TimeoutMarkupEnd = "</span>"sv;
auto constexpr SuccessMarkupBegin = "<span color='#080'>"sv;
auto constexpr SuccessMarkupEnd = "</span>"sv;

std::array<std::string_view, 3> const text_dir_mark = { ""sv, "\u200E"sv, "\u200F"sv };

void appendAnnounceInfo(tr_tracker_view const& tracker, time_t const now, Gtk::TextDirection direction, std::ostream& gstr)
{
    auto const dir_mark = text_dir_mark.at(static_cast<int>(direction));

    if (tracker.hasAnnounced && tracker.announceState != TR_TRACKER_INACTIVE)
    {
        gstr << '\n';
        gstr << dir_mark;
        auto const time_span_ago = tr_format_time_relative(now, tracker.lastAnnounceTime);

        if (tracker.lastAnnounceSucceeded)
        {
            gstr << fmt::format(
                // {markup_begin} and {markup_end} should surround the peer text
                ngettext(
                    "Got a list of {markup_begin}{peer_count} peer{markup_end} {time_span_ago}",
                    "Got a list of {markup_begin}{peer_count} peers{markup_end} {time_span_ago}",
                    tracker.lastAnnouncePeerCount),
                fmt::arg("markup_begin", SuccessMarkupBegin),
                fmt::arg("peer_count", tracker.lastAnnouncePeerCount),
                fmt::arg("markup_end", SuccessMarkupEnd),
                fmt::arg("time_span_ago", time_span_ago));
        }
        else if (tracker.lastAnnounceTimedOut)
        {
            gstr << fmt::format(
                // {markup_begin} and {markup_end} should surround the time_span
                _("Peer list request {markup_begin}timed out {time_span_ago}{markup_end}; will retry"),
                fmt::arg("markup_begin", TimeoutMarkupBegin),
                fmt::arg("time_span_ago", time_span_ago),
                fmt::arg("markup_end", TimeoutMarkupEnd));
        }
        else
        {
            gstr << fmt::format(
                // {markup_begin} and {markup_end} should surround the error
                _("Got an error '{markup_begin}{error}{markup_end}' {time_span_ago}"),
                fmt::arg("markup_begin", ErrMarkupBegin),
                fmt::arg("error", Glib::Markup::escape_text(std::data(tracker.lastAnnounceResult))),
                fmt::arg("markup_end", ErrMarkupEnd),
                fmt::arg("time_span_ago", time_span_ago));
        }
    }

    switch (tracker.announceState)
    {
    case TR_TRACKER_INACTIVE:
        gstr << '\n';
        gstr << dir_mark;
        gstr << _("No updates scheduled");
        break;

    case TR_TRACKER_WAITING:
        gstr << '\n';
        gstr << dir_mark;
        gstr << fmt::format(
            _("Asking for more peers {time_span_from_now}"),
            fmt::arg("time_span_from_now", tr_format_time_relative(now, tracker.nextAnnounceTime)));
        break;

    case TR_TRACKER_QUEUED:
        gstr << '\n';
        gstr << dir_mark;
        gstr << _("Queued to ask for more peers");
        break;

    case TR_TRACKER_ACTIVE:
        gstr << '\n';
        gstr << dir_mark;
        gstr << fmt::format(
            // {markup_begin} and {markup_end} should surround time_span_ago
            _("Asked for more peers {markup_begin}{time_span_ago}{markup_end}"),
            fmt::arg("markup_begin", "<small>"),
            fmt::arg("time_span_ago", tr_format_time_relative(now, tracker.lastAnnounceStartTime)),
            fmt::arg("markup_end", "</small>"));
        break;

    default:
        g_assert_not_reached();
    }
}

void appendScrapeInfo(tr_tracker_view const& tracker, time_t const now, Gtk::TextDirection direction, std::ostream& gstr)
{
    auto const dir_mark = text_dir_mark.at(static_cast<int>(direction));

    if (tracker.hasScraped)
    {
        gstr << '\n';
        gstr << dir_mark;
        auto const time_span_ago = tr_format_time_relative(now, tracker.lastScrapeTime);

        if (tracker.lastScrapeSucceeded)
        {
            gstr << fmt::format(
                // {markup_begin} and {markup_end} should surround the seeder/leecher text
                _("Tracker had {markup_begin}{seeder_count} {seeder_or_seeders} and {leecher_count} {leecher_or_leechers}{markup_end} {time_span_ago}"),
                fmt::arg("seeder_count", tracker.seederCount),
                fmt::arg("seeder_or_seeders", ngettext("seeder", "seeders", tracker.seederCount)),
                fmt::arg("leecher_count", tracker.leecherCount),
                fmt::arg("leecher_or_leechers", ngettext("leecher", "leechers", tracker.leecherCount)),
                fmt::arg("time_span_ago", time_span_ago),
                fmt::arg("markup_begin", SuccessMarkupBegin),
                fmt::arg("markup_end", SuccessMarkupEnd));
        }
        else
        {
            gstr << fmt::format(
                // {markup_begin} and {markup_end} should surround the error text
                _("Got a scrape error '{markup_begin}{error}{markup_end}' {time_span_ago}"),
                fmt::arg("error", Glib::Markup::escape_text(std::data(tracker.lastScrapeResult))),
                fmt::arg("time_span_ago", time_span_ago),
                fmt::arg("markup_begin", ErrMarkupBegin),
                fmt::arg("markup_end", ErrMarkupEnd));
        }
    }

    switch (tracker.scrapeState)
    {
    case TR_TRACKER_INACTIVE:
        break;

    case TR_TRACKER_WAITING:
        gstr << '\n';
        gstr << dir_mark;
        gstr << fmt::format(
            _("Asking for peer counts in {time_span_from_now}"),
            fmt::arg("time_span_from_now", tr_format_time_relative(now, tracker.nextScrapeTime)));
        break;

    case TR_TRACKER_QUEUED:
        gstr << '\n';
        gstr << dir_mark;
        gstr << _("Queued to ask for peer counts");
        break;

    case TR_TRACKER_ACTIVE:
        gstr << '\n';
        gstr << dir_mark;
        gstr << fmt::format(
            _("Asked for peer counts {markup_begin}{time_span_ago}{markup_end}"),
            fmt::arg("markup_begin", "<small>"),
            fmt::arg("time_span_ago", tr_format_time_relative(now, tracker.lastScrapeStartTime)),
            fmt::arg("markup_end", "</small>"));
        break;

    default:
        g_assert_not_reached();
    }
}

void buildTrackerSummary(
    std::ostream& gstr,
    std::string const& key,
    tr_tracker_view const& tracker,
    bool showScrape,
    Gtk::TextDirection direction)
{
    // hostname
    gstr << text_dir_mark.at(static_cast<int>(direction));
    gstr << (tracker.isBackup ? "<i>" : "<b>");
    gstr << Glib::Markup::escape_text(
        !key.empty() ? fmt::format(FMT_STRING("{:s} - {:s}"), tracker.host_and_port, key) : tracker.host_and_port);
    gstr << (tracker.isBackup ? "</i>" : "</b>");

    if (!tracker.isBackup)
    {
        time_t const now = time(nullptr);

        appendAnnounceInfo(tracker, now, direction, gstr);

        if (showScrape)
        {
            appendScrapeInfo(tracker, now, direction, gstr);
        }
    }
}

class TrackerModelColumns : public Gtk::TreeModelColumnRecord
{
public:
    TrackerModelColumns() noexcept
    {
        add(torrent_id);
        add(text);
        add(is_backup);
        add(tracker_id);
        add(favicon);
        add(was_updated);
        add(key);
    }

    Gtk::TreeModelColumn<tr_torrent_id_t> torrent_id;
    Gtk::TreeModelColumn<Glib::ustring> text;
    Gtk::TreeModelColumn<bool> is_backup;
    Gtk::TreeModelColumn<int> tracker_id;
    Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> favicon;
    Gtk::TreeModelColumn<bool> was_updated;
    Gtk::TreeModelColumn<std::string> key;
};

TrackerModelColumns const tracker_cols;

} // namespace

bool DetailsDialog::Impl::trackerVisibleFunc(Gtk::TreeModel::const_iterator const& iter)
{
    /* show all */
    if (all_check_->get_active())
    {
        return true;
    }

    /* don't show the backups... */
    return !iter->get_value(tracker_cols.is_backup);
}

tr_torrent_id_t DetailsDialog::Impl::tracker_list_get_current_torrent_id() const
{
    // if there's only one torrent in the dialog, always use it
    if (ids_.size() == 1)
    {
        return ids_.front();
    }

    // otherwise, use the selected tracker's torrent
    auto const sel = tracker_view_->get_selection();
    if (auto const iter = sel->get_selected(); iter)
    {
        return iter->get_value(tracker_cols.torrent_id);
    }

    return -1;
}

tr_torrent* DetailsDialog::Impl::tracker_list_get_current_torrent() const
{
    return core_->find_torrent(tracker_list_get_current_torrent_id());
}

namespace
{

void favicon_ready_cb(Glib::RefPtr<Gdk::Pixbuf> const* pixbuf, Gtk::TreeRowReference& reference)
{
    if (pixbuf != nullptr && *pixbuf != nullptr)
    {
        auto const path = reference.get_path();
        auto const model = reference.get_model();

        if (auto const iter = model->get_iter(path); iter)
        {
            (*iter)[tracker_cols.favicon] = *pixbuf;
        }
    }
}

} // namespace

void DetailsDialog::Impl::refreshTracker(std::vector<tr_torrent*> const& torrents)
{
    std::ostringstream gstr;
    auto& hash = tracker_hash_;
    auto const& store = tracker_store_;
    bool const showScrape = scrape_check_->get_active();

    /* step 1: get all the trackers */
    auto trackers = std::multimap<tr_torrent const*, tr_tracker_view>{};
    for (auto const* tor : torrents)
    {
        for (size_t i = 0, n = tr_torrentTrackerCount(tor); i < n; ++i)
        {
            trackers.emplace(tor, tr_torrentTracker(tor, i));
        }
    }

    /* step 2: mark all the trackers in the list as not-updated */
    for (auto& row : store->children())
    {
        row[tracker_cols.was_updated] = false;
    }

    /* step 3: add / update trackers */
    for (auto const& [tor, tracker] : trackers)
    {
        auto const torrent_id = tr_torrentId(tor);

        // build the key to find the row
        gstr.str({});
        gstr << torrent_id << '\t' << tracker.tier << '\t' << tracker.announce;
        if (hash.find(gstr.str()) == hash.end())
        {
            // if we didn't have that row, add it
            auto const iter = store->append();
            (*iter)[tracker_cols.torrent_id] = torrent_id;
            (*iter)[tracker_cols.tracker_id] = tracker.id;
            (*iter)[tracker_cols.key] = gstr.str();

            auto const p = store->get_path(iter);
            hash.try_emplace(gstr.str(), Gtk::TreeRowReference(store, p));
            core_->favicon_cache().load(
                tracker.announce,
                [ref = Gtk::TreeRowReference(store, p)](auto const* pixbuf_refptr) mutable
                { favicon_ready_cb(pixbuf_refptr, ref); });
        }
    }

    /* step 4: update the rows */
    auto const summary_name = std::string(std::size(torrents) == 1 ? tr_torrentName(torrents.front()) : "");
    for (auto const& [tor, tracker] : trackers)
    {
        auto const torrent_id = tr_torrentId(tor);

        // build the key to find the row
        gstr.str({});
        gstr << torrent_id << '\t' << tracker.tier << '\t' << tracker.announce;
        auto const iter = store->get_iter(hash.at(gstr.str()).get_path());

        // update the row
        gstr.str({});
        buildTrackerSummary(gstr, summary_name, tracker, showScrape, dialog_.get_direction());
        (*iter)[tracker_cols.text] = gstr.str();
        (*iter)[tracker_cols.is_backup] = tracker.isBackup;
        (*iter)[tracker_cols.tracker_id] = tracker.id;
        (*iter)[tracker_cols.was_updated] = true;
    }

    /* step 5: remove trackers that have disappeared */
    if (auto iter = store->children().begin(); iter)
    {
        while (iter)
        {
            if (iter->get_value(tracker_cols.was_updated))
            {
                ++iter;
            }
            else
            {
                auto const key = iter->get_value(tracker_cols.key);
                hash.erase(key);
                iter = store->erase(iter);
            }
        }
    }

    edit_trackers_button_->set_sensitive(tracker_list_get_current_torrent_id() > 0);
}

void DetailsDialog::Impl::refreshFiles(std::vector<tr_torrent*> const& torrents)
{
    if (torrents.size() == 1)
    {
        file_list_->set_torrent(tr_torrentId(torrents.front()));
        file_list_->show();
        file_label_->hide();
    }
    else
    {
        file_list_->clear();
        file_list_->hide();
        file_label_->show();
    }
}

void DetailsDialog::Impl::onScrapeToggled()
{
    tr_quark const key = TR_KEY_show_tracker_scrapes;
    bool const value = scrape_check_->get_active();
    core_->set_pref(key, value);
    refresh();
}

void DetailsDialog::Impl::onBackupToggled()
{
    tr_quark const key = TR_KEY_show_backup_trackers;
    bool const value = all_check_->get_active();
    core_->set_pref(key, value);
    refresh();
}

namespace
{

class EditTrackersDialog : public Gtk::Dialog
{
public:
    EditTrackersDialog(
        BaseObjectType* cast_item,
        Glib::RefPtr<Gtk::Builder> const& builder,
        DetailsDialog& parent,
        Glib::RefPtr<Session> const& core,
        tr_torrent const* torrent);
    ~EditTrackersDialog() override = default;

    TR_DISABLE_COPY_MOVE(EditTrackersDialog)

    static std::unique_ptr<EditTrackersDialog> create(
        DetailsDialog& parent,
        Glib::RefPtr<Session> const& core,
        tr_torrent const* tor);

private:
    void on_response(int response) override;

private:
    DetailsDialog& parent_;
    Glib::RefPtr<Session> const core_;
    tr_torrent_id_t const torrent_id_;
    Gtk::TextView* const urls_view_;
};

EditTrackersDialog::EditTrackersDialog(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    DetailsDialog& parent,
    Glib::RefPtr<Session> const& core,
    tr_torrent const* torrent)
    : Gtk::Dialog(cast_item)
    , parent_(parent)
    , core_(core)
    , torrent_id_(tr_torrentId(torrent))
    , urls_view_(gtr_get_widget<Gtk::TextView>(builder, "urls_view"))
{
    set_title(fmt::format(_("{torrent_name} - Edit Trackers"), fmt::arg("torrent_name", tr_torrentName(torrent))));
    set_transient_for(parent);

    urls_view_->get_buffer()->set_text(tr_torrentGetTrackerList(torrent));
}

std::unique_ptr<EditTrackersDialog> EditTrackersDialog::create(
    DetailsDialog& parent,
    Glib::RefPtr<Session> const& core,
    tr_torrent const* torrent)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("EditTrackersDialog.ui"));
    return std::unique_ptr<EditTrackersDialog>(
        gtr_get_widget_derived<EditTrackersDialog>(builder, "EditTrackersDialog", parent, core, torrent));
}

void EditTrackersDialog::on_response(int response)
{
    bool do_destroy = true;

    if (response == TR_GTK_RESPONSE_TYPE(ACCEPT))
    {
        auto const text_buffer = urls_view_->get_buffer();

        if (auto* const tor = core_->find_torrent(torrent_id_); tor != nullptr)
        {
            if (tr_torrentSetTrackerList(tor, text_buffer->get_text(false).c_str()))
            {
                parent_.refresh();
            }
            else
            {
                auto w = std::make_shared<Gtk::MessageDialog>(
                    *this,
                    _("List contains invalid URLs"),
                    false,
                    TR_GTK_MESSAGE_TYPE(ERROR),
                    TR_GTK_BUTTONS_TYPE(CLOSE),
                    true);
                w->set_secondary_text(_("Please correct the errors and try again."));
                w->signal_response().connect([w](int /*response*/) mutable { w.reset(); });
                w->show();

                do_destroy = false;
            }
        }
    }

    if (do_destroy)
    {
        close();
    }
}

} // namespace

void DetailsDialog::Impl::on_edit_trackers()
{
    if (auto const* const tor = tracker_list_get_current_torrent(); tor != nullptr)
    {
        auto d = std::shared_ptr<EditTrackersDialog>(EditTrackersDialog::create(dialog_, core_, tor));
        gtr_window_on_close(*d, [d]() mutable { d.reset(); });
        d->show();
    }
}

void DetailsDialog::Impl::on_tracker_list_selection_changed()
{
    int const n = tracker_view_->get_selection()->count_selected_rows();
    auto const* const tor = tracker_list_get_current_torrent();

    remove_tracker_button_->set_sensitive(n > 0);
    add_tracker_button_->set_sensitive(tor != nullptr);
    edit_trackers_button_->set_sensitive(tor != nullptr);
}

namespace
{

class AddTrackerDialog : public Gtk::Dialog
{
public:
    AddTrackerDialog(
        BaseObjectType* cast_item,
        Glib::RefPtr<Gtk::Builder> const& builder,
        DetailsDialog& parent,
        Glib::RefPtr<Session> const& core,
        tr_torrent const* torrent);
    ~AddTrackerDialog() override = default;

    TR_DISABLE_COPY_MOVE(AddTrackerDialog)

    static std::unique_ptr<AddTrackerDialog> create(
        DetailsDialog& parent,
        Glib::RefPtr<Session> const& core,
        tr_torrent const* tor);

private:
    void on_response(int response) override;

private:
    DetailsDialog& parent_;
    Glib::RefPtr<Session> const core_;
    tr_torrent_id_t const torrent_id_;
    Gtk::Entry* const url_entry_;
};

AddTrackerDialog::AddTrackerDialog(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    DetailsDialog& parent,
    Glib::RefPtr<Session> const& core,
    tr_torrent const* torrent)
    : Gtk::Dialog(cast_item)
    , parent_(parent)
    , core_(core)
    , torrent_id_(tr_torrentId(torrent))
    , url_entry_(gtr_get_widget<Gtk::Entry>(builder, "url_entry"))
{
    set_title(fmt::format(_("{torrent_name} - Add Tracker"), fmt::arg("torrent_name", tr_torrentName(torrent))));
    set_transient_for(parent);

    gtr_paste_clipboard_url_into_entry(*url_entry_);
}

std::unique_ptr<AddTrackerDialog> AddTrackerDialog::create(
    DetailsDialog& parent,
    Glib::RefPtr<Session> const& core,
    tr_torrent const* torrent)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("AddTrackerDialog.ui"));
    return std::unique_ptr<AddTrackerDialog>(
        gtr_get_widget_derived<AddTrackerDialog>(builder, "AddTrackerDialog", parent, core, torrent));
}

void AddTrackerDialog::on_response(int response)
{
    bool destroy = true;

    if (response == TR_GTK_RESPONSE_TYPE(ACCEPT))
    {
        auto const url = gtr_str_strip(url_entry_->get_text());

        if (!url.empty())
        {
            if (tr_urlIsValidTracker(url.c_str()))
            {
                auto top = tr_variant::make_map(2U);
                tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-set"sv);
                auto* const args = tr_variantDictAddDict(&top, TR_KEY_arguments, 2);
                tr_variantDictAddInt(args, TR_KEY_id, torrent_id_);
                auto* const trackers = tr_variantDictAddList(args, TR_KEY_trackerAdd, 1);
                tr_variantListAddStr(trackers, url.raw());

                core_->exec(&top);
                parent_.refresh();
            }
            else
            {
                gtr_unrecognized_url_dialog(*this, url);
                destroy = false;
            }
        }
    }

    if (destroy)
    {
        close();
    }
}

} // namespace

void DetailsDialog::Impl::on_tracker_list_add_button_clicked()
{
    if (auto const* const tor = tracker_list_get_current_torrent(); tor != nullptr)
    {
        auto d = std::shared_ptr<AddTrackerDialog>(AddTrackerDialog::create(dialog_, core_, tor));
        gtr_window_on_close(*d, [d]() mutable { d.reset(); });
        d->show();
    }
}

void DetailsDialog::Impl::on_tracker_list_remove_button_clicked()
{
    auto* v = tracker_view_;
    auto sel = v->get_selection();

    if (auto const iter = sel->get_selected(); iter)
    {
        auto const torrent_id = iter->get_value(tracker_cols.torrent_id);
        auto const tracker_id = iter->get_value(tracker_cols.tracker_id);

        auto top = tr_variant::make_map(2U);
        tr_variantDictAddStrView(&top, TR_KEY_method, "torrent-set"sv);
        auto* const args = tr_variantDictAddDict(&top, TR_KEY_arguments, 2);
        tr_variantDictAddInt(args, TR_KEY_id, torrent_id);
        auto* const trackers = tr_variantDictAddList(args, TR_KEY_trackerRemove, 1);
        tr_variantListAddInt(trackers, tracker_id);

        core_->exec(&top);
        refresh();
    }
}

void DetailsDialog::Impl::tracker_page_init(Glib::RefPtr<Gtk::Builder> const& /*builder*/)
{
    int const pad = (GUI_PAD + GUI_PAD_BIG) / 2;

    tracker_store_ = Gtk::ListStore::create(tracker_cols);

    trackers_filtered_ = Gtk::TreeModelFilter::create(tracker_store_);
    trackers_filtered_->set_visible_func(sigc::mem_fun(*this, &Impl::trackerVisibleFunc));

    tracker_view_->set_model(trackers_filtered_);
    setup_item_view_button_event_handling(
        *tracker_view_,
        [this](guint /*button*/, TrGdkModifierType /*state*/, double view_x, double view_y, bool context_menu_requested)
        { return on_item_view_button_pressed(*tracker_view_, view_x, view_y, context_menu_requested); },
        [this](double view_x, double view_y) { return on_item_view_button_released(*tracker_view_, view_x, view_y); });

    auto sel = tracker_view_->get_selection();
    sel->signal_changed().connect(sigc::mem_fun(*this, &Impl::on_tracker_list_selection_changed));

    auto* c = Gtk::make_managed<Gtk::TreeViewColumn>();
    c->set_title(_("Trackers"));
    tracker_view_->append_column(*c);

    {
        auto* r = Gtk::make_managed<Gtk::CellRendererPixbuf>();
        r->property_width() = 20 + (GUI_PAD_SMALL * 2);
        r->property_xpad() = GUI_PAD_SMALL;
        r->property_ypad() = pad;
        r->property_yalign() = 0.0F;
        c->pack_start(*r, false);
        c->add_attribute(r->property_pixbuf(), tracker_cols.favicon);
    }

    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        r->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(END);
        r->property_xpad() = GUI_PAD_SMALL;
        r->property_ypad() = pad;
        c->pack_start(*r, true);
        c->add_attribute(r->property_markup(), tracker_cols.text);
    }

    add_tracker_button_->signal_clicked().connect(sigc::mem_fun(*this, &Impl::on_tracker_list_add_button_clicked));
    edit_trackers_button_->signal_clicked().connect(sigc::mem_fun(*this, &Impl::on_edit_trackers));
    remove_tracker_button_->signal_clicked().connect(sigc::mem_fun(*this, &Impl::on_tracker_list_remove_button_clicked));

    scrape_check_->set_active(gtr_pref_flag_get(TR_KEY_show_tracker_scrapes));
    scrape_check_->signal_toggled().connect(sigc::mem_fun(*this, &Impl::onScrapeToggled));

    all_check_->set_active(gtr_pref_flag_get(TR_KEY_show_backup_trackers));
    all_check_->signal_toggled().connect(sigc::mem_fun(*this, &Impl::onBackupToggled));
}

/****
*****  DIALOG
****/

void DetailsDialog::Impl::refresh()
{
    auto const torrents = getTorrents();

    refreshInfo(torrents);
    refreshPeers(torrents);
    refreshTracker(torrents);
    refreshFiles(torrents);
    refreshOptions(torrents);

    if (torrents.empty())
    {
        dialog_.response(TR_GTK_RESPONSE_TYPE(CLOSE));
    }
}

void DetailsDialog::Impl::on_details_window_size_allocated()
{
    int w = 0;
    int h = 0;
#if GTKMM_CHECK_VERSION(4, 0, 0)
    dialog_.get_default_size(w, h);
#else
    dialog_.get_size(w, h);
#endif
    gtr_pref_int_set(TR_KEY_details_window_width, w);
    gtr_pref_int_set(TR_KEY_details_window_height, h);
}

DetailsDialog::Impl::~Impl()
{
    periodic_refresh_tag_.disconnect();
}

std::unique_ptr<DetailsDialog> DetailsDialog::create(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("DetailsDialog.ui"));
    return std::unique_ptr<DetailsDialog>(gtr_get_widget_derived<DetailsDialog>(builder, "DetailsDialog", parent, core));
}

DetailsDialog::DetailsDialog(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core)
    : Gtk::Dialog(cast_item)
    , impl_(std::make_unique<Impl>(*this, builder, core))
{
    set_transient_for(parent);
}

DetailsDialog::~DetailsDialog() = default;

DetailsDialog::Impl::Impl(DetailsDialog& dialog, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core)
    : dialog_(dialog)
    , core_(core)
    , honor_limits_check_(gtr_get_widget<Gtk::CheckButton>(builder, "honor_limits_check"))
    , up_limited_check_(gtr_get_widget<Gtk::CheckButton>(builder, "upload_limit_check"))
    , up_limit_sping_(gtr_get_widget<Gtk::SpinButton>(builder, "upload_limit_spin"))
    , down_limited_check_(gtr_get_widget<Gtk::CheckButton>(builder, "download_limit_check"))
    , down_limit_spin_(gtr_get_widget<Gtk::SpinButton>(builder, "download_limit_spin"))
    , bandwidth_combo_(gtr_get_widget<Gtk::ComboBox>(builder, "priority_combo"))
    , ratio_combo_(gtr_get_widget<Gtk::ComboBox>(builder, "ratio_limit_combo"))
    , ratio_spin_(gtr_get_widget<Gtk::SpinButton>(builder, "ratio_limit_spin"))
    , idle_combo_(gtr_get_widget<Gtk::ComboBox>(builder, "idle_limit_combo"))
    , idle_spin_(gtr_get_widget<Gtk::SpinButton>(builder, "idle_limit_spin"))
    , max_peers_spin_(gtr_get_widget<Gtk::SpinButton>(builder, "max_peers_spin"))
    , added_lb_(gtr_get_widget<Gtk::Label>(builder, "added_value_label"))
    , size_lb_(gtr_get_widget<Gtk::Label>(builder, "torrent_size_value_label"))
    , state_lb_(gtr_get_widget<Gtk::Label>(builder, "state_value_label"))
    , have_lb_(gtr_get_widget<Gtk::Label>(builder, "have_value_label"))
    , dl_lb_(gtr_get_widget<Gtk::Label>(builder, "downloaded_value_label"))
    , ul_lb_(gtr_get_widget<Gtk::Label>(builder, "uploaded_value_label"))
    , error_lb_(gtr_get_widget<Gtk::Label>(builder, "error_value_label"))
    , date_started_lb_(gtr_get_widget<Gtk::Label>(builder, "running_time_value_label"))
    , eta_lb_(gtr_get_widget<Gtk::Label>(builder, "remaining_time_value_label"))
    , last_activity_lb_(gtr_get_widget<Gtk::Label>(builder, "last_activity_value_label"))
    , hash_lb_(gtr_get_widget<Gtk::Label>(builder, "hash_value_label"))
    , privacy_lb_(gtr_get_widget<Gtk::Label>(builder, "privacy_value_label"))
    , origin_lb_(gtr_get_widget<Gtk::Label>(builder, "origin_value_label"))
    , destination_lb_(gtr_get_widget<Gtk::Label>(builder, "location_value_label"))
    , webseed_view_(gtr_get_widget<Gtk::ScrolledWindow>(builder, "webseeds_view_scroll"))
    , peer_view_(gtr_get_widget<Gtk::TreeView>(builder, "peers_view"))
    , more_peer_details_check_(gtr_get_widget<Gtk::CheckButton>(builder, "more_peer_details_check"))
    , add_tracker_button_(gtr_get_widget<Gtk::Button>(builder, "add_tracker_button"))
    , edit_trackers_button_(gtr_get_widget<Gtk::Button>(builder, "edit_tracker_button"))
    , remove_tracker_button_(gtr_get_widget<Gtk::Button>(builder, "remove_tracker_button"))
    , tracker_view_(gtr_get_widget<Gtk::TreeView>(builder, "trackers_view"))
    , scrape_check_(gtr_get_widget<Gtk::CheckButton>(builder, "more_tracker_details_check"))
    , all_check_(gtr_get_widget<Gtk::CheckButton>(builder, "backup_trackers_check"))
    , file_list_(gtr_get_widget_derived<FileList>(builder, "files_view_scroll", "files_view", core, 0))
    , file_label_(gtr_get_widget<Gtk::Label>(builder, "files_label"))
{
    /* return saved window size */
    auto const width = (int)gtr_pref_int_get(TR_KEY_details_window_width);
    auto const height = (int)gtr_pref_int_get(TR_KEY_details_window_height);
#if GTKMM_CHECK_VERSION(4, 0, 0)
    dialog_.set_default_size(width, height);
    dialog_.property_default_width().signal_changed().connect(sigc::mem_fun(*this, &Impl::on_details_window_size_allocated));
    dialog_.property_default_height().signal_changed().connect(sigc::mem_fun(*this, &Impl::on_details_window_size_allocated));
#else
    dialog_.resize(width, height);
    dialog_.signal_size_allocate().connect(sigc::hide<0>(sigc::mem_fun(*this, &Impl::on_details_window_size_allocated)));
#endif

    dialog_.signal_response().connect(sigc::hide<0>(sigc::mem_fun(dialog_, &DetailsDialog::close)));

    info_page_init(builder);
    peer_page_init(builder);
    tracker_page_init(builder);
    options_page_init(builder);

    periodic_refresh_tag_ = Glib::signal_timeout().connect_seconds(
        [this]() { return refresh(), true; },
        SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS);

    auto* const n = gtr_get_widget<Gtk::Notebook>(builder, "dialog_pages");
    n->set_current_page(last_page_);
    n->signal_switch_page().connect([](Gtk::Widget* /*page*/, guint page_number) { last_page_ = page_number; });
}

void DetailsDialog::set_torrents(std::vector<tr_torrent_id_t> const& ids)
{
    impl_->set_torrents(ids);
}

void DetailsDialog::refresh()
{
    impl_->refresh();
}

void DetailsDialog::Impl::set_torrents(std::vector<tr_torrent_id_t> const& ids)
{
    Glib::ustring title;
    auto const len = ids.size();

    ids_ = ids;

    if (len == 1)
    {
        int const id = ids.front();
        auto const* tor = core_->find_torrent(id);
        title = fmt::format(_("{torrent_name} Properties"), fmt::arg("torrent_name", tr_torrentName(tor)));
    }
    else
    {
        title = fmt::format(
            ngettext("Properties - {torrent_count:L} Torrent", "Properties - {torrent_count:L} Torrents", len),
            fmt::arg("torrent_count", len));
    }

    dialog_.set_title(title);

    refresh();
}
