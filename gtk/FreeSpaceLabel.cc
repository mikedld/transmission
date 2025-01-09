// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "FreeSpaceLabel.h"

#include "Session.h"
#include "Utils.h"

#include <libtransmission/file.h>

#include <glibmm/i18n.h>
#include <glibmm/main.h>

#include <fmt/core.h>

#include <memory>
#include <string>
#include <string_view>

class FreeSpaceLabel::Impl
{
public:
    Impl(FreeSpaceLabel& label, Glib::RefPtr<Session> const& core, std::string_view dir);
    Impl(Impl&&) = delete;
    Impl(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    ~Impl();

    void set_dir(std::string_view dir);

private:
    bool on_freespace_timer();

    FreeSpaceLabel& label_;
    Glib::RefPtr<Session> const core_;
    std::string dir_;
    sigc::connection timer_id_;
};

FreeSpaceLabel::Impl::~Impl()
{
    timer_id_.disconnect();
}

bool FreeSpaceLabel::Impl::on_freespace_timer()
{
    if (core_->get_session() == nullptr)
    {
        return false;
    }

    auto const capacity = tr_sys_path_get_capacity(dir_);
    auto const text = capacity ?
        fmt::format(fmt::runtime(_("{disk_space} free")), fmt::arg("disk_space", tr_strlsize(capacity->free))) :
        _("Error");
    label_.set_markup(fmt::format("<i>{:s}</i>", text));

    return true;
}

FreeSpaceLabel::FreeSpaceLabel(Glib::RefPtr<Session> const& core, std::string_view dir)
    : impl_(std::make_unique<Impl>(*this, core, dir))
{
}

FreeSpaceLabel::FreeSpaceLabel(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& /*builder*/,
    Glib::RefPtr<Session> const& core,
    std::string_view dir)
    : Gtk::Label(cast_item)
    , impl_(std::make_unique<Impl>(*this, core, dir))
{
}

FreeSpaceLabel::~FreeSpaceLabel() = default;

FreeSpaceLabel::Impl::Impl(FreeSpaceLabel& label, Glib::RefPtr<Session> const& core, std::string_view dir)
    : label_(label)
    , core_(core)
    , dir_(dir)
{
    timer_id_ = Glib::signal_timeout().connect_seconds(sigc::mem_fun(*this, &Impl::on_freespace_timer), 3);
    on_freespace_timer();
}

void FreeSpaceLabel::set_dir(std::string_view dir)
{
    impl_->set_dir(dir);
}

void FreeSpaceLabel::Impl::set_dir(std::string_view dir)
{
    dir_ = dir;
    on_freespace_timer();
}
