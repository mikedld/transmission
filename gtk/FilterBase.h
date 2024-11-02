// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include "GtkCompat.h"

#include <libtransmission/tr-macros.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gtkmm/filter.h>
#else
#include <glibmm/object.h>
#endif

#include <cstdint>

template<typename T>
class FilterBase : public IF_GTKMM4(Gtk::Filter, Glib::Object)
{
#if !GTKMM_CHECK_VERSION(4, 0, 0)
public:
    // clang-format off
    enum class Change : uint8_t
    {
        DIFFERENT,
        LESS_STRICT,
        MORE_STRICT,
    };
    // clang-format on
#endif

public:
    FilterBase() = default;
    ~FilterBase() override = default;

    TR_DISABLE_COPY_MOVE(FilterBase)

    virtual bool match(T const& item) const = 0;

    virtual bool matches_all() const;
    virtual bool matches_none() const;

#if !GTKMM_CHECK_VERSION(4, 0, 0)
    sigc::signal<void(Change)>& signal_changed();
#endif

protected:
#if GTKMM_CHECK_VERSION(4, 0, 0)
    // Gtk::Filter
    bool match_vfunc(Glib::RefPtr<Glib::ObjectBase> const& object) override;
    Match get_strictness_vfunc() override;
#else
    void changed(Change change);
#endif

private:
#if !GTKMM_CHECK_VERSION(4, 0, 0)
    sigc::signal<void(Change)> signal_changed_;
#endif
};
