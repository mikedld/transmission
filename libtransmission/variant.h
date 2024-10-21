// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <algorithm> // std::move()
#include <cstddef> // size_t
#include <cstdint> // int64_t
#include <optional>
#include <string>
#include <string_view>
#include <type_traits> // std::is_same_v
#include <utility> // std::pair
#include <variant>
#include <vector>

#include "libtransmission/error.h"
#include "libtransmission/quark.h"
#include "libtransmission/tr-macros.h" // TR_CONSTEXPR20

/**
 * A variant that holds typical benc/json types: bool, int,
 * double, string, vectors of variants, and maps of variants.
 * Useful when serializing / deserializing benc/json data.
 *
 * @see tr_variant_serde
 */
struct tr_variant
{
public:
    enum Type : uint8_t
    {
        NoneIndex,
        NullIndex,
        BoolIndex,
        IntIndex,
        DoubleIndex,
        StringIndex,
        VectorIndex,
        MapIndex
    };

    using Vector = std::vector<tr_variant>;

    class Map
    {
    public:
        Map() = default;

        Map(size_t const n_reserve)
        {
            vec_.reserve(n_reserve);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto begin() noexcept
        {
            return std::begin(vec_);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto begin() const noexcept
        {
            return std::cbegin(vec_);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto cbegin() const noexcept
        {
            return std::cbegin(vec_);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto end() noexcept
        {
            return std::end(vec_);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto end() const noexcept
        {
            return std::cend(vec_);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto cend() const noexcept
        {
            return std::cend(vec_);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto find(tr_quark const key) noexcept
        {
            auto const predicate = [key](auto const& item)
            {
                return item.first == key;
            };
            return std::find_if(std::begin(vec_), std::end(vec_), predicate);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto find(tr_quark const key) const noexcept
        {
            auto const predicate = [key](auto const& item)
            {
                return item.first == key;
            };
            return std::find_if(std::cbegin(vec_), std::cend(vec_), predicate);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto size() const noexcept
        {
            return std::size(vec_);
        }

        [[nodiscard]] TR_CONSTEXPR20 auto empty() const noexcept
        {
            return std::empty(vec_);
        }

        void reserve(size_t const new_cap)
        {
            vec_.reserve(std::max(new_cap, size_t{ 16U }));
        }

        auto erase(tr_quark const key)
        {
            if (auto iter = find(key); iter != end())
            {
                vec_.erase(iter);
                return 1U;
            }

            return 0U;
        }

        [[nodiscard]] tr_variant& operator[](tr_quark const& key)
        {
            if (auto const iter = find(key); iter != end())
            {
                return iter->second;
            }

            return vec_.emplace_back(key, tr_variant{}).second;
        }

        template<typename Val>
        std::pair<tr_variant&, bool> try_emplace(tr_quark const key, Val&& val)
        {
            if (auto iter = find(key); iter != end())
            {
                return { iter->second, false };
            }

            return { vec_.emplace_back(key, tr_variant{ std::forward<Val>(val) }).second, true };
        }

        template<typename Val>
        std::pair<tr_variant&, bool> insert_or_assign(tr_quark const key, Val&& val)
        {
            auto res = try_emplace(key, std::forward<Val>(val));
            if (!res.second)
            {
                res.first = std::forward<Val>(val);
            }
            return res;
        }

        // --- custom functions

        template<typename Type>
        [[nodiscard]] TR_CONSTEXPR20 auto find_if(tr_quark const key) noexcept
        {
            auto const iter = find(key);
            return iter != end() ? iter->second.get_if<Type>() : nullptr;
        }

        template<typename Type>
        [[nodiscard]] TR_CONSTEXPR20 auto find_if(tr_quark const key) const noexcept
        {
            auto const iter = find(key);
            return iter != end() ? iter->second.get_if<Type>() : nullptr;
        }

        template<typename Type>
        [[nodiscard]] std::optional<Type> value_if(tr_quark const key) const noexcept
        {
            if (auto it = find(key); it != end())
            {
                return it->second.value_if<Type>();
            }

            return {};
        }

    private:
        using Vector = std::vector<std::pair<tr_quark, tr_variant>>;
        Vector vec_;
    };

    constexpr tr_variant() noexcept = default;
    tr_variant(tr_variant const&) = delete;
    tr_variant(tr_variant&& that) noexcept = default;
    tr_variant& operator=(tr_variant const&) = delete;
    tr_variant& operator=(tr_variant&& that) noexcept = default;

    template<typename Val>
    tr_variant(Val&& value)
    {
        *this = std::forward<Val>(value);
    }

    [[nodiscard]] static auto make_map(size_t const n_reserve = 0U) noexcept
    {
        auto ret = tr_variant{};
        ret.val_.emplace<Map>(n_reserve);
        return ret;
    }

    [[nodiscard]] static auto make_vector(size_t const n_reserve = 0U)
    {
        auto ret = tr_variant{};
        ret.val_.emplace<Vector>().reserve(n_reserve);
        return ret;
    }

    [[nodiscard]] static auto make_raw(void const* value, size_t n_bytes)
    {
        return tr_variant{ std::string_view{ reinterpret_cast<char const*>(value), n_bytes } };
    }

    template<typename CharSpan>
    [[nodiscard]] static auto make_raw(CharSpan const& value)
    {
        static_assert(sizeof(typename CharSpan::value_type) == 1U);
        return make_raw(std::data(value), std::size(value));
    }

    [[nodiscard]] static tr_variant unmanaged_string(std::string_view val)
    {
        auto ret = tr_variant{};
        ret.val_.emplace<StringHolder>().set_unmanaged(val);
        return ret;
    }

    template<typename Val>
    tr_variant& operator=(Val value)
    {
        if constexpr (std::is_same_v<Val, std::nullptr_t>)
        {
            val_.emplace<std::nullptr_t>(value);
        }
        else if constexpr (std::is_same_v<Val, std::string_view>)
        {
            val_.emplace<StringHolder>(std::string{ value });
        }
        // note: std::is_integral_v<bool> is true, so this check
        // must come first to prevent bools from being stored as ints
        else if constexpr (std::is_same_v<Val, bool>)
        {
            val_.emplace<bool>(value);
        }
        else if constexpr (std::is_integral_v<Val> || std::is_enum_v<Val>)
        {
            val_ = static_cast<int64_t>(value);
        }
        else
        {
            val_ = std::move(value);
        }
        return *this;
    }

    tr_variant& operator=(std::string&& value)
    {
        val_.emplace<StringHolder>(std::move(value));
        return *this;
    }

    tr_variant& operator=(std::string const& value)
    {
        *this = std::string{ value };
        return *this;
    }

    tr_variant& operator=(char const* const value)
    {
        *this = std::string{ value != nullptr ? value : "" };
        return *this;
    }

    [[nodiscard]] constexpr auto index() const noexcept
    {
        return val_.index();
    }

    [[nodiscard]] constexpr auto has_value() const noexcept
    {
        return index() != NoneIndex;
    }

    template<typename Val>
    [[nodiscard]] constexpr auto* get_if() noexcept
    {
        if constexpr (std::is_same_v<Val, std::string_view>)
        {
            auto const* const val = std::get_if<StringHolder>(&val_);
            return val != nullptr ? &val->sv_ : nullptr;
        }
        else
        {
            return std::get_if<Val>(&val_);
        }
    }

    template<typename Val>
    [[nodiscard]] constexpr auto const* get_if() const noexcept
    {
        return const_cast<tr_variant*>(this)->get_if<Val>();
    }

    template<size_t Index>
    [[nodiscard]] constexpr auto* get_if() noexcept
    {
        if constexpr (Index == StringIndex)
        {
            auto const* const val = std::get_if<StringIndex>(&val_);
            return val != nullptr ? &val->sv_ : nullptr;
        }
        else
        {
            return std::get_if<Index>(&val_);
        }
    }

    template<size_t Index>
    [[nodiscard]] constexpr auto const* get_if() const noexcept
    {
        return const_cast<tr_variant*>(this)->get_if<Index>();
    }

    template<typename Val>
    [[nodiscard]] constexpr std::optional<Val> value_if() noexcept
    {
        if (auto const* const val = get_if<Val>())
        {
            return *val;
        }

        return {};
    }

    template<typename Val>
    [[nodiscard]] std::optional<Val> value_if() const noexcept
    {
        return const_cast<tr_variant*>(this)->value_if<Val>();
    }

    template<typename Val>
    [[nodiscard]] constexpr bool holds_alternative() const noexcept
    {
        if constexpr (std::is_same_v<Val, std::string_view>)
        {
            return std::holds_alternative<StringHolder>(val_);
        }
        else
        {
            return std::holds_alternative<Val>(val_);
        }
    }

    void clear()
    {
        val_.emplace<std::monostate>();
    }

    tr_variant& merge(tr_variant const& that)
    {
        std::visit(Merge{ *this }, that.val_);
        return *this;
    }

private:
    // Holds a string_view to either an unmanaged/external string or to
    // one owned by the class. If the string is unmanaged, only sv_ is used.
    // If we own the string, then sv_ points to the managed str_.
    class StringHolder
    {
    public:
        StringHolder() = default;
        explicit StringHolder(std::string&& str) noexcept;
        explicit StringHolder(StringHolder&& that) noexcept;
        void set_unmanaged(std::string_view sv);
        StringHolder& operator=(StringHolder&& that) noexcept;
        std::string_view sv_;

    private:
        std::string str_;
    };

    class Merge
    {
    public:
        explicit Merge(tr_variant& tgt);
        void operator()(std::monostate const& src);
        void operator()(std::nullptr_t const& src);
        void operator()(bool const& src);
        void operator()(int64_t const& src);
        void operator()(double const& src);
        void operator()(tr_variant::StringHolder const& src);
        void operator()(tr_variant::Vector const& src);
        void operator()(tr_variant::Map const& src);

    private:
        tr_variant& tgt_;
    };

    std::variant<std::monostate, std::nullptr_t, bool, int64_t, double, StringHolder, Vector, Map> val_;
};

template<>
[[nodiscard]] std::optional<int64_t> tr_variant::value_if() noexcept;
template<>
[[nodiscard]] std::optional<bool> tr_variant::value_if() noexcept;
template<>
[[nodiscard]] std::optional<double> tr_variant::value_if() noexcept;

// --- Strings

bool tr_variantGetStrView(tr_variant const* variant, std::string_view* setme);

bool tr_variantGetRaw(tr_variant const* variant, std::byte const** setme_raw, size_t* setme_len);
bool tr_variantGetRaw(tr_variant const* variant, uint8_t const** setme_raw, size_t* setme_len);

// --- Real Numbers

bool tr_variantGetReal(tr_variant const* variant, double* value_setme);

// --- Booleans

bool tr_variantGetBool(tr_variant const* variant, bool* setme);

// --- Ints

bool tr_variantGetInt(tr_variant const* var, int64_t* setme);

// --- Lists

void tr_variantInitList(tr_variant* initme, size_t n_reserve);
void tr_variantListReserve(tr_variant* var, size_t n_reserve);

tr_variant* tr_variantListAdd(tr_variant* var);
tr_variant* tr_variantListAddBool(tr_variant* var, bool value);
tr_variant* tr_variantListAddInt(tr_variant* var, int64_t value);
tr_variant* tr_variantListAddReal(tr_variant* var, double value);
tr_variant* tr_variantListAddStr(tr_variant* var, std::string_view value);
tr_variant* tr_variantListAddStrView(tr_variant* var, std::string_view value);
tr_variant* tr_variantListAddRaw(tr_variant* var, void const* value, size_t n_bytes);
tr_variant* tr_variantListAddList(tr_variant* var, size_t n_reserve);
tr_variant* tr_variantListAddDict(tr_variant* var, size_t n_reserve);
tr_variant* tr_variantListChild(tr_variant* var, size_t pos);

bool tr_variantListRemove(tr_variant* var, size_t pos);

[[nodiscard]] constexpr size_t tr_variantListSize(tr_variant const* const var)
{
    if (var != nullptr)
    {
        if (auto const* const vec = var->get_if<tr_variant::Vector>(); vec != nullptr)
        {
            return std::size(*vec);
        }
    }

    return {};
}

// --- Dictionaries

void tr_variantInitDict(tr_variant* initme, size_t n_reserve);
void tr_variantDictReserve(tr_variant* var, size_t n_reserve);
bool tr_variantDictRemove(tr_variant* var, tr_quark key);

tr_variant* tr_variantDictAdd(tr_variant* var, tr_quark key);
tr_variant* tr_variantDictAddReal(tr_variant* var, tr_quark key, double value);
tr_variant* tr_variantDictAddInt(tr_variant* var, tr_quark key, int64_t value);
tr_variant* tr_variantDictAddBool(tr_variant* var, tr_quark key, bool value);
tr_variant* tr_variantDictAddStr(tr_variant* var, tr_quark key, std::string_view value);
tr_variant* tr_variantDictAddStrView(tr_variant* var, tr_quark key, std::string_view value);
tr_variant* tr_variantDictAddList(tr_variant* var, tr_quark key, size_t n_reserve);
tr_variant* tr_variantDictAddDict(tr_variant* var, tr_quark key, size_t n_reserve);
tr_variant* tr_variantDictAddRaw(tr_variant* var, tr_quark key, void const* value, size_t n_bytes);

bool tr_variantDictChild(tr_variant* var, size_t pos, tr_quark* setme_key, tr_variant** setme_value);
tr_variant* tr_variantDictFind(tr_variant* var, tr_quark key);
bool tr_variantDictFindList(tr_variant* var, tr_quark key, tr_variant** setme);
bool tr_variantDictFindDict(tr_variant* var, tr_quark key, tr_variant** setme_value);
bool tr_variantDictFindInt(tr_variant* var, tr_quark key, int64_t* setme);
bool tr_variantDictFindReal(tr_variant* var, tr_quark key, double* setme);
bool tr_variantDictFindBool(tr_variant* var, tr_quark key, bool* setme);
bool tr_variantDictFindStrView(tr_variant* var, tr_quark key, std::string_view* setme);
bool tr_variantDictFindRaw(tr_variant* var, tr_quark key, uint8_t const** setme_raw, size_t* setme_len);
bool tr_variantDictFindRaw(tr_variant* var, tr_quark key, std::byte const** setme_raw, size_t* setme_len);

/* this is only quasi-supported. don't rely on it too heavily outside of libT */
void tr_variantMergeDicts(tr_variant* tgt, tr_variant const* src);

/**
 * Helper class for serializing and deserializing benc/json data.
 *
 * @see tr_variant
 */
class tr_variant_serde
{
public:
    [[nodiscard]] static tr_variant_serde benc() noexcept
    {
        return tr_variant_serde{ Type::Benc };
    }

    [[nodiscard]] static tr_variant_serde json() noexcept
    {
        return tr_variant_serde{ Type::Json };
    }

    // Serialize data as compactly as possible, e.g.
    // omit pretty-printing JSON whitespace
    constexpr tr_variant_serde& compact() noexcept
    {
        compact_ = true;
        return *this;
    }

    // When set, assumes that the `input` passed to parse() is valid
    // for the lifespan of the variant and we can use string_views of
    // `input` instead of cloning new strings.
    constexpr tr_variant_serde& inplace() noexcept
    {
        parse_inplace_ = true;
        return *this;
    }

    // ---

    [[nodiscard]] std::optional<tr_variant> parse(std::string_view input);

    template<typename CharSpan>
    [[nodiscard]] std::optional<tr_variant> parse(CharSpan const& input)
    {
        return parse(std::string_view{ std::data(input), std::size(input) });
    }

    [[nodiscard]] std::optional<tr_variant> parse_file(std::string_view filename);

    [[nodiscard]] constexpr char const* end() const noexcept
    {
        return end_;
    }

    // ---

    [[nodiscard]] std::string to_string(tr_variant const& var) const;

    bool to_file(tr_variant const& var, std::string_view filename);

    // ---

    // Tracks errors when parsing / saving
    tr_error error_;

private:
    friend tr_variant;

    enum class Type
    {
        Benc,
        Json
    };

    struct WalkFuncs
    {
        void (*null_func)(tr_variant const& var, std::nullptr_t val, void* user_data);
        void (*int_func)(tr_variant const& var, int64_t val, void* user_data);
        void (*bool_func)(tr_variant const& var, bool val, void* user_data);
        void (*double_func)(tr_variant const& var, double val, void* user_data);
        void (*string_func)(tr_variant const& var, std::string_view val, void* user_data);
        void (*dict_begin_func)(tr_variant const& var, void* user_data);
        void (*list_begin_func)(tr_variant const& var, void* user_data);
        void (*container_end_func)(tr_variant const& var, void* user_data);
    };

    tr_variant_serde(Type type)
        : type_{ type }
    {
    }

    [[nodiscard]] std::optional<tr_variant> parse_json(std::string_view input);
    [[nodiscard]] std::optional<tr_variant> parse_benc(std::string_view input);

    [[nodiscard]] std::string to_json_string(tr_variant const& var) const;
    [[nodiscard]] static std::string to_benc_string(tr_variant const& var);

    static void walk(tr_variant const& top, WalkFuncs const& walk_funcs, void* user_data, bool sort_dicts);

    Type type_;

    bool compact_ = false;

    bool parse_inplace_ = false;

    // This is set to the first unparsed character after `parse()`.
    char const* end_ = nullptr;
};

/* @} */
