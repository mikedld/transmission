// This file Copyright © 2020-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef> // size_t
#include <cstdint> // int64_t
#include <ctime>
#include <optional>
#include <string_view>
#include <vector>

#include <QString>

#include <libtransmission/variant.h>

class QByteArray;

class Speed;
class TorrentHash;
struct Peer;
struct TorrentFile;
struct TrackerStat;

namespace trqt::variant_helpers
{

template<typename T, typename std::enable_if_t<std::is_same_v<T, bool>>* = nullptr>
auto getValue(tr_variant const* variant)
{
    std::optional<T> ret;

    if (auto value = T{}; tr_variantGetBool(variant, &value))
    {
        ret = value;
    }

    return ret;
}

template<
    typename T,
    typename std::enable_if_t<
        std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, int> || std::is_same_v<T, time_t>>* =
        nullptr>
auto getValue(tr_variant const* variant)
{
    std::optional<T> ret;

    if (auto value = int64_t{}; tr_variantGetInt(variant, &value))
    {
        ret = value;
    }

    return ret;
}

template<typename T, typename std::enable_if_t<std::is_same_v<T, double>>* = nullptr>
auto getValue(tr_variant const* variant)
{
    std::optional<T> ret;

    if (auto value = T{}; tr_variantGetReal(variant, &value))
    {
        ret = value;
    }

    return ret;
}

template<typename T, typename std::enable_if_t<std::is_same_v<T, QString>>* = nullptr>
auto getValue(tr_variant const* variant)
{
    std::optional<T> ret;

    if (auto sv = std::string_view{}; tr_variantGetStrView(variant, &sv))
    {
        ret = QString::fromUtf8(std::data(sv), std::size(sv));
    }

    return ret;
}

template<typename T, typename std::enable_if_t<std::is_same_v<T, std::string_view>>* = nullptr>
auto getValue(tr_variant const* variant)
{
    std::optional<T> ret;

    if (auto sv = std::string_view{}; tr_variantGetStrView(variant, &sv))
    {
        ret = std::string_view(std::data(sv), std::size(sv));
    }

    return ret;
}

template<
    typename C,
    typename T = typename C::value_type,
    typename std::enable_if_t<
        std::is_same_v<C, QStringList> || std::is_same_v<C, QList<T>> || std::is_same_v<C, std::vector<T>>>* = nullptr>
auto getValue(tr_variant const* var)
{
    std::optional<C> ret;

    if (var != nullptr && var->holds_alternative<tr_variant::Vector>())
    {
        auto list = C{};

        for (size_t i = 0, n = tr_variantListSize(var); i < n; ++i)
        {
            tr_variant* const child = tr_variantListChild(const_cast<tr_variant*>(var), i);
            auto const value = getValue<T>(child);
            if (value)
            {
                list.push_back(*value);
            }
        }

        ret = list;
    }

    return ret;
}

template<typename T>
bool change(T& setme, T const& value)
{
    bool const changed = setme != value;

    if (changed)
    {
        setme = value;
    }

    return changed;
}

bool change(double& setme, double const& value);
bool change(Speed& setme, tr_variant const* value);
bool change(Peer& setme, tr_variant const* value);
bool change(TorrentFile& setme, tr_variant const* value);
bool change(TorrentHash& setme, tr_variant const* value);
bool change(TrackerStat& setme, tr_variant const* value);

template<typename T>
bool change(T& setme, tr_variant const* variant)
{
    auto const value = getValue<T>(variant);
    return value && change(setme, *value);
}

template<typename T>
bool change(std::vector<T>& setme, tr_variant const* value)
{
    bool changed = false;

    auto const n = tr_variantListSize(value);
    if (setme.size() != n)
    {
        setme.resize(n);
        changed = true;
    }

    for (size_t i = 0; i < n; ++i)
    {
        changed = change(setme[i], tr_variantListChild(const_cast<tr_variant*>(value), i)) || changed;
    }

    return changed;
}

///

template<typename T>
auto dictFind(tr_variant* dict, tr_quark key)
{
    std::optional<T> ret;

    if (auto const* child = tr_variantDictFind(dict, key); child != nullptr)
    {
        ret = getValue<T>(child);
    }

    return ret;
}

///

void variantInit(tr_variant* init_me, bool value);
void variantInit(tr_variant* init_me, int64_t value);
void variantInit(tr_variant* init_me, int value);
void variantInit(tr_variant* init_me, double value);
void variantInit(tr_variant* init_me, QByteArray const& value);
void variantInit(tr_variant* init_me, QString const& value);
void variantInit(tr_variant* init_me, std::string_view value);
void variantInit(tr_variant* init_me, char const* value) = delete; // use string_view

template<typename C, typename T = typename C::value_type>
void variantInit(tr_variant* init_me, C const& value)
{
    *init_me = tr_variant::make_vector(std::size(value));

    for (auto const& item : value)
    {
        variantInit(tr_variantListAdd(init_me), item);
    }
}

template<typename T>
void listAdd(tr_variant* list, T const& value)
{
    variantInit(tr_variantListAdd(list), value);
}

template<typename T>
void dictAdd(tr_variant* dict, tr_quark key, T const& value)
{
    variantInit(tr_variantDictAdd(dict, key), value);
}

} // namespace trqt::variant_helpers
