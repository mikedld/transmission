// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "FilterBar.h"

#include "FilterListModel.hh"
#include "HigWorkarea.h" // GUI_PAD
#include "ListModelAdapter.h"
#include "Session.h" // torrent_cols
#include "Torrent.h"
#include "TorrentFilter.h"
#include "Utils.h"

#include <gdkmm/pixbuf.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/unicode.h>
#include <glibmm/ustring.h>
#include <gtkmm/cellrendererpixbuf.h>
#include <gtkmm/cellrenderertext.h>
#include <gtkmm/combobox.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/liststore.h>
#include <gtkmm/treemodel.h>
#include <gtkmm/treemodelcolumn.h>
#include <gtkmm/treemodelfilter.h>
#include <gtkmm/treerowreference.h>
#include <gtkmm/treestore.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gtkmm/filterlistmodel.h>
#endif

#include <fmt/core.h>

#include <algorithm> // std::transform()
#include <array>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace
{
using ActivityType = TorrentFilter::Activity;
using TrackerType = TorrentFilter::Tracker;

constexpr auto ActivitySeparator = static_cast<ActivityType>(-1);
constexpr auto TrackerSeparator = static_cast<TrackerType>(-1);
} // namespace

class FilterBar::Impl
{
    using FilterModel = IF_GTKMM4(Gtk::FilterListModel, Gtk::TreeModelFilter);

public:
    Impl(FilterBar& widget, Glib::RefPtr<Session> const& core);
    Impl(Impl&&) = delete;
    Impl(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    ~Impl();

    [[nodiscard]] Glib::RefPtr<FilterModel> get_filter_model() const;

private:
    template<typename T>
    T* get_template_child(char const* name) const;

    void activity_combo_box_init(Gtk::ComboBox& combo);
    static void render_activity_pixbuf_func(Gtk::CellRendererPixbuf& cell_renderer, Gtk::TreeModel::const_iterator const& iter);

    void tracker_combo_box_init(Gtk::ComboBox& combo);
    static void render_pixbuf_func(Gtk::CellRendererPixbuf& cell_renderer, Gtk::TreeModel::const_iterator const& iter);
    static void render_number_func(Gtk::CellRendererText& cell_renderer, Gtk::TreeModel::const_iterator const& iter);

    void update_filter_activity();
    void update_filter_tracker();
    void update_filter_text();

    bool activity_filter_model_update();

    bool tracker_filter_model_update();
    void favicon_ready_cb(Glib::RefPtr<Gdk::Pixbuf> const* pixbuf, Gtk::TreeModel::Path const& path);

    void update_filter_models(Torrent::ChangeFlags changes);
    void update_filter_models_idle(Torrent::ChangeFlags changes);

    void update_count_label_idle();
    bool update_count_label();

    static Glib::RefPtr<Gtk::ListStore> activity_filter_model_new();
    static void status_model_update_count(Gtk::TreeModel::iterator const& iter, int n);
    static bool activity_is_it_a_separator(Gtk::TreeModel::const_iterator const& iter);

    static Glib::RefPtr<Gtk::TreeStore> tracker_filter_model_new();
    static void tracker_model_update_count(Gtk::TreeModel::iterator const& iter, int n);
    static bool is_it_a_separator(Gtk::TreeModel::const_iterator const& iter);

    static Glib::ustring get_name_from_host(std::string const& host);

    static Gtk::CellRendererText* number_renderer_new();

private:
    FilterBar& widget_;
    Glib::RefPtr<Session> const core_;

    Glib::RefPtr<Gtk::ListStore> const activity_model_;
    Glib::RefPtr<Gtk::TreeStore> const tracker_model_;

    Gtk::ComboBox* activity_ = nullptr;
    Gtk::ComboBox* tracker_ = nullptr;
    Gtk::Entry* entry_ = nullptr;
    Gtk::Label* show_lb_ = nullptr;
    Glib::RefPtr<TorrentFilter> filter_ = TorrentFilter::create();
    Glib::RefPtr<FilterListModel<Torrent>> filter_model_;

    sigc::connection update_count_label_tag_;
    sigc::connection update_filter_models_tag_;
    sigc::connection update_filter_models_on_add_remove_tag_;
    sigc::connection update_filter_models_on_change_tag_;
};

// --- TRACKERS

namespace
{
class TrackerFilterModelColumns : public Gtk::TreeModelColumnRecord
{
public:
    TrackerFilterModelColumns() noexcept
    {
        add(displayname);
        add(count);
        add(type);
        add(sitename);
        add(pixbuf);
    }

    Gtk::TreeModelColumn<Glib::ustring> displayname; /* human-readable name; ie, Legaltorrents */
    Gtk::TreeModelColumn<int> count; /* how many matches there are */
    Gtk::TreeModelColumn<TrackerType> type;
    Gtk::TreeModelColumn<Glib::ustring> sitename; // pattern-matching text; see tr_parsed_url.sitename
    Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> pixbuf;
};

TrackerFilterModelColumns const tracker_filter_cols;

} // namespace

/* human-readable name; ie, Legaltorrents */
Glib::ustring FilterBar::Impl::get_name_from_host(std::string const& host)
{
    std::string name = host;

    if (!name.empty())
    {
        name.front() = Glib::Ascii::toupper(name.front());
    }

    return name;
}

void FilterBar::Impl::tracker_model_update_count(Gtk::TreeModel::iterator const& iter, int n)
{
    if (n != iter->get_value(tracker_filter_cols.count))
    {
        iter->set_value(tracker_filter_cols.count, n);
    }
}

void FilterBar::Impl::favicon_ready_cb(Glib::RefPtr<Gdk::Pixbuf> const* pixbuf, Gtk::TreeModel::Path const& path)
{
    if (pixbuf != nullptr && *pixbuf != nullptr)
    {
        if (auto const iter = tracker_model_->get_iter(path); iter)
        {
            iter->set_value(tracker_filter_cols.pixbuf, *pixbuf);
        }
    }
}

bool FilterBar::Impl::tracker_filter_model_update()
{
    struct site_info
    {
        int count = 0;
        std::string host;
        std::string sitename;
        std::string announce_url;

        bool operator<(site_info const& that) const
        {
            return sitename < that.sitename;
        }
    };

    auto const torrents_model = core_->get_model();

    /* Walk through all the torrents, tallying how many matches there are
     * for the various categories. Also make a sorted list of all tracker
     * hosts s.t. we can merge it with the existing list */
    auto n_torrents = 0;
    auto site_infos = std::unordered_map<std::string /*site*/, site_info>{};
    for (auto i = 0U, count = torrents_model->get_n_items(); i < count; ++i)
    {
        auto const torrent = gtr_ptr_dynamic_cast<Torrent>(torrents_model->get_object(i));
        if (torrent == nullptr)
        {
            continue;
        }

        auto const& raw_torrent = torrent->get_underlying();

        auto site_to_host_and_announce = std::map<std::string, std::pair<std::string, std::string>>{};
        for (size_t j = 0, n = tr_torrentTrackerCount(&raw_torrent); j < n; ++j)
        {
            auto const view = tr_torrentTracker(&raw_torrent, j);
            site_to_host_and_announce.try_emplace(std::data(view.sitename), view.host_and_port, view.announce);
        }

        for (auto const& [sitename, host_and_announce] : site_to_host_and_announce)
        {
            auto& info = site_infos[sitename];
            info.host = host_and_announce.first;
            info.announce_url = host_and_announce.second;
            info.sitename = sitename;
            ++info.count;
        }

        ++n_torrents;
    }

    auto const n_sites = std::size(site_infos);
    auto sites_v = std::vector<site_info>(n_sites);
    std::transform(std::begin(site_infos), std::end(site_infos), std::begin(sites_v), [](auto const& it) { return it.second; });
    std::sort(std::begin(sites_v), std::end(sites_v));

    // update the "all" count
    auto iter = tracker_model_->children().begin();
    if (iter)
    {
        tracker_model_update_count(iter, n_torrents);
    }

    // offset past the "All" and the separator
    ++iter;
    ++iter;

    size_t i = 0;
    for (;;)
    {
        // are we done yet?
        bool const new_sites_done = i >= n_sites;
        bool const old_sites_done = !iter;
        if (new_sites_done && old_sites_done)
        {
            break;
        }

        // decide what to do
        bool remove_row = false;
        bool insert_row = false;
        if (new_sites_done)
        {
            remove_row = true;
        }
        else if (old_sites_done)
        {
            insert_row = true;
        }
        else
        {
            auto const sitename = iter->get_value(tracker_filter_cols.sitename);
            int const cmp = sitename.raw().compare(sites_v.at(i).sitename);

            if (cmp < 0)
            {
                remove_row = true;
            }
            else if (cmp > 0)
            {
                insert_row = true;
            }
        }

        // do something
        if (remove_row)
        {
            iter = tracker_model_->erase(iter);
        }
        else if (insert_row)
        {
            auto const& site = sites_v.at(i);
            auto const add = tracker_model_->insert(iter);
            add->set_value(tracker_filter_cols.sitename, Glib::ustring{ site.sitename });
            add->set_value(tracker_filter_cols.displayname, get_name_from_host(site.sitename));
            add->set_value(tracker_filter_cols.count, site.count);
            add->set_value(tracker_filter_cols.type, TrackerType::HOST);
            auto path = tracker_model_->get_path(add);
            core_->favicon_cache().load(
                site.announce_url,
                [this, path = std::move(path)](auto const* pixbuf) { favicon_ready_cb(pixbuf, path); });
            ++i;
        }
        else // update row
        {
            tracker_model_update_count(iter, sites_v.at(i).count);
            ++iter;
            ++i;
        }
    }

    return false;
}

Glib::RefPtr<Gtk::TreeStore> FilterBar::Impl::tracker_filter_model_new()
{
    auto store = Gtk::TreeStore::create(tracker_filter_cols);

    auto iter = store->append();
    iter->set_value(tracker_filter_cols.displayname, Glib::ustring(_("All")));
    iter->set_value(tracker_filter_cols.type, TrackerType::ALL);

    iter = store->append();
    iter->set_value(tracker_filter_cols.type, TrackerSeparator);

    return store;
}

bool FilterBar::Impl::is_it_a_separator(Gtk::TreeModel::const_iterator const& iter)
{
    return iter->get_value(tracker_filter_cols.type) == TrackerSeparator;
}

void FilterBar::Impl::render_pixbuf_func(Gtk::CellRendererPixbuf& cell_renderer, Gtk::TreeModel::const_iterator const& iter)
{
    cell_renderer.property_width() = TrackerType{ iter->get_value(tracker_filter_cols.type) } == TrackerType::HOST ? 20 : 0;
}

void FilterBar::Impl::render_number_func(Gtk::CellRendererText& cell_renderer, Gtk::TreeModel::const_iterator const& iter)
{
    auto const count = iter->get_value(tracker_filter_cols.count);
    cell_renderer.property_text() = count >= 0 ? fmt::format("{:L}", count) : "";
}

Gtk::CellRendererText* FilterBar::Impl::number_renderer_new()
{
    auto* r = Gtk::make_managed<Gtk::CellRendererText>();

    r->property_alignment() = TR_PANGO_ALIGNMENT(RIGHT);
    r->property_weight() = TR_PANGO_WEIGHT(ULTRALIGHT);
    r->property_xalign() = 1.0;
    r->property_xpad() = GUI_PAD;

    return r;
}

void FilterBar::Impl::tracker_combo_box_init(Gtk::ComboBox& combo)
{
    combo.set_model(tracker_model_);
    combo.set_row_separator_func(sigc::hide<0>(&Impl::is_it_a_separator));
    combo.set_active(0);

    {
        auto* r = Gtk::make_managed<Gtk::CellRendererPixbuf>();
        combo.pack_start(*r, false);
        combo.set_cell_data_func(*r, [r](auto const& iter) { render_pixbuf_func(*r, iter); });
        combo.add_attribute(r->property_pixbuf(), tracker_filter_cols.pixbuf);
    }

    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        combo.pack_start(*r, false);
        combo.add_attribute(r->property_text(), tracker_filter_cols.displayname);
    }

    {
        auto* r = number_renderer_new();
        combo.pack_end(*r, true);
        combo.set_cell_data_func(*r, [r](auto const& iter) { render_number_func(*r, iter); });
    }
}

namespace
{

/***
****
****  ACTIVITY
****
***/

class ActivityFilterModelColumns : public Gtk::TreeModelColumnRecord
{
public:
    ActivityFilterModelColumns() noexcept
    {
        add(name);
        add(count);
        add(type);
        add(icon_name);
    }

    Gtk::TreeModelColumn<Glib::ustring> name;
    Gtk::TreeModelColumn<int> count;
    Gtk::TreeModelColumn<ActivityType> type;
    Gtk::TreeModelColumn<Glib::ustring> icon_name;
};

ActivityFilterModelColumns const activity_filter_cols;

} // namespace

bool FilterBar::Impl::activity_is_it_a_separator(Gtk::TreeModel::const_iterator const& iter)
{
    return iter->get_value(activity_filter_cols.type) == ActivitySeparator;
}

void FilterBar::Impl::status_model_update_count(Gtk::TreeModel::iterator const& iter, int n)
{
    if (n != iter->get_value(activity_filter_cols.count))
    {
        iter->set_value(activity_filter_cols.count, n);
    }
}

bool FilterBar::Impl::activity_filter_model_update()
{
    auto const torrents_model = core_->get_model();

    for (auto& row : activity_model_->children())
    {
        auto const type = row.get_value(activity_filter_cols.type);
        if (type == ActivitySeparator)
        {
            continue;
        }

        auto hits = 0;

        for (auto i = 0U, count = torrents_model->get_n_items(); i < count; ++i)
        {
            auto const torrent = gtr_ptr_dynamic_cast<Torrent>(torrents_model->get_object(i));
            if (torrent != nullptr && TorrentFilter::match_activity(*torrent, static_cast<ActivityType>(type)))
            {
                ++hits;
            }
        }

        status_model_update_count(TR_GTK_TREE_MODEL_CHILD_ITER(row), hits);
    }

    return false;
}

Glib::RefPtr<Gtk::ListStore> FilterBar::Impl::activity_filter_model_new()
{
    struct FilterTypeInfo
    {
        ActivityType type;
        char const* context;
        char const* name;
        char const* icon_name;
    };

    static auto constexpr types = std::array<FilterTypeInfo, 9>({ {
        { ActivityType::ALL, nullptr, N_("All"), nullptr },
        { ActivityType{ -1 }, nullptr, nullptr, nullptr },
        { ActivityType::ACTIVE, nullptr, N_("Active"), "system-run" },
        { ActivityType::DOWNLOADING, "Verb", NC_("Verb", "Downloading"), "network-receive" },
        { ActivityType::SEEDING, "Verb", NC_("Verb", "Seeding"), "network-transmit" },
        { ActivityType::PAUSED, nullptr, N_("Paused"), "media-playback-pause" },
        { ActivityType::FINISHED, nullptr, N_("Finished"), "media-playback-stop" },
        { ActivityType::VERIFYING, "Verb", NC_("Verb", "Verifying"), "view-refresh" },
        { ActivityType::ERROR, nullptr, N_("Error"), "dialog-error" },
    } });

    auto store = Gtk::ListStore::create(activity_filter_cols);

    for (auto const& type : types)
    {
        auto const name = type.name != nullptr ?
            Glib::ustring(type.context != nullptr ? g_dpgettext2(nullptr, type.context, type.name) : _(type.name)) :
            Glib::ustring();
        auto const iter = store->append();
        iter->set_value(activity_filter_cols.name, name);
        iter->set_value(activity_filter_cols.type, type.type);
        iter->set_value(activity_filter_cols.icon_name, Glib::ustring(type.icon_name != nullptr ? type.icon_name : ""));
    }

    return store;
}

void FilterBar::Impl::render_activity_pixbuf_func(
    Gtk::CellRendererPixbuf& cell_renderer,
    Gtk::TreeModel::const_iterator const& iter)
{
    auto const type = ActivityType{ iter->get_value(activity_filter_cols.type) };
    cell_renderer.property_width() = type == ActivityType::ALL ? 0 : 20;
    cell_renderer.property_ypad() = type == ActivityType::ALL ? 0 : 2;
}

void FilterBar::Impl::activity_combo_box_init(Gtk::ComboBox& combo)
{
    combo.set_model(activity_model_);
    combo.set_row_separator_func(sigc::hide<0>(&Impl::activity_is_it_a_separator));
    combo.set_active(0);

    {
        auto* r = Gtk::make_managed<Gtk::CellRendererPixbuf>();
        combo.pack_start(*r, false);
        combo.add_attribute(r->property_icon_name(), activity_filter_cols.icon_name);
        combo.set_cell_data_func(*r, [r](auto const& iter) { render_activity_pixbuf_func(*r, iter); });
    }

    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        combo.pack_start(*r, true);
        combo.add_attribute(r->property_text(), activity_filter_cols.name);
    }

    {
        auto* r = number_renderer_new();
        combo.pack_end(*r, true);
        combo.set_cell_data_func(*r, [r](auto const& iter) { render_number_func(*r, iter); });
    }
}

void FilterBar::Impl::update_filter_text()
{
    filter_->set_text(entry_->get_text());
}

void FilterBar::Impl::update_filter_activity()
{
    /* set active_activity_type_ from the activity combobox */
    if (auto const iter = activity_->get_active(); iter)
    {
        filter_->set_activity(ActivityType{ iter->get_value(activity_filter_cols.type) });
    }
    else
    {
        filter_->set_activity(ActivityType::ALL);
    }
}

void FilterBar::Impl::update_filter_tracker()
{
    /* set the active tracker type & host from the tracker combobox */
    if (auto const iter = tracker_->get_active(); iter)
    {
        filter_->set_tracker(
            static_cast<TrackerType>(iter->get_value(tracker_filter_cols.type)),
            iter->get_value(tracker_filter_cols.sitename));
    }
    else
    {
        filter_->set_tracker(TrackerType::ALL, {});
    }
}

bool FilterBar::Impl::update_count_label()
{
    /* get the visible count */
    auto const visibleCount = static_cast<int>(filter_model_->get_n_items());

    /* get the tracker count */
    int trackerCount = 0;
    if (auto const iter = tracker_->get_active(); iter)
    {
        trackerCount = iter->get_value(tracker_filter_cols.count);
    }

    /* get the activity count */
    int activityCount = 0;
    if (auto const iter = activity_->get_active(); iter)
    {
        activityCount = iter->get_value(activity_filter_cols.count);
    }

    /* set the text */
    if (auto const new_markup = visibleCount == std::min(activityCount, trackerCount) ?
            _("_Show:") :
            fmt::format(fmt::runtime(_("_Show {count:L} of:")), fmt::arg("count", visibleCount));
        new_markup != show_lb_->get_label().raw())
    {
        show_lb_->set_markup_with_mnemonic(new_markup);
    }

    return false;
}

void FilterBar::Impl::update_count_label_idle()
{
    if (!update_count_label_tag_.connected())
    {
        update_count_label_tag_ = Glib::signal_idle().connect(sigc::mem_fun(*this, &Impl::update_count_label));
    }
}

void FilterBar::Impl::update_filter_models(Torrent::ChangeFlags changes)
{
    static auto constexpr activity_flags = Torrent::ChangeFlag::ACTIVE_PEERS_DOWN | Torrent::ChangeFlag::ACTIVE_PEERS_UP |
        Torrent::ChangeFlag::ACTIVE | Torrent::ChangeFlag::ACTIVITY | Torrent::ChangeFlag::ERROR_CODE |
        Torrent::ChangeFlag::FINISHED;
    static auto constexpr tracker_flags = Torrent::ChangeFlag::TRACKERS;

    if (changes.test(activity_flags))
    {
        activity_filter_model_update();
    }

    if (changes.test(tracker_flags))
    {
        tracker_filter_model_update();
    }

    filter_->update(changes);

    if (changes.test(activity_flags | tracker_flags))
    {
        update_count_label_idle();
    }
}

void FilterBar::Impl::update_filter_models_idle(Torrent::ChangeFlags changes)
{
    if (!update_filter_models_tag_.connected())
    {
        update_filter_models_tag_ = Glib::signal_idle().connect(
            [this, changes]()
            {
                update_filter_models(changes);
                return false;
            });
    }
}

/***
****
***/

FilterBarExtraInit::FilterBarExtraInit()
    : ExtraClassInit(&FilterBarExtraInit::class_init, nullptr, &FilterBarExtraInit::instance_init)
{
}

void FilterBarExtraInit::class_init(void* klass, void* /*user_data*/)
{
    auto* const widget_klass = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(widget_klass, gtr_get_full_resource_path("FilterBar.ui").c_str());

    gtk_widget_class_bind_template_child_full(widget_klass, "activity_combo", FALSE, 0);
    gtk_widget_class_bind_template_child_full(widget_klass, "tracker_combo", FALSE, 0);
    gtk_widget_class_bind_template_child_full(widget_klass, "text_entry", FALSE, 0);
    gtk_widget_class_bind_template_child_full(widget_klass, "show_label", FALSE, 0);
}

void FilterBarExtraInit::instance_init(GTypeInstance* instance, void* /*klass*/)
{
    gtk_widget_init_template(GTK_WIDGET(instance));
}

/***
****
***/

FilterBar::FilterBar()
    : Glib::ObjectBase(typeid(FilterBar))
{
}

FilterBar::FilterBar(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& /*builder*/,
    Glib::RefPtr<Session> const& core)
    : Glib::ObjectBase(typeid(FilterBar))
    , Gtk::Box(cast_item)
    , impl_(std::make_unique<Impl>(*this, core))
{
}

FilterBar::~FilterBar() = default;

FilterBar::Impl::Impl(FilterBar& widget, Glib::RefPtr<Session> const& core)
    : widget_(widget)
    , core_(core)
    , activity_model_(activity_filter_model_new())
    , tracker_model_(tracker_filter_model_new())
    , activity_(get_template_child<Gtk::ComboBox>("activity_combo"))
    , tracker_(get_template_child<Gtk::ComboBox>("tracker_combo"))
    , entry_(get_template_child<Gtk::Entry>("text_entry"))
    , show_lb_(get_template_child<Gtk::Label>("show_label"))
{
    update_filter_models_on_add_remove_tag_ = core_->get_model()->signal_items_changed().connect(
        [this](guint /*position*/, guint /*removed*/, guint /*added*/) { update_filter_models_idle(~Torrent::ChangeFlags()); });
    update_filter_models_on_change_tag_ = core_->signal_torrents_changed().connect(
        sigc::hide<0>(sigc::mem_fun(*this, &Impl::update_filter_models_idle)));

    activity_filter_model_update();
    tracker_filter_model_update();

    activity_combo_box_init(*activity_);
    tracker_combo_box_init(*tracker_);

    filter_->signal_changed().connect([this](auto /*changes*/) { update_count_label_idle(); });

    filter_model_ = FilterListModel<Torrent>::create(core_->get_sorted_model(), filter_);

    tracker_->signal_changed().connect(sigc::mem_fun(*this, &Impl::update_filter_tracker));
    activity_->signal_changed().connect(sigc::mem_fun(*this, &Impl::update_filter_activity));

#if GTKMM_CHECK_VERSION(4, 0, 0)
    entry_->signal_icon_release().connect([this](auto /*icon_position*/) { entry_->set_text({}); });
#else
    entry_->signal_icon_release().connect([this](auto /*icon_position*/, auto const* /*event*/) { entry_->set_text({}); });
#endif
    entry_->signal_changed().connect(sigc::mem_fun(*this, &Impl::update_filter_text));
}

FilterBar::Impl::~Impl()
{
    update_filter_models_on_change_tag_.disconnect();
    update_filter_models_on_add_remove_tag_.disconnect();
    update_filter_models_tag_.disconnect();
    update_count_label_tag_.disconnect();
}

Glib::RefPtr<FilterBar::Model> FilterBar::get_filter_model() const
{
    return impl_->get_filter_model();
}

Glib::RefPtr<FilterBar::Impl::FilterModel> FilterBar::Impl::get_filter_model() const
{
    return filter_model_;
}

template<typename T>
T* FilterBar::Impl::get_template_child(char const* name) const
{
    auto full_type_name = std::string("gtkmm__CustomObject_");
    Glib::append_canonical_typename(full_type_name, typeid(FilterBar).name());

    return Glib::wrap(G_TYPE_CHECK_INSTANCE_CAST(
        gtk_widget_get_template_child(GTK_WIDGET(widget_.gobj()), g_type_from_name(full_type_name.c_str()), name),
        T::get_base_type(),
        typename T::BaseObjectType));
}
