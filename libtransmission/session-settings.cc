// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "libtransmission/session-settings.h"
#include "libtransmission/variant.h"

void tr_session_settings::load(tr_variant const& src)
{
    auto const* map = src.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return;
    }

#define V(key, field, type, default_value, comment) \
    if (auto const iter = map->find(key); iter != std::end(*map)) \
    { \
        if (auto val = libtransmission::VariantConverter::load<decltype(field)>(iter->second); val) \
        { \
            this->field = *val; \
        } \
    }
    SESSION_SETTINGS_FIELDS(V)
#undef V
}

tr_variant tr_session_settings::settings() const
{
    auto map = tr_variant::Map{ 60U };
#define V(key, field, type, default_value, comment) \
    map.try_emplace(key, libtransmission::VariantConverter::save<decltype(field)>(field));
    SESSION_SETTINGS_FIELDS(V)
#undef V
    return tr_variant{ std::move(map) };
}

tr_variant tr_session_settings::default_settings()
{
    return tr_session_settings{}.settings();
}
