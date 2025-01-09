// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "StatsDialog.h"

#include "GtkCompat.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/ustring.h>
#include <gtkmm/label.h>
#include <gtkmm/messagedialog.h>

#include <fmt/core.h>

#include <memory>

static auto constexpr TR_RESPONSE_RESET = 1;

class StatsDialog::Impl
{
public:
    Impl(StatsDialog& dialog, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    Impl(Impl&&) = delete;
    Impl(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    ~Impl();

private:
    bool updateStats();
    void dialogResponse(int response);

private:
    StatsDialog& dialog_;
    Glib::RefPtr<Session> const core_;

    Gtk::Label* one_up_lb_;
    Gtk::Label* one_down_lb_;
    Gtk::Label* one_ratio_lb_;
    Gtk::Label* one_time_lb_;

    Gtk::Label* all_up_lb_;
    Gtk::Label* all_down_lb_;
    Gtk::Label* all_ratio_lb_;
    Gtk::Label* all_time_lb_;

    Gtk::Label* all_sessions_lb_;

    sigc::connection update_stats_tag_;
};

namespace
{

void setLabel(Gtk::Label* l, Glib::ustring const& str)
{
    gtr_label_set_text(*l, str);
}

void setLabelFromRatio(Gtk::Label* l, double d)
{
    setLabel(l, tr_strlratio(d));
}

auto startedTimesText(uint64_t n)
{
    return fmt::format(fmt::runtime(ngettext("Started {count:L} time", "Started {count:L} times", n)), fmt::arg("count", n));
}

} // namespace

bool StatsDialog::Impl::updateStats()
{
    auto stats = tr_sessionGetStats(core_->get_session());
    setLabel(one_up_lb_, tr_strlsize(stats.uploadedBytes));
    setLabel(one_down_lb_, tr_strlsize(stats.downloadedBytes));
    setLabel(one_time_lb_, tr_format_time(stats.secondsActive));
    setLabelFromRatio(one_ratio_lb_, stats.ratio);

    stats = tr_sessionGetCumulativeStats(core_->get_session());
    setLabel(all_sessions_lb_, startedTimesText(stats.sessionCount));
    setLabel(all_up_lb_, tr_strlsize(stats.uploadedBytes));
    setLabel(all_down_lb_, tr_strlsize(stats.downloadedBytes));
    setLabel(all_time_lb_, tr_format_time(stats.secondsActive));
    setLabelFromRatio(all_ratio_lb_, stats.ratio);

    return true;
}

StatsDialog::Impl::~Impl()
{
    update_stats_tag_.disconnect();
}

void StatsDialog::Impl::dialogResponse(int response)
{
    if (response == TR_RESPONSE_RESET)
    {
        auto w = std::make_shared<Gtk::MessageDialog>(
            dialog_,
            _("Reset your statistics?"),
            false,
            TR_GTK_MESSAGE_TYPE(QUESTION),
            TR_GTK_BUTTONS_TYPE(NONE),
            true);
        w->add_button(_("_Cancel"), TR_GTK_RESPONSE_TYPE(CANCEL));
        w->add_button(_("_Reset"), TR_RESPONSE_RESET);
        w->set_secondary_text(
            _("These statistics are for your information only. "
              "Resetting them doesn't affect the statistics logged by your BitTorrent trackers."));
        w->signal_response().connect(
            [this, w](int inner_response) mutable
            {
                if (inner_response == TR_RESPONSE_RESET)
                {
                    tr_sessionClearStats(core_->get_session());
                    updateStats();
                }
                w.reset();
            });
        w->show();
    }

    if (response == TR_GTK_RESPONSE_TYPE(CLOSE))
    {
        dialog_.close();
    }
}

StatsDialog::StatsDialog(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core)
    : Gtk::Dialog(cast_item)
    , impl_(std::make_unique<Impl>(*this, builder, core))
{
    set_transient_for(parent);
}

StatsDialog::~StatsDialog() = default;

std::unique_ptr<StatsDialog> StatsDialog::create(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("StatsDialog.ui"));
    return std::unique_ptr<StatsDialog>(gtr_get_widget_derived<StatsDialog>(builder, "StatsDialog", parent, core));
}

StatsDialog::Impl::Impl(StatsDialog& dialog, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core)
    : dialog_(dialog)
    , core_(core)
    , one_up_lb_(gtr_get_widget<Gtk::Label>(builder, "current_uploaded_value_label"))
    , one_down_lb_(gtr_get_widget<Gtk::Label>(builder, "current_downloaded_value_label"))
    , one_ratio_lb_(gtr_get_widget<Gtk::Label>(builder, "current_ratio_value_label"))
    , one_time_lb_(gtr_get_widget<Gtk::Label>(builder, "current_duration_value_label"))
    , all_up_lb_(gtr_get_widget<Gtk::Label>(builder, "total_uploaded_value_label"))
    , all_down_lb_(gtr_get_widget<Gtk::Label>(builder, "total_downloaded_value_label"))
    , all_ratio_lb_(gtr_get_widget<Gtk::Label>(builder, "total_ratio_value_label"))
    , all_time_lb_(gtr_get_widget<Gtk::Label>(builder, "total_duration_value_label"))
    , all_sessions_lb_(gtr_get_widget<Gtk::Label>(builder, "start_count_label"))
{
    dialog_.set_default_response(TR_GTK_RESPONSE_TYPE(CLOSE));
    dialog_.signal_response().connect(sigc::mem_fun(*this, &Impl::dialogResponse));

    updateStats();
    update_stats_tag_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(*this, &Impl::updateStats),
        SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS);
}
