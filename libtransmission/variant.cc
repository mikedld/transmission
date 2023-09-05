// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::sort
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <share.h>
#endif

#include <fmt/core.h>

#include <small/vector.hpp>

#define LIBTRANSMISSION_VARIANT_MODULE

#include "libtransmission/error.h"
#include "libtransmission/log.h"
#include "libtransmission/quark.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"

using namespace std::literals;

namespace
{
constexpr bool variant_is_container(tr_variant const* const var)
{
    return var != nullptr && (var->holds_alternative<tr_variant::Vector>() || var->holds_alternative<tr_variant::Map>());
}

constexpr int variant_index(tr_variant const* const var)
{
    if (var != nullptr)
    {
        return var->index();
    }

    return tr_variant::NoneIndex;
}
} // namespace

// ---

tr_variant::StringHolder::StringHolder(std::string&& str) noexcept
    : str_{ std::move(str) }
{
    sv_ = str_;
}

tr_variant::StringHolder::StringHolder(StringHolder&& that) noexcept
{
    *this = std::move(that);
}

void tr_variant::StringHolder::set_unmanaged(std::string_view sv)
{
    str_.clear();
    sv_ = sv;
}

tr_variant::StringHolder& tr_variant::StringHolder::operator=(StringHolder&& that) noexcept
{
    auto const managed = std::data(that.sv_) == std::data(that.str_);
    std::swap(str_, that.str_);
    sv_ = managed ? str_ : that.sv_;
    return *this;
}

// ---

tr_variant::Merge::Merge(tr_variant& tgt)
    : tgt_{ tgt }
{
}

void tr_variant::Merge::operator()(std::monostate const& src)
{
    tgt_ = src;
}
void tr_variant::Merge::operator()(bool const& src)
{
    tgt_ = src;
}
void tr_variant::Merge::operator()(int64_t const& src)
{
    tgt_ = src;
}
void tr_variant::Merge::operator()(double const& src)
{
    tgt_ = src;
}
void tr_variant::Merge::operator()(tr_variant::StringHolder const& src)
{
    tgt_ = src.sv_;
}

void tr_variant::Merge::operator()(tr_variant::Vector const& src)
{
    auto const n_items = std::size(src);
    auto& tgt = tgt_.val_.emplace<Vector>();
    tgt.resize(n_items);
    for (size_t i = 0; i < n_items; ++i)
    {
        std::visit(Merge{ tgt[i] }, src[i].val_);
    }
}

void tr_variant::Merge::operator()(tr_variant::Map const& src)
{
    if (tgt_.index() != tr_variant::MapIndex)
    {
        tgt_.val_.emplace<tr_variant::Map>();
    }
    auto* const tgt = tgt_.get_if<tr_variant::MapIndex>();
    for (auto const& [key, val] : src)
    {
        std::visit(Merge{ (*tgt)[key] }, val.val_);
    }
}

// ---

tr_variant* tr_variantDictFind(tr_variant* const var, tr_quark key)
{
    if (auto* const map = var != nullptr ? var->get_if<tr_variant::MapIndex>() : nullptr; map != nullptr)
    {
        if (auto iter = map->find(key); iter != std::end(*map))
        {
            return &iter->second;
        }
    }

    return {};
}

tr_variant* tr_variantListChild(tr_variant* const var, size_t pos)
{
    if (auto* const vec = var != nullptr ? var->get_if<tr_variant::VectorIndex>() : nullptr; vec != nullptr)
    {
        if (pos < std::size(*vec))
        {
            return &vec->at(pos);
        }
    }

    return {};
}

bool tr_variantListRemove(tr_variant* const var, size_t pos)
{
    if (auto* const vec = var != nullptr ? var->get_if<tr_variant::VectorIndex>() : nullptr;
        vec != nullptr && pos < std::size(*vec))
    {
        vec->erase(std::begin(*vec) + pos);
        return true;
    }

    return false;
}

bool tr_variantGetInt(tr_variant const* const var, int64_t* setme)
{
    switch (variant_index(var))
    {
    case tr_variant::IntIndex:
        if (setme != nullptr)
        {
            *setme = *var->get_if<tr_variant::IntIndex>();
        }
        return true;

    case tr_variant::BoolIndex:
        if (setme != nullptr)
        {
            *setme = *var->get_if<tr_variant::BoolIndex>() ? 1 : 0;
        }
        return true;

    default:
        return false;
    }
}

bool tr_variantGetStrView(tr_variant const* const var, std::string_view* setme)
{
    switch (variant_index(var))
    {
    case tr_variant::StringIndex:
        *setme = *var->get_if<tr_variant::StringIndex>();
        return true;

    default:
        return false;
    }
}

bool tr_variantGetRaw(tr_variant const* v, std::byte const** setme_raw, size_t* setme_len)
{
    if (auto sv = std::string_view{}; tr_variantGetStrView(v, &sv))
    {
        *setme_raw = reinterpret_cast<std::byte const*>(std::data(sv));
        *setme_len = std::size(sv);
        return true;
    }

    return false;
}

bool tr_variantGetRaw(tr_variant const* v, uint8_t const** setme_raw, size_t* setme_len)
{
    if (auto sv = std::string_view{}; tr_variantGetStrView(v, &sv))
    {
        *setme_raw = reinterpret_cast<uint8_t const*>(std::data(sv));
        *setme_len = std::size(sv);
        return true;
    }

    return false;
}

bool tr_variantGetBool(tr_variant const* const var, bool* setme)
{
    switch (variant_index(var))
    {
    case tr_variant::BoolIndex:
        *setme = *var->get_if<tr_variant::BoolIndex>();
        return true;

    case tr_variant::IntIndex:
        if (auto const val = *var->get_if<tr_variant::IntIndex>(); val == 0 || val == 1)
        {
            *setme = val != 0;
            return true;
        }
        break;

    case tr_variant::StringIndex:
        if (auto const val = *var->get_if<tr_variant::StringIndex>(); val == "true"sv)
        {
            *setme = true;
            return true;
        }
        else if (val == "false"sv)
        {
            *setme = false;
            return true;
        }
        break;

    default:
        break;
    }

    return false;
}

bool tr_variantGetReal(tr_variant const* const var, double* setme)
{
    switch (variant_index(var))
    {
    case tr_variant::DoubleIndex:
        *setme = *var->get_if<tr_variant::DoubleIndex>();
        return true;

    case tr_variant::IntIndex:
        *setme = static_cast<double>(*var->get_if<tr_variant::IntIndex>());
        return true;

    case tr_variant::StringIndex:
        if (auto const val = tr_num_parse<double>(*var->get_if<tr_variant::StringIndex>()); val)
        {
            *setme = *val;
            return true;
        }
        [[fallthrough]];

    default:
        return false;
    }
}

bool tr_variantDictFindInt(tr_variant* const var, tr_quark key, int64_t* setme)
{
    auto const* const child = tr_variantDictFind(var, key);
    return tr_variantGetInt(child, setme);
}

bool tr_variantDictFindBool(tr_variant* const var, tr_quark key, bool* setme)
{
    auto const* const child = tr_variantDictFind(var, key);
    return tr_variantGetBool(child, setme);
}

bool tr_variantDictFindReal(tr_variant* const var, tr_quark key, double* setme)
{
    auto const* const child = tr_variantDictFind(var, key);
    return tr_variantGetReal(child, setme);
}

bool tr_variantDictFindStrView(tr_variant* const var, tr_quark key, std::string_view* setme)
{
    auto const* const child = tr_variantDictFind(var, key);
    return tr_variantGetStrView(child, setme);
}

bool tr_variantDictFindList(tr_variant* const var, tr_quark key, tr_variant** setme)
{
    if (auto* const res = tr_variantDictFind(var, key); res != nullptr && res->holds_alternative<tr_variant::Vector>())
    {
        *setme = res;
        return true;
    }

    return false;
}

bool tr_variantDictFindDict(tr_variant* const var, tr_quark key, tr_variant** setme)
{
    if (auto* const res = tr_variantDictFind(var, key); res != nullptr && res->holds_alternative<tr_variant::Map>())
    {
        *setme = res;
        return true;
    }

    return false;
}

bool tr_variantDictFindRaw(tr_variant* const var, tr_quark key, uint8_t const** setme_raw, size_t* setme_len)
{
    auto const* const child = tr_variantDictFind(var, key);
    return tr_variantGetRaw(child, setme_raw, setme_len);
}

bool tr_variantDictFindRaw(tr_variant* const var, tr_quark key, std::byte const** setme_raw, size_t* setme_len)
{
    auto const* const child = tr_variantDictFind(var, key);
    return tr_variantGetRaw(child, setme_raw, setme_len);
}

// ---

void tr_variantInitReal(tr_variant* initme, double value)
{
    *initme = value;
}

void tr_variantInitBool(tr_variant* initme, bool value)
{
    *initme = value;
}

void tr_variantInitInt(tr_variant* initme, int64_t value)
{
    *initme = value;
}

void tr_variantInitStrView(tr_variant* initme, std::string_view val)
{
    *initme = tr_variant::unmanaged_string(val);
}

void tr_variantInitRaw(tr_variant* initme, void const* value, size_t value_len)
{
    tr_variantInitStr(initme, std::string_view{ static_cast<char const*>(value), value_len });
}

void tr_variantInitQuark(tr_variant* initme, tr_quark value)
{
    tr_variantInitStrView(initme, tr_quark_get_string_view(value));
}

void tr_variantInitStr(tr_variant* initme, std::string_view value)
{
    *initme = value;
}

void tr_variantListReserve(tr_variant* const var, size_t count)
{
    TR_ASSERT(var != nullptr);
    TR_ASSERT(var->holds_alternative<tr_variant::Vector>());

    if (auto* const vec = var != nullptr ? var->get_if<tr_variant::VectorIndex>() : nullptr; vec != nullptr)
    {
        vec->reserve(std::size(*vec) + count);
    }
}

void tr_variantDictReserve(tr_variant* const /*var*/, size_t /*reserve_count*/)
{
}

tr_variant* tr_variantListAdd(tr_variant* const var)
{
    TR_ASSERT(var != nullptr);
    TR_ASSERT(var->holds_alternative<tr_variant::Vector>());

    if (auto* const vec = var != nullptr ? var->get_if<tr_variant::VectorIndex>() : nullptr; vec != nullptr)
    {
        return &vec->emplace_back();
    }

    return nullptr;
}

tr_variant* tr_variantListAddInt(tr_variant* const var, int64_t value)
{
    auto* const child = tr_variantListAdd(var);
    *child = value;
    return child;
}

tr_variant* tr_variantListAddReal(tr_variant* const var, double value)
{
    auto* const child = tr_variantListAdd(var);
    *child = value;
    return child;
}

tr_variant* tr_variantListAddBool(tr_variant* const var, bool value)
{
    auto* const child = tr_variantListAdd(var);
    *child = value;
    return child;
}

tr_variant* tr_variantListAddStr(tr_variant* const var, std::string_view value)
{
    auto* const child = tr_variantListAdd(var);
    *child = value;
    return child;
}

tr_variant* tr_variantListAddStrView(tr_variant* const var, std::string_view value)
{
    auto* const child = tr_variantListAdd(var);
    *child = tr_variant::unmanaged_string(value);
    return child;
}

tr_variant* tr_variantListAddQuark(tr_variant* const var, tr_quark value)
{
    return tr_variantListAddStrView(var, tr_quark_get_string_view(value));
}

tr_variant* tr_variantListAddRaw(tr_variant* const var, void const* value, size_t value_len)
{
    auto* const child = tr_variantListAdd(var);
    *child = std::string_view{ static_cast<char const*>(value), value_len };
    return child;
}

tr_variant* tr_variantListAddList(tr_variant* const var, size_t reserve_count)
{
    auto* const child = tr_variantListAdd(var);
    auto vec = tr_variant::Vector{};
    vec.reserve(reserve_count);
    *child = std::move(vec);
    return child;
}

tr_variant* tr_variantListAddDict(tr_variant* const var, size_t reserve_count)
{
    auto* const child = tr_variantListAdd(var);
    *child = tr_variant::make_map(reserve_count);
    return child;
}

tr_variant* tr_variantDictAdd(tr_variant* const var, tr_quark key)
{
    TR_ASSERT(var != nullptr);
    TR_ASSERT(var->holds_alternative<tr_variant::Map>());

    if (auto* const map = var != nullptr ? var->get_if<tr_variant::MapIndex>() : nullptr; map != nullptr)
    {
        return &(*map)[key];
    }

    return {};
}

tr_variant* tr_variantDictAddInt(tr_variant* const var, tr_quark key, int64_t val)
{
    tr_variantDictRemove(var, key);
    auto* const child = tr_variantDictAdd(var, key);
    tr_variantInitInt(child, val);
    return child;
}

tr_variant* tr_variantDictAddBool(tr_variant* const var, tr_quark key, bool val)
{
    tr_variantDictRemove(var, key);
    auto* const child = tr_variantDictAdd(var, key);
    tr_variantInitBool(child, val);
    return child;
}

tr_variant* tr_variantDictAddReal(tr_variant* const var, tr_quark key, double val)
{
    tr_variantDictRemove(var, key);
    auto* const child = tr_variantDictAdd(var, key);
    tr_variantInitReal(child, val);
    return child;
}

tr_variant* tr_variantDictAddQuark(tr_variant* const var, tr_quark key, tr_quark const val)
{
    tr_variantDictRemove(var, key);
    auto* const child = tr_variantDictAdd(var, key);
    tr_variantInitQuark(child, val);
    return child;
}

tr_variant* tr_variantDictAddStr(tr_variant* const var, tr_quark key, std::string_view val)
{
    tr_variantDictRemove(var, key);
    auto* const child = tr_variantDictAdd(var, key);
    tr_variantInitStr(child, val);
    return child;
}

tr_variant* tr_variantDictAddStrView(tr_variant* const var, tr_quark key, std::string_view val)
{
    tr_variantDictRemove(var, key);
    auto* const child = tr_variantDictAdd(var, key);
    tr_variantInitStrView(child, val);
    return child;
}

tr_variant* tr_variantDictAddRaw(tr_variant* const var, tr_quark key, void const* value, size_t len)
{
    tr_variantDictRemove(var, key);
    auto* const child = tr_variantDictAdd(var, key);
    tr_variantInitRaw(child, value, len);
    return child;
}

tr_variant* tr_variantDictAddList(tr_variant* const var, tr_quark key, size_t reserve_count)
{
    auto* const child = tr_variantDictAdd(var, key);
    *child = tr_variant::make_vector(reserve_count);
    return child;
}

tr_variant* tr_variantDictAddDict(tr_variant* const var, tr_quark key, size_t reserve_count)
{
    auto* const child = tr_variantDictAdd(var, key);
    *child = tr_variant::make_map(reserve_count);
    return child;
}

bool tr_variantDictRemove(tr_variant* const var, tr_quark key)
{
    if (auto* const map = var != nullptr ? var->get_if<tr_variant::MapIndex>() : nullptr; map != nullptr)
    {
        return map->erase(key) != 0U;
    }

    return false;
}

// --- BENC WALKING

class WalkNode
{
public:
    WalkNode() = default;

    explicit WalkNode(tr_variant const* const var)
        : var_{ var }
    {
    }

    std::pair<tr_quark, tr_variant const*> next_child()
    {
        if (var_ == nullptr)
        {
            return {};
        }

        if (auto const* const map = var_->get_if<tr_variant::MapIndex>(); map != nullptr)
        {
            if (auto idx = next_index(); idx < std::size(*map))
            {
                auto iter = std::cbegin(*map);
                std::advance(iter, idx);
                return { iter->first, &iter->second };
            }
        }
        else if (auto const* const vec = var_->get_if<tr_variant::VectorIndex>(); vec != nullptr)
        {
            if (auto idx = next_index(); idx < std::size(*vec))
            {
                return { {}, &vec->at(idx) };
            }
        }

        return {};
    }

    [[nodiscard]] constexpr auto is_visited() const noexcept
    {
        return is_visited_;
    }

    constexpr void set_visited() noexcept
    {
        is_visited_ = true;
    }

    [[nodiscard]] tr_variant const* current() const noexcept
    {
        return var_;
    }

protected:
    friend class VariantWalker;

    tr_variant const* var_ = nullptr;

    bool is_visited_ = false;

    void assign(tr_variant const* v_in)
    {
        var_ = v_in;
        is_visited_ = false;
        child_index_ = 0;
        sorted.clear();
    }

    struct ByKey
    {
        std::string_view key;
        size_t idx = {};
    };

    template<typename Container>
    void sort(Container& sortbuf)
    {
        auto const* const map = var_ != nullptr ? var_->get_if<tr_variant::MapIndex>() : nullptr;
        if (map == nullptr)
        {
            return;
        }

        auto idx = size_t{};
        auto const n = std::size(*map);
        sortbuf.resize(n);
        for (auto const& [key, val] : *map)
        {
            sortbuf[idx] = { tr_quark_get_string_view(key), idx };
            ++idx;
        }

        std::sort(std::begin(sortbuf), std::end(sortbuf), [](ByKey const& a, ByKey const& b) { return a.key < b.key; });

        //  keep the sorted indices

        sorted.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            sorted[i] = sortbuf[i].idx;
        }
    }

private:
    // When walking `v`'s children, this is the index of the next child
    size_t child_index_ = 0;

    // When `v` is a dict, this is its children's indices sorted by key.
    // Bencoded dicts must be sorted, so this is useful when writing benc.
    small::vector<size_t, 128U> sorted;

    [[nodiscard]] size_t next_index()
    {
        auto idx = child_index_++;

        if (idx < std::size(sorted))
        {
            idx = sorted[idx];
        }

        return idx;
    }
};

class VariantWalker
{
public:
    void emplace(tr_variant const* v_in, bool sort_dicts)
    {
        stack_.emplace_back(v_in);

        if (sort_dicts)
        {
            top().sort(sortbuf_);
        }
    }

    void pop()
    {
        TR_ASSERT(!std::empty(stack_));
        stack_.resize(std::size(stack_) - 1U);
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return std::empty(stack_);
    }

    WalkNode& top()
    {
        TR_ASSERT(!std::empty(stack_));
        return stack_.back();
    }

private:
    static auto constexpr InitialCapacity = size_t{ 24U };
    small::vector<WalkNode, InitialCapacity> stack_;
    small::vector<WalkNode::ByKey, InitialCapacity> sortbuf_;
};

/**
 * This function's previous recursive implementation was
 * easier to read, but was vulnerable to a smash-stacking
 * attack via maliciously-crafted data. (#667)
 */
void tr_variant_serde::walk(tr_variant const& top, WalkFuncs const& walk_funcs, void* user_data, bool sort_dicts)
{
    auto stack = VariantWalker{};
    stack.emplace(&top, sort_dicts);

    while (!stack.empty())
    {
        auto& node = stack.top();
        tr_variant const* v = nullptr;

        if (!node.is_visited())
        {
            v = node.current();
            node.set_visited();
        }
        else
        {
            auto [key, child] = node.next_child();

            v = child;

            if (v != nullptr)
            {
                if (node.current()->holds_alternative<tr_variant::Map>())
                {
                    auto const keystr = tr_quark_get_string_view(key);
                    auto tmp = tr_variant{};
                    tr_variantInitQuark(&tmp, key);
                    walk_funcs.string_func(tmp, keystr, user_data);
                }
            }
            else // finished with this node
            {
                if (variant_is_container(node.current()))
                {
                    walk_funcs.container_end_func(*node.current(), user_data);
                }

                stack.pop();
                continue;
            }
        }

        switch (variant_index(v))
        {
        case tr_variant::BoolIndex:
            walk_funcs.bool_func(*v, *v->get_if<tr_variant::BoolIndex>(), user_data);
            break;

        case tr_variant::IntIndex:
            walk_funcs.int_func(*v, *v->get_if<tr_variant::IntIndex>(), user_data);
            break;

        case tr_variant::DoubleIndex:
            walk_funcs.double_func(*v, *v->get_if<tr_variant::DoubleIndex>(), user_data);
            break;

        case tr_variant::StringIndex:
            walk_funcs.string_func(*v, *v->get_if<tr_variant::StringIndex>(), user_data);
            break;

        case tr_variant::VectorIndex:
            if (v == node.current())
            {
                walk_funcs.list_begin_func(*v, user_data);
            }
            else
            {
                stack.emplace(v, sort_dicts);
            }
            break;

        case tr_variant::MapIndex:
            if (v == node.current())
            {
                walk_funcs.dict_begin_func(*v, user_data);
            }
            else
            {
                stack.emplace(v, sort_dicts);
            }
            break;

        default: // NoneIndex:
            break;
        }
    }
}

// ---

bool tr_variantDictChild(tr_variant* const var, size_t pos, tr_quark* key, tr_variant** setme_value)
{
    TR_ASSERT(var != nullptr);
    TR_ASSERT(var->holds_alternative<tr_variant::Map>());

    if (auto* const map = var != nullptr ? var->get_if<tr_variant::MapIndex>() : nullptr;
        map != nullptr && pos < std::size(*map))
    {
        auto iter = std::begin(*map);
        std::advance(iter, pos);
        *key = iter->first;
        *setme_value = &iter->second;
        return true;
    }

    return false;
}

void tr_variantMergeDicts(tr_variant* const tgt, tr_variant const* const src)
{
    TR_ASSERT(tgt != nullptr);
    TR_ASSERT(src != nullptr);
    tgt->merge(*src);
}

// ---

tr_variant_serde::~tr_variant_serde()
{
    tr_error_clear(&error_);
}

std::optional<tr_variant> tr_variant_serde::parse(std::string_view input)
{
    tr_error_clear(&error_);
    return type_ == Type::Json ? parse_json(input) : parse_benc(input);
}

[[nodiscard]] std::optional<tr_variant> tr_variant_serde::parse_file(std::string_view filename)
{
    TR_ASSERT_MSG(!parse_inplace_, "not supported in from_file()");
    parse_inplace_ = false;

    if (auto buf = std::vector<char>{}; tr_file_read(filename, buf, &error_))
    {
        return parse(buf);
    }

    return {};
}

std::string tr_variant_serde::to_string(tr_variant const& var) const
{
    return type_ == Type::Json ? to_json_string(var) : to_benc_string(var);
}

bool tr_variant_serde::to_file(tr_variant const& var, std::string_view filename)
{
    tr_file_save(filename, to_string(var), &error_);

    if (error_ != nullptr)
    {
        tr_logAddError(fmt::format(
            _("Couldn't save '{path}': {error} ({error_code})"),
            fmt::arg("path", filename),
            fmt::arg("error", error_->message),
            fmt::arg("error_code", error_->code)));
        return false;
    }

    return true;
}
