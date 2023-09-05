// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <algorithm> // std::move()
#include <cstddef> // size_t
#include <cstdint> // int64_t
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits> // std::is_same_v
#include <utility> // std::as_const, std::pair
#include <variant>
#include <vector>

#include "libtransmission/quark.h"

struct tr_error;

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
    enum Type
    {
        NoneIndex = 0,
        BoolIndex = 1,
        IntIndex = 2,
        DoubleIndex = 3,
        StringIndex = 4,
        VectorIndex = 5,
        MapIndex = 6
    };

    class Map : public std::map<tr_quark, tr_variant>
    {
    public:
        template<typename Val>
        [[nodiscard]] Val const* find_if(tr_quark key) const noexcept
        {
            auto const iter = find(key);
            return iter != end() ? iter->second.get_if<Val>() : nullptr;
        }

        template<typename Val>
        [[nodiscard]] std::optional<Val> get_if(tr_quark key) const noexcept
        {
            if (auto const* const val = find_if<Val>(key); val != nullptr)
            {
                return *val;
            }

            return std::nullopt;
        }
    };

    using Vector = std::vector<tr_variant>;

    constexpr tr_variant() noexcept = default;
    tr_variant(tr_variant const&) = delete;
    tr_variant(tr_variant&& that) noexcept = default;
    tr_variant& operator=(tr_variant const&) = delete;
    tr_variant& operator=(tr_variant&& that) noexcept = default;

    template<typename Val>
    explicit tr_variant(Val value)
    {
        *this = std::move(value);
    }

    [[nodiscard]] static auto make_map(size_t /*n_reserve*/ = 0U)
    {
        auto ret = tr_variant{};
        ret.val_.emplace<Map>();
        return ret;
    }

    [[nodiscard]] static auto make_vector(size_t n_reserve = 0U)
    {
        auto ret = tr_variant{};
        ret.val_.emplace<Vector>().reserve(n_reserve);
        return ret;
    }

    template<typename Val>
    tr_variant& operator=(Val value)
    {
        val_ = std::move(value);
        return *this;
    }

    tr_variant& operator=(std::string&& value)
    {
        val_.emplace<StringHolder>(std::move(value));
        return *this;
    }

    tr_variant& operator=(std::string const& value)
    {
        val_.emplace<StringHolder>(std::string(value));
        return *this;
    }

    tr_variant& operator=(std::string_view value)
    {
        val_.emplace<StringHolder>(std::string(value));
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
            auto const* const str = std::get_if<StringHolder>(&val_);
            return str != nullptr ? &str->sv_ : nullptr;
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
    [[nodiscard]] auto* get_if() noexcept
    {
        if constexpr (Index == StringIndex)
        {
            auto const* const str = std::get_if<StringIndex>(&val_);
            return str != nullptr ? &str->sv_ : nullptr;
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

    [[nodiscard]] static tr_variant unmanaged_string(std::string_view val)
    {
        auto ret = tr_variant{};
        ret.val_.emplace<StringHolder>().set_unmanaged(val);
        return ret;
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

    void merge(tr_variant const& that)
    {
        std::visit(Merge{ *this }, that.val_);
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
        void operator()(bool const& src);
        void operator()(int64_t const& src);
        void operator()(double const& src);
        void operator()(tr_variant::StringHolder const& src);
        void operator()(tr_variant::Vector const& src);
        void operator()(tr_variant::Map const& src);

    private:
        tr_variant& tgt_;
    };

    std::variant<std::monostate, bool, int64_t, double, StringHolder, Vector, Map> val_;
};

// --- Strings

bool tr_variantGetStrView(tr_variant const* variant, std::string_view* setme);

void tr_variantInitStr(tr_variant* initme, std::string_view value);
void tr_variantInitQuark(tr_variant* initme, tr_quark value);
void tr_variantInitRaw(tr_variant* initme, void const* value, size_t value_len);
void tr_variantInitStrView(tr_variant* initme, std::string_view val);

bool tr_variantGetRaw(tr_variant const* variant, std::byte const** setme_raw, size_t* setme_len);
bool tr_variantGetRaw(tr_variant const* variant, uint8_t const** setme_raw, size_t* setme_len);

// --- Real Numbers

bool tr_variantGetReal(tr_variant const* variant, double* value_setme);

void tr_variantInitReal(tr_variant* initme, double value);

// --- Booleans

bool tr_variantGetBool(tr_variant const* variant, bool* setme);

void tr_variantInitBool(tr_variant* initme, bool value);

// --- Ints

bool tr_variantGetInt(tr_variant const* var, int64_t* setme);

void tr_variantInitInt(tr_variant* initme, int64_t value);

// --- Lists

void tr_variantListReserve(tr_variant* var, size_t reserve_count);

tr_variant* tr_variantListAdd(tr_variant* var);
tr_variant* tr_variantListAddBool(tr_variant* var, bool value);
tr_variant* tr_variantListAddInt(tr_variant* var, int64_t value);
tr_variant* tr_variantListAddReal(tr_variant* var, double value);
tr_variant* tr_variantListAddStr(tr_variant* var, std::string_view value);
tr_variant* tr_variantListAddStrView(tr_variant* var, std::string_view value);
tr_variant* tr_variantListAddQuark(tr_variant* var, tr_quark value);
tr_variant* tr_variantListAddRaw(tr_variant* var, void const* value, size_t value_len);
tr_variant* tr_variantListAddList(tr_variant* var, size_t reserve_count);
tr_variant* tr_variantListAddDict(tr_variant* var, size_t reserve_count);
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

void tr_variantDictReserve(tr_variant* var, size_t reserve_count);
bool tr_variantDictRemove(tr_variant* var, tr_quark key);

tr_variant* tr_variantDictAdd(tr_variant* var, tr_quark key);
tr_variant* tr_variantDictAddReal(tr_variant* var, tr_quark key, double value);
tr_variant* tr_variantDictAddInt(tr_variant* var, tr_quark key, int64_t value);
tr_variant* tr_variantDictAddBool(tr_variant* var, tr_quark key, bool value);
tr_variant* tr_variantDictAddStr(tr_variant* var, tr_quark key, std::string_view value);
tr_variant* tr_variantDictAddStrView(tr_variant* var, tr_quark key, std::string_view value);
tr_variant* tr_variantDictAddQuark(tr_variant* var, tr_quark key, tr_quark val);
tr_variant* tr_variantDictAddList(tr_variant* var, tr_quark key, size_t reserve_count);
tr_variant* tr_variantDictAddDict(tr_variant* var, tr_quark key, size_t reserve_count);
tr_variant* tr_variantDictAddRaw(tr_variant* var, tr_quark key, void const* value, size_t len);

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
    ~tr_variant_serde();

    static tr_variant_serde benc() noexcept
    {
        return tr_variant_serde{ Type::Benc };
    }

    static tr_variant_serde json() noexcept
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
    tr_error* error_ = nullptr;

private:
    friend tr_variant;

    enum class Type
    {
        Benc,
        Json
    };

    struct WalkFuncs
    {
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

namespace libtransmission
{

struct VariantConverter
{
public:
    template<typename T>
    static std::optional<T> load(tr_variant* src);

    template<typename T>
    static void save(tr_variant* tgt, T const& val);
};

} // namespace libtransmission

/* @} */
