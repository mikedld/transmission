// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include "GtkCompat.h"

#include <libtransmission/tr-macros.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gtkmm/sorter.h>
#else
#include <glibmm/object.h>
#endif

template<typename T>
class SorterBase : public IF_GTKMM4(Gtk::Sorter, Glib::Object)
{
#if !GTKMM_CHECK_VERSION(4, 0, 0)
public:
    // clang-format off
    enum class Change : uint8_t
    {
        DIFFERENT,
        INVERTED,
        LESS_STRICT,
        MORE_STRICT,
    };
    // clang-format on
#endif

public:
    SorterBase() = default;
    ~SorterBase() override = default;

    TR_DISABLE_COPY_MOVE(SorterBase)

    virtual int compare(T const& lhs, T const& rhs) const = 0;

#if !GTKMM_CHECK_VERSION(4, 0, 0)
    sigc::signal<void(Change)>& signal_changed();
#endif

protected:
#if GTKMM_CHECK_VERSION(4, 0, 0)
    // Gtk::Sorter
    Gtk::Ordering compare_vfunc(gpointer lhs, gpointer rhs) override;
    Order get_order_vfunc() override;
#else
    void changed(Change change);
#endif

private:
#if !GTKMM_CHECK_VERSION(4, 0, 0)
    sigc::signal<void(Change)> signal_changed_;
#endif
};
