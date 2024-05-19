// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "MessageLogWindow.h"

#include "Actions.h"
#include "GtkCompat.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

#include <libtransmission/log.h>

#include <giomm/simpleaction.h>
#include <glibmm/convert.h>
#include <glibmm/datetime.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/ustring.h>
#include <glibmm/variant.h>
#include <gtkmm/cellrenderertext.h>
#include <gtkmm/combobox.h>
#include <gtkmm/filechoosernative.h>
#include <gtkmm/liststore.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/treemodel.h>
#include <gtkmm/treemodelcolumn.h>
#include <gtkmm/treemodelfilter.h>
#include <gtkmm/treemodelsort.h>
#include <gtkmm/treeview.h>

#include <fmt/core.h>
#include <fmt/ostream.h>

#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <utility>

class MessageLogColumnsModel : public Gtk::TreeModelColumnRecord
{
public:
    MessageLogColumnsModel() noexcept
    {
        add(sequence);
        add(name);
        add(message);
        add(tr_msg);
    }

    Gtk::TreeModelColumn<unsigned int> sequence;
    Gtk::TreeModelColumn<Glib::ustring> name;
    Gtk::TreeModelColumn<Glib::ustring> message;
    Gtk::TreeModelColumn<tr_log_message const*> tr_msg;
};

MessageLogColumnsModel const message_log_cols;

class MessageLogWindow::Impl
{
public:
    Impl(MessageLogWindow& window, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    Impl(Impl&&) = delete;
    Impl(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    ~Impl();

private:
    bool onRefresh();

    void onSaveRequest();
    void onSaveDialogResponse(Glib::RefPtr<Gtk::FileChooserNative>& d, int response);
    void doSave(std::string const& filename);

    void onClearRequest();
    void onPauseToggled(Gio::SimpleAction& action);

    void scroll_to_bottom();
    void level_combo_changed_cb(Gtk::ComboBox* combo_box);
    static void level_combo_init(Gtk::ComboBox* level_combo);

    [[nodiscard]] bool is_pinned_to_new() const;
    [[nodiscard]] bool isRowVisible(Gtk::TreeModel::const_iterator const& iter) const;

private:
    MessageLogWindow& window_;
    Glib::RefPtr<Session> const core_;

    Gtk::TreeView* view_ = nullptr;
    Glib::RefPtr<Gtk::ListStore> store_;
    Glib::RefPtr<Gtk::TreeModelFilter> filter_;
    Glib::RefPtr<Gtk::TreeModelSort> sort_;
    tr_log_level maxLevel_ = TR_LOG_INFO;
    bool isPaused_ = false;
    sigc::connection refresh_tag_;

    static auto inline const level_names_ = std::array<std::pair<tr_log_level, char const*>, 5U>{ {
        { TR_LOG_CRITICAL, C_("Logging level", "Critical") },
        { TR_LOG_ERROR, C_("Logging level", "Error") },
        { TR_LOG_WARN, C_("Logging level", "Warning") },
        { TR_LOG_INFO, C_("Logging level", "Information") },
        { TR_LOG_DEBUG, C_("Logging level", "Debug") },
    } };
};

namespace
{

tr_log_message* myTail = nullptr;
tr_log_message* myHead = nullptr;

} // namespace

/****
*****
****/

/* is the user looking at the latest messages? */
bool MessageLogWindow::Impl::is_pinned_to_new() const
{
    bool pinned_to_new = false;

    if (view_ == nullptr)
    {
        pinned_to_new = true;
    }
    else
    {
        Gtk::TreeModel::Path first_visible;
        Gtk::TreeModel::Path last_visible;

        if (view_->get_visible_range(first_visible, last_visible))
        {
            auto const row_count = sort_->children().size();

            if (auto const iter = sort_->children()[row_count - 1]; iter)
            {
                pinned_to_new = last_visible == sort_->get_path(TR_GTK_TREE_MODEL_CHILD_ITER(iter));
            }
        }
    }

    return pinned_to_new;
}

void MessageLogWindow::Impl::scroll_to_bottom()
{
    auto const row_count = sort_->children().size();
    if (row_count == 0)
    {
        return;
    }

    if (auto const iter = sort_->children()[row_count - 1]; iter)
    {
        view_->scroll_to_row(sort_->get_path(TR_GTK_TREE_MODEL_CHILD_ITER(iter)), 1);
    }
}

/****
*****
****/

// static
void MessageLogWindow::Impl::level_combo_init(Gtk::ComboBox* level_combo)
{
    auto const pref_level = static_cast<tr_log_level>(gtr_pref_int_get(TR_KEY_message_level));
    auto const default_level = TR_LOG_INFO;

    auto has_pref_level = false;
    auto items = std::vector<std::pair<Glib::ustring, int>>{};
    items.reserve(std::size(level_names_));
    for (auto const& [level, name] : level_names_)
    {
        items.emplace_back(name, level);
        has_pref_level |= level == pref_level;
    }

    gtr_combo_box_set_enum(*level_combo, items);
    gtr_combo_box_set_active_enum(*level_combo, has_pref_level ? pref_level : default_level);
}

void MessageLogWindow::Impl::level_combo_changed_cb(Gtk::ComboBox* combo_box)
{
    auto const level = static_cast<tr_log_level>(gtr_combo_box_get_active_enum(*combo_box));
    bool const pinned_to_new = is_pinned_to_new();

    tr_logSetLevel(level);
    core_->set_pref(TR_KEY_message_level, level);
    maxLevel_ = level;
    filter_->refilter();

    if (pinned_to_new)
    {
        scroll_to_bottom();
    }
}

namespace
{
using std::chrono::system_clock;

/* similar to asctime, but is utf8-clean */
Glib::ustring gtr_asctime(system_clock::time_point t)
{
    return Glib::DateTime::create_now_local(system_clock::to_time_t(t)).format("%a %b %e %T %Y"); /* ctime equiv */
}

} // namespace

void MessageLogWindow::Impl::doSave(std::string const& filename)
{
    try
    {
        auto stream = std::ofstream();
        stream.exceptions(std::ios_base::failbit | std::ios_base::badbit);
        stream.open(filename, std::ios_base::trunc);

        for (auto const& row : store_->children())
        {
            auto const* const node = row.get_value(message_log_cols.tr_msg);
            auto const date = gtr_asctime(node->when);

            auto const iter = std::find_if(
                std::begin(level_names_),
                std::end(level_names_),
                [key = node->level](auto const& item) { return item.first == key; });
            auto const* const level_str = iter != std::end(level_names_) ? iter->second : "???";

            fmt::print(stream, "{}\t{}\t{}\t{}\n", date, level_str, node->name, node->message);
        }
    }
    catch (std::ios_base::failure const& e)
    {
        auto w = std::make_shared<Gtk::MessageDialog>(
            window_,
            fmt::format(
                fmt::runtime(_("Couldn't save '{path}': {error} ({error_code})")),
                fmt::arg("path", Glib::filename_to_utf8(filename)),
                fmt::arg("error", e.code().message()),
                fmt::arg("error_code", e.code().value())),
            false,
            TR_GTK_MESSAGE_TYPE(ERROR),
            TR_GTK_BUTTONS_TYPE(CLOSE));
        w->set_secondary_text(e.code().message());
        w->signal_response().connect([w](int /*response*/) mutable { w.reset(); });
        w->show();
    }
}

void MessageLogWindow::Impl::onSaveDialogResponse(Glib::RefPtr<Gtk::FileChooserNative>& d, int response)
{
    auto const filename = response == TR_GTK_RESPONSE_TYPE(ACCEPT) ? d->get_file()->get_path() : std::string();

    d.reset();

    if (!filename.empty())
    {
        doSave(filename);
    }
}

void MessageLogWindow::Impl::onSaveRequest()
{
    auto d = Gtk::FileChooserNative::create(_("Save Log"), window_, TR_GTK_FILE_CHOOSER_ACTION(SAVE), _("_Save"), _("_Cancel"));
    d->signal_response().connect([this, d](int response) mutable { onSaveDialogResponse(d, response); });
    d->show();
}

void MessageLogWindow::Impl::onClearRequest()
{
    store_->clear();
    tr_logFreeQueue(myHead);
    myHead = myTail = nullptr;
}

void MessageLogWindow::Impl::onPauseToggled(Gio::SimpleAction& action)
{
    bool value = false;
    action.get_state(value);

    action.set_state(Glib::Variant<bool>::create(!value));

    isPaused_ = !value;
}

namespace
{

void setForegroundColor(Gtk::CellRendererText* renderer, tr_log_level level)
{
    switch (level)
    {
    case TR_LOG_CRITICAL:
    case TR_LOG_ERROR:
    case TR_LOG_WARN:
        renderer->property_foreground() = "red";
        break;

    case TR_LOG_DEBUG:
    case TR_LOG_TRACE:
        renderer->property_foreground() = "forestgreen";
        break;

    default:
        renderer->property_foreground_set() = false;
        break;
    }
}

void renderText(
    Gtk::CellRendererText* renderer,
    Gtk::TreeModel::const_iterator const& iter,
    Gtk::TreeModelColumn<Glib::ustring> const& col)
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    renderer->property_text() = iter->get_value(col);
    renderer->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(END);
    setForegroundColor(renderer, node->level);
}

void renderTime(Gtk::CellRendererText* renderer, Gtk::TreeModel::const_iterator const& iter)
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    renderer->property_text() = Glib::DateTime::create_now_local(std::chrono::system_clock::to_time_t(node->when)).format("%T");
    setForegroundColor(renderer, node->level);
}

void appendColumn(Gtk::TreeView* view, Gtk::TreeModelColumnBase const& col)
{
    Gtk::TreeViewColumn* c = nullptr;

    if (col == message_log_cols.name)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Name"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderText(r, iter, message_log_cols.name); });
        c->set_sizing(TR_GTK_TREE_VIEW_COLUMN_SIZING(FIXED));
        c->set_fixed_width(200);
        c->set_resizable(true);
    }
    else if (col == message_log_cols.message)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Message"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderText(r, iter, message_log_cols.message); });
        c->set_sizing(TR_GTK_TREE_VIEW_COLUMN_SIZING(FIXED));
        c->set_fixed_width(500);
        c->set_resizable(true);
    }
    else if (col == message_log_cols.sequence)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Time"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderTime(r, iter); });
        c->set_resizable(true);
    }
    else
    {
        g_assert_not_reached();
    }

    view->append_column(*c);
}

} // namespace

bool MessageLogWindow::Impl::isRowVisible(Gtk::TreeModel::const_iterator const& iter) const
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    return node != nullptr && node->level <= maxLevel_;
}

MessageLogWindow::Impl::~Impl()
{
    refresh_tag_.disconnect();
}

namespace
{

tr_log_message* addMessages(Glib::RefPtr<Gtk::ListStore> const& store, tr_log_message* head)
{
    static unsigned int sequence = 0;
    auto const default_name = Glib::get_application_name();

    while (head != nullptr && head->next != nullptr)
    {
        auto const& message = *head;
        head = head->next;

        char const* name = !std::empty(message.name) ? message.name.c_str() : default_name.c_str();

        auto row_it = store->prepend();
        auto& row = *row_it;
        row[message_log_cols.tr_msg] = &message;
        row[message_log_cols.name] = name;
        row[message_log_cols.message] = message.message;
        row[message_log_cols.sequence] = ++sequence;

        /* if it's an error message, dump it to the terminal too */
        if (message.level == TR_LOG_ERROR)
        {
            auto gstr = fmt::format("{}:{} {}", message.file, message.line, message.message);

            if (!std::empty(message.name))
            {
                gstr += fmt::format(" ({})", message.name.c_str());
            }

            gtr_warning(gstr);
        }
    }

    return head; /* tail */
}

} // namespace

bool MessageLogWindow::Impl::onRefresh()
{
    bool const pinned_to_new = is_pinned_to_new();

    if (!isPaused_)
    {
        if (auto* msgs = tr_logGetQueue(); msgs != nullptr)
        {
            /* add the new messages and append them to the end of
             * our persistent list */
            tr_log_message* tail = addMessages(store_, msgs);

            if (myTail != nullptr)
            {
                myTail->next = msgs;
            }
            else
            {
                myHead = msgs;
            }

            myTail = tail;
        }

        if (pinned_to_new)
        {
            scroll_to_bottom();
        }
    }

    return true;
}

/**
***  Public Functions
**/

std::unique_ptr<MessageLogWindow> MessageLogWindow::create(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("MessageLogWindow.ui"));
    return std::unique_ptr<MessageLogWindow>(
        gtr_get_widget_derived<MessageLogWindow>(builder, "MessageLogWindow", parent, core));
}

MessageLogWindow::MessageLogWindow(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core)
    : Gtk::Window(cast_item)
    , impl_(std::make_unique<Impl>(*this, builder, core))
{
    set_transient_for(parent);
}

MessageLogWindow::~MessageLogWindow() = default;

MessageLogWindow::Impl::Impl(
    MessageLogWindow& window,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Session> const& core)
    : window_(window)
    , core_(core)
    , view_(gtr_get_widget<Gtk::TreeView>(builder, "messages_view"))
    , store_(Gtk::ListStore::create(message_log_cols))
    , filter_(Gtk::TreeModelFilter::create(store_))
    , sort_(Gtk::TreeModelSort::create(filter_))
    , maxLevel_(static_cast<tr_log_level>(gtr_pref_int_get(TR_KEY_message_level)))
    , refresh_tag_(Glib::signal_timeout().connect_seconds(
          sigc::mem_fun(*this, &Impl::onRefresh),
          SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS))
{
    /**
    ***  toolbar
    **/

    auto const action_group = Gio::SimpleActionGroup::create();

    auto const save_action = Gio::SimpleAction::create("save-message-log");
    save_action->signal_activate().connect([this](auto const& /*value*/) { onSaveRequest(); });
    action_group->add_action(save_action);

    auto const clear_action = Gio::SimpleAction::create("clear-message-log");
    clear_action->signal_activate().connect([this](auto const& /*value*/) { onClearRequest(); });
    action_group->add_action(clear_action);

    auto const pause_action = Gio::SimpleAction::create_bool("pause-message-log");
    pause_action->signal_activate().connect([this, &action = *pause_action](auto const& /*value*/) { onPauseToggled(action); });
    action_group->add_action(pause_action);

    auto* const level_combo = gtr_get_widget<Gtk::ComboBox>(builder, "level_combo");
    level_combo_init(level_combo);
    level_combo->signal_changed().connect([this, level_combo]() { level_combo_changed_cb(level_combo); });

    window_.insert_action_group("win", action_group);

    /**
    ***  messages
    **/

    addMessages(store_, myHead);
    onRefresh(); /* much faster to populate *before* it has listeners */

    sort_->set_sort_column(message_log_cols.sequence, TR_GTK_SORT_TYPE(ASCENDING));
    filter_->set_visible_func(sigc::mem_fun(*this, &Impl::isRowVisible));

    view_->set_model(sort_);
    setup_item_view_button_event_handling(
        *view_,
        {},
        [this](double view_x, double view_y) { return on_item_view_button_released(*view_, view_x, view_y); });
    appendColumn(view_, message_log_cols.sequence);
    appendColumn(view_, message_log_cols.name);
    appendColumn(view_, message_log_cols.message);

    level_combo_changed_cb(level_combo);

    scroll_to_bottom();
}
