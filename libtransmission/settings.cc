// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <chrono>
#include <cstddef> // size_t
#include <cstdint> // int64_t, uint32_t
#include <string>
#include <string_view>
#include <utility>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/log.h" // for tr_log_level
#include "libtransmission/net.h" // for tr_port
#include "libtransmission/open-files.h" // for tr_open_files::Preallocation
#include "libtransmission/peer-io.h" // tr_preferred_transport
#include "libtransmission/settings.h"
#include "libtransmission/utils.h" // for tr_strv_strip(), tr_strlower()
#include "libtransmission/variant.h"
#include "libtransmission/tr-assert.h"

using namespace std::literals;

namespace libtransmission
{
namespace
{
template<typename T, size_t N>
using Lookup = std::array<std::pair<std::string_view, T>, N>;

// ---

bool load_bool(tr_variant const& src, bool* tgt)
{
    if (auto val = src.value_if<bool>())
    {
        *tgt = *val;
        return true;
    }

    return false;
}

tr_variant save_bool(bool const& val)
{
    return val;
}

// ---

bool load_double(tr_variant const& src, double* tgt)
{
    if (auto val = src.value_if<double>())
    {
        *tgt = *val;
        return true;
    }

    return false;
}

tr_variant save_double(double const& val)
{
    return val;
}

// ---

auto constexpr EncryptionKeys = Lookup<tr_encryption_mode, 3U>{ {
    { "required", TR_ENCRYPTION_REQUIRED },
    { "preferred", TR_ENCRYPTION_PREFERRED },
    { "allowed", TR_CLEAR_PREFERRED },
} };

bool load_encryption_mode(tr_variant const& src, tr_encryption_mode* tgt)
{
    static constexpr auto& Keys = EncryptionKeys;

    if (auto const val = src.value_if<std::string_view>())
    {
        auto const needle = tr_strlower(tr_strv_strip(*val));

        for (auto const& [key, encryption] : Keys)
        {
            if (key == needle)
            {
                *tgt = encryption;
                return true;
            }
        }
    }

    if (auto const val = src.value_if<int64_t>())
    {
        for (auto const& [key, encryption] : Keys)
        {
            if (encryption == *val)
            {
                *tgt = encryption;
                return true;
            }
        }
    }

    return false;
}

tr_variant save_encryption_mode(tr_encryption_mode const& val)
{
    return static_cast<int64_t>(val);
}

// ---

auto constexpr LogKeys = Lookup<tr_log_level, 7U>{ {
    { "critical", TR_LOG_CRITICAL },
    { "debug", TR_LOG_DEBUG },
    { "error", TR_LOG_ERROR },
    { "info", TR_LOG_INFO },
    { "off", TR_LOG_OFF },
    { "trace", TR_LOG_TRACE },
    { "warn", TR_LOG_WARN },
} };

bool load_log_level(tr_variant const& src, tr_log_level* tgt)
{
    static constexpr auto& Keys = LogKeys;

    if (auto const val = src.value_if<std::string_view>())
    {
        auto const needle = tr_strlower(tr_strv_strip(*val));

        for (auto const& [name, log_level] : Keys)
        {
            if (needle == name)
            {
                *tgt = log_level;
                return true;
            }
        }
    }

    if (auto const val = src.value_if<int64_t>())
    {
        for (auto const& [name, log_level] : Keys)
        {
            if (log_level == *val)
            {
                *tgt = log_level;
                return true;
            }
        }
    }

    return false;
}

tr_variant save_log_level(tr_log_level const& val)
{
    return static_cast<int64_t>(val);
}

// ---

bool load_mode_t(tr_variant const& src, tr_mode_t* tgt)
{
    if (auto const val = src.value_if<std::string_view>())
    {
        if (auto const mode = tr_num_parse<uint32_t>(*val, nullptr, 8); mode)
        {
            *tgt = static_cast<tr_mode_t>(*mode);
            return true;
        }
    }

    if (auto const val = src.value_if<int64_t>())
    {
        *tgt = static_cast<tr_mode_t>(*val);
        return true;
    }

    return false;
}

tr_variant save_mode_t(tr_mode_t const& val)
{
    return fmt::format("{:#03o}", val);
}

// ---

bool load_msec(tr_variant const& src, std::chrono::milliseconds* tgt)
{
    if (auto val = src.value_if<int64_t>())
    {
        *tgt = std::chrono::milliseconds(*val);
        return true;
    }

    return false;
}

tr_variant save_msec(std::chrono::milliseconds const& src)
{
    return src.count();
}

// ---

bool load_port(tr_variant const& src, tr_port* tgt)
{
    if (auto const val = src.value_if<int64_t>())
    {
        *tgt = tr_port::from_host(*val);
        return true;
    }

    return false;
}

tr_variant save_port(tr_port const& val)
{
    return int64_t{ val.host() };
}

// ---

auto constexpr PreallocationKeys = Lookup<tr_open_files::Preallocation, 5U>{ {
    { "off", tr_open_files::Preallocation::None },
    { "none", tr_open_files::Preallocation::None },
    { "fast", tr_open_files::Preallocation::Sparse },
    { "sparse", tr_open_files::Preallocation::Sparse },
    { "full", tr_open_files::Preallocation::Full },
} };

bool load_preallocation_mode(tr_variant const& src, tr_open_files::Preallocation* tgt)
{
    static constexpr auto& Keys = PreallocationKeys;

    if (auto const val = src.value_if<std::string_view>())
    {
        auto const needle = tr_strlower(tr_strv_strip(*val));

        for (auto const& [name, value] : Keys)
        {
            if (name == needle)
            {
                *tgt = value;
                return true;
            }
        }
    }

    if (auto const val = src.value_if<int64_t>())
    {
        for (auto const& [name, value] : Keys)
        {
            if (value == static_cast<tr_open_files::Preallocation>(*val))
            {
                *tgt = value;
                return true;
            }
        }
    }

    return false;
}

tr_variant save_preallocation_mode(tr_open_files::Preallocation const& val)
{
    return static_cast<int64_t>(val);
}

// ---

auto constexpr PreferredTransportKeys = Lookup<tr_preferred_transport, TR_NUM_PREFERRED_TRANSPORT>{ {
    { "utp", TR_PREFER_UTP },
    { "tcp", TR_PREFER_TCP },
} };

bool load_preferred_transport(tr_variant const& src, std::array<tr_preferred_transport, TR_NUM_PREFERRED_TRANSPORT>* tgt)
{
    static constexpr auto& Keys = PreferredTransportKeys;

    auto preferred = TR_NUM_PREFERRED_TRANSPORT;
    if (auto const val = src.value_if<std::string_view>())
    {
        auto const needle = tr_strlower(tr_strv_strip(*val));

        for (auto const& [name, value] : Keys)
        {
            if (name == needle)
            {
                preferred = value;
                break;
            }
        }
    }

    if (auto const val = src.value_if<int64_t>())
    {
        for (auto const& [name, value] : Keys)
        {
            if (value == *val)
            {
                preferred = value;
                break;
            }
        }
    }

    if (preferred >= TR_NUM_PREFERRED_TRANSPORT)
    {
        return false;
    }

    tgt->front() = preferred;
    for (size_t i = 0U; i < TR_NUM_PREFERRED_TRANSPORT; ++i)
    {
        if (i != preferred)
        {
            (*tgt)[i + (i < preferred ? 1U : 0U)] = static_cast<tr_preferred_transport>(i);
        }
    }

    return true;
}

tr_variant save_preferred_transport(std::array<tr_preferred_transport, TR_NUM_PREFERRED_TRANSPORT> const& val)
{
    auto const& preferred = val.front();
    for (auto const& [key, value] : PreferredTransportKeys)
    {
        if (value == preferred)
        {
            return key;
        }
    }

    return static_cast<int64_t>(preferred);
}

// ---

bool load_size_t(tr_variant const& src, size_t* tgt)
{
    if (auto const val = src.value_if<int64_t>())
    {
        *tgt = static_cast<size_t>(*val);
        return true;
    }

    return false;
}

tr_variant save_size_t(size_t const& val)
{
    return uint64_t{ val };
}

// ---

bool load_string(tr_variant const& src, std::string* tgt)
{
    if (auto const val = src.value_if<std::string_view>())
    {
        *tgt = std::string{ *val };
        return true;
    }

    return false;
}

tr_variant save_string(std::string const& val)
{
    return val;
}

// ---

bool load_tos_t(tr_variant const& src, tr_tos_t* tgt)
{
    if (auto const val = src.value_if<std::string_view>())
    {
        if (auto const tos = tr_tos_t::from_string(*val); tos)
        {
            *tgt = *tos;
            return true;
        }

        return false;
    }

    if (auto const val = src.value_if<int64_t>())
    {
        *tgt = tr_tos_t{ static_cast<int>(*val) };
        return true;
    }

    return false;
}

tr_variant save_tos_t(tr_tos_t const& val)
{
    return val.toString();
}

// ---

auto constexpr VerifyModeKeys = Lookup<tr_verify_added_mode, 2U>{ {
    { "fast", TR_VERIFY_ADDED_FAST },
    { "full", TR_VERIFY_ADDED_FULL },
} };

bool load_verify_added_mode(tr_variant const& src, tr_verify_added_mode* tgt)
{
    static constexpr auto& Keys = VerifyModeKeys;

    if (auto const val = src.value_if<std::string_view>())
    {
        auto const needle = tr_strlower(tr_strv_strip(*val));

        for (auto const& [name, value] : Keys)
        {
            if (name == needle)
            {
                *tgt = value;
                return true;
            }
        }
    }

    if (auto const val = src.value_if<int64_t>())
    {
        for (auto const& [name, value] : Keys)
        {
            if (value == *val)
            {
                *tgt = value;
                return true;
            }
        }
    }

    return false;
}

tr_variant save_verify_added_mode(tr_verify_added_mode const& val)
{
    for (auto const& [key, value] : VerifyModeKeys)
    {
        if (value == val)
        {
            return key;
        }
    }

    return static_cast<int64_t>(val);
}
} // unnamed namespace

Settings::Settings()
{
    add_type_handler(load_bool, save_bool);
    add_type_handler(load_double, save_double);
    add_type_handler(load_encryption_mode, save_encryption_mode);
    add_type_handler(load_log_level, save_log_level);
    add_type_handler(load_mode_t, save_mode_t);
    add_type_handler(load_msec, save_msec);
    add_type_handler(load_port, save_port);
    add_type_handler(load_preallocation_mode, save_preallocation_mode);
    add_type_handler(load_preferred_transport, save_preferred_transport);
    add_type_handler(load_size_t, save_size_t);
    add_type_handler(load_string, save_string);
    add_type_handler(load_tos_t, save_tos_t);
    add_type_handler(load_verify_added_mode, save_verify_added_mode);
}

void Settings::load(tr_variant const& src)
{
    auto const* map = src.get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return;
    }

    for (auto& field : fields())
    {
        if (auto const iter = map->find(field.key); iter != std::end(*map))
        {
            auto const type_index = std::type_index{ field.type };
            TR_ASSERT(load_.count(type_index) == 1U);
            load_.at(type_index)(iter->second, field.ptr);
        }
    }
}

tr_variant Settings::save() const
{
    auto const fields = const_cast<Settings*>(this)->fields();

    auto map = tr_variant::Map{ std::size(fields) };

    for (auto const& field : fields)
    {
        auto const type_index = std::type_index{ field.type };
        TR_ASSERT(save_.count(type_index) == 1U);
        map.try_emplace(field.key, save_.at(type_index)(field.ptr));
    }

    return map;
}
} // namespace libtransmission
