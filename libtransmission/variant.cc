// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#if defined(HAVE_USELOCALE) && (!defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < 700)
#undef _XOPEN_SOURCE
#define XOPEN_SOURCE 700 // NOLINT
#endif

#if defined(HAVE_USELOCALE) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <algorithm> // std::sort
#include <cerrno>
#include <cstdlib> /* strtod() */
#include <cstring>
#include <stack>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <share.h>
#endif

#include <clocale> /* setlocale() */

#if defined(HAVE_USELOCALE) && defined(HAVE_XLOCALE_H)
#include <xlocale.h>
#endif

#include <event2/buffer.h>

#include <fmt/core.h>

#define LIBTRANSMISSION_VARIANT_MODULE

#include "transmission.h"

#include "error.h"
#include "file.h"
#include "log.h"
#include "quark.h"
#include "tr-assert.h"
#include "utils.h"
#include "variant-common.h"
#include "variant.h"

/* don't use newlocale/uselocale on old versions of uClibc because they're buggy.
 * https://trac.transmissionbt.com/ticket/6006 */
#if defined(__UCLIBC__) && !TR_UCLIBC_CHECK_VERSION(0, 9, 34)
#undef HAVE_USELOCALE
#endif

/**
***
**/

using namespace std::literals;

struct locale_context
{
#ifdef HAVE_USELOCALE
    locale_t new_locale;
    locale_t old_locale;
#else
#if defined(HAVE__CONFIGTHREADLOCALE) && defined(_ENABLE_PER_THREAD_LOCALE)
    int old_thread_config;
#endif
    int category;
    char old_locale[128];
#endif
};

static void use_numeric_locale(struct locale_context* context, char const* locale_name)
{
#ifdef HAVE_USELOCALE

    context->new_locale = newlocale(LC_NUMERIC_MASK, locale_name, nullptr);
    context->old_locale = uselocale(context->new_locale);

#else

#if defined(HAVE__CONFIGTHREADLOCALE) && defined(_ENABLE_PER_THREAD_LOCALE)
    context->old_thread_config = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
#endif

    context->category = LC_NUMERIC;
    tr_strlcpy(context->old_locale, setlocale(context->category, nullptr), sizeof(context->old_locale));
    setlocale(context->category, locale_name);

#endif
}

static void restore_locale(struct locale_context* context)
{
#ifdef HAVE_USELOCALE

    uselocale(context->old_locale);
    freelocale(context->new_locale);

#else

    setlocale(context->category, context->old_locale);

#if defined(HAVE__CONFIGTHREADLOCALE) && defined(_ENABLE_PER_THREAD_LOCALE)
    _configthreadlocale(context->old_thread_config);
#endif

#endif
}

/***
****
***/

static bool tr_variantIsContainer(tr_variant const* v)
{
    return tr_variantIsList(v) || tr_variantIsDict(v);
}

static bool tr_variantIsSomething(tr_variant const* v)
{
    return tr_variantIsContainer(v) || tr_variantIsInt(v) || tr_variantIsString(v) || tr_variantIsReal(v) ||
        tr_variantIsBool(v);
}

void tr_variantInit(tr_variant* v, char type)
{
    v->type = type;
    memset(&v->val, 0, sizeof(v->val));
}

/***
****
***/

static auto constexpr STRING_INIT = tr_variant_string{
    TR_STRING_TYPE_QUARK,
    0,
    {},
};

static void tr_variant_string_clear(struct tr_variant_string* str)
{
    if (str->type == TR_STRING_TYPE_HEAP)
    {
        tr_free((char*)(str->str.str));
    }

    *str = STRING_INIT;
}

/* returns a const pointer to the variant's string */
static constexpr char const* tr_variant_string_get_string(struct tr_variant_string const* str)
{
    switch (str->type)
    {
    case TR_STRING_TYPE_BUF:
        return str->str.buf;

    case TR_STRING_TYPE_HEAP:
    case TR_STRING_TYPE_QUARK:
    case TR_STRING_TYPE_VIEW:
        return str->str.str;

    default:
        return nullptr;
    }
}

static void tr_variant_string_set_quark(struct tr_variant_string* str, tr_quark const quark)
{
    tr_variant_string_clear(str);

    str->type = TR_STRING_TYPE_QUARK;
    str->str.str = tr_quark_get_string(quark, &str->len);
}

static void tr_variant_string_set_string_view(struct tr_variant_string* str, std::string_view in)
{
    tr_variant_string_clear(str);

    str->type = TR_STRING_TYPE_VIEW;
    str->len = std::size(in);
    str->str.str = std::data(in);
}

static void tr_variant_string_set_string(struct tr_variant_string* str, std::string_view in)
{
    tr_variant_string_clear(str);

    auto const* const bytes = std::data(in);
    auto const len = std::size(in);

    if (len < sizeof(str->str.buf))
    {
        str->type = TR_STRING_TYPE_BUF;
        if (len > 0)
        {
            std::copy_n(bytes, len, str->str.buf);
        }

        str->str.buf[len] = '\0';
        str->len = len;
    }
    else
    {
        auto* tmp = tr_new(char, len + 1);
        std::copy_n(bytes, len, tmp);
        tmp[len] = '\0';
        str->type = TR_STRING_TYPE_HEAP;
        str->str.str = tmp;
        str->len = len;
    }
}

/***
****
***/

static constexpr char const* getStr(tr_variant const* v)
{
    TR_ASSERT(tr_variantIsString(v));

    return tr_variant_string_get_string(&v->val.s);
}

static int dictIndexOf(tr_variant const* dict, tr_quark const key)
{
    if (tr_variantIsDict(dict))
    {
        for (size_t i = 0; i < dict->val.l.count; ++i)
        {
            if (dict->val.l.vals[i].key == key)
            {
                return (int)i;
            }
        }
    }

    return -1;
}

tr_variant* tr_variantDictFind(tr_variant* dict, tr_quark const key)
{
    int const i = dictIndexOf(dict, key);

    return i < 0 ? nullptr : dict->val.l.vals + i;
}

static bool tr_variantDictFindType(tr_variant* dict, tr_quark const key, int type, tr_variant** setme)
{
    *setme = tr_variantDictFind(dict, key);
    return tr_variantIsType(*setme, type);
}

size_t tr_variantListSize(tr_variant const* list)
{
    return tr_variantIsList(list) ? list->val.l.count : 0;
}

tr_variant* tr_variantListChild(tr_variant* v, size_t i)
{
    tr_variant* ret = nullptr;

    if (tr_variantIsList(v) && i < v->val.l.count)
    {
        ret = v->val.l.vals + i;
    }

    return ret;
}

bool tr_variantListRemove(tr_variant* list, size_t i)
{
    bool removed = false;

    if (tr_variantIsList(list) && i < list->val.l.count)
    {
        removed = true;
        tr_variantFree(&list->val.l.vals[i]);
        tr_removeElementFromArray(list->val.l.vals, i, sizeof(tr_variant), list->val.l.count);
        --list->val.l.count;
    }

    return removed;
}

bool tr_variantGetInt(tr_variant const* v, int64_t* setme)
{
    bool success = false;

    if (tr_variantIsInt(v))
    {
        if (setme != nullptr)
        {
            *setme = v->val.i;
        }

        success = true;
    }

    if (!success && tr_variantIsBool(v))
    {
        if (setme != nullptr)
        {
            *setme = v->val.b ? 1 : 0;
        }

        success = true;
    }

    return success;
}

bool tr_variantGetStrView(tr_variant const* v, std::string_view* setme)
{
    if (!tr_variantIsString(v))
    {
        return false;
    }

    char const* const str = tr_variant_string_get_string(&v->val.s);
    size_t const len = v->val.s.len;
    *setme = std::string_view{ str, len };
    return true;
}

bool tr_variantGetRaw(tr_variant const* v, uint8_t const** setme_raw, size_t* setme_len)
{
    bool const success = tr_variantIsString(v);

    if (success)
    {
        *setme_raw = (uint8_t const*)getStr(v);
        *setme_len = v->val.s.len;
    }

    return success;
}

bool tr_variantGetBool(tr_variant const* v, bool* setme)
{
    if (tr_variantIsBool(v))
    {
        *setme = v->val.b;
        return true;
    }

    if (tr_variantIsInt(v) && (v->val.i == 0 || v->val.i == 1))
    {
        *setme = v->val.i != 0;
        return true;
    }

    if (auto sv = std::string_view{}; tr_variantGetStrView(v, &sv))
    {
        if (sv == "true"sv)
        {
            *setme = true;
            return true;
        }

        if (sv == "false"sv)
        {
            *setme = false;
            return true;
        }
    }

    return false;
}

bool tr_variantGetReal(tr_variant const* v, double* setme)
{
    bool success = false;

    if (tr_variantIsReal(v))
    {
        *setme = v->val.d;
        success = true;
    }

    if (!success && tr_variantIsInt(v))
    {
        *setme = (double)v->val.i;
        success = true;
    }

    if (!success && tr_variantIsString(v))
    {
        /* the json spec requires a '.' decimal point regardless of locale */
        struct locale_context locale_ctx;
        use_numeric_locale(&locale_ctx, "C");
        char* endptr = nullptr;
        double const d = strtod(getStr(v), &endptr);
        restore_locale(&locale_ctx);

        if (getStr(v) != endptr && *endptr == '\0')
        {
            *setme = d;
            success = true;
        }
    }

    return success;
}

bool tr_variantDictFindInt(tr_variant* dict, tr_quark const key, int64_t* setme)
{
    tr_variant const* child = tr_variantDictFind(dict, key);
    return tr_variantGetInt(child, setme);
}

bool tr_variantDictFindBool(tr_variant* dict, tr_quark const key, bool* setme)
{
    tr_variant const* child = tr_variantDictFind(dict, key);
    return tr_variantGetBool(child, setme);
}

bool tr_variantDictFindReal(tr_variant* dict, tr_quark const key, double* setme)
{
    tr_variant const* child = tr_variantDictFind(dict, key);
    return tr_variantGetReal(child, setme);
}

bool tr_variantDictFindStrView(tr_variant* dict, tr_quark const key, std::string_view* setme)
{
    tr_variant const* const child = tr_variantDictFind(dict, key);
    return tr_variantGetStrView(child, setme);
}

bool tr_variantDictFindList(tr_variant* dict, tr_quark const key, tr_variant** setme)
{
    return tr_variantDictFindType(dict, key, TR_VARIANT_TYPE_LIST, setme);
}

bool tr_variantDictFindDict(tr_variant* dict, tr_quark const key, tr_variant** setme)
{
    return tr_variantDictFindType(dict, key, TR_VARIANT_TYPE_DICT, setme);
}

bool tr_variantDictFindRaw(tr_variant* dict, tr_quark const key, uint8_t const** setme_raw, size_t* setme_len)
{
    tr_variant const* child = tr_variantDictFind(dict, key);
    return tr_variantGetRaw(child, setme_raw, setme_len);
}

/***
****
***/

void tr_variantInitRaw(tr_variant* v, void const* src, size_t byteCount)
{
    tr_variantInit(v, TR_VARIANT_TYPE_STR);
    tr_variant_string_set_string(&v->val.s, { static_cast<char const*>(src), byteCount });
}

void tr_variantInitQuark(tr_variant* v, tr_quark const q)
{
    tr_variantInit(v, TR_VARIANT_TYPE_STR);
    tr_variant_string_set_quark(&v->val.s, q);
}

void tr_variantInitStr(tr_variant* v, std::string_view str)
{
    tr_variantInit(v, TR_VARIANT_TYPE_STR);
    tr_variant_string_set_string(&v->val.s, str);
}

void tr_variantInitStrView(tr_variant* v, std::string_view str)
{
    tr_variantInit(v, TR_VARIANT_TYPE_STR);
    tr_variant_string_set_string_view(&v->val.s, str);
}

void tr_variantInitBool(tr_variant* v, bool value)
{
    tr_variantInit(v, TR_VARIANT_TYPE_BOOL);
    v->val.b = value;
}

void tr_variantInitReal(tr_variant* v, double value)
{
    tr_variantInit(v, TR_VARIANT_TYPE_REAL);
    v->val.d = value;
}

void tr_variantInitInt(tr_variant* v, int64_t value)
{
    tr_variantInit(v, TR_VARIANT_TYPE_INT);
    v->val.i = value;
}

void tr_variantInitList(tr_variant* v, size_t reserve_count)
{
    tr_variantInit(v, TR_VARIANT_TYPE_LIST);
    tr_variantListReserve(v, reserve_count);
}

static tr_variant* containerReserve(tr_variant* v, size_t count)
{
    TR_ASSERT(tr_variantIsContainer(v));

    size_t const needed = v->val.l.count + count;

    if (needed > v->val.l.alloc)
    {
        /* scale the alloc size in powers-of-2 */
        size_t n = v->val.l.alloc != 0 ? v->val.l.alloc : 8;

        while (n < needed)
        {
            n *= 2U;
        }

        v->val.l.vals = tr_renew(tr_variant, v->val.l.vals, n);
        v->val.l.alloc = n;
    }

    return v->val.l.vals + v->val.l.count;
}

void tr_variantListReserve(tr_variant* list, size_t count)
{
    TR_ASSERT(tr_variantIsList(list));

    containerReserve(list, count);
}

void tr_variantInitDict(tr_variant* v, size_t reserve_count)
{
    tr_variantInit(v, TR_VARIANT_TYPE_DICT);
    tr_variantDictReserve(v, reserve_count);
}

void tr_variantDictReserve(tr_variant* dict, size_t reserve_count)
{
    TR_ASSERT(tr_variantIsDict(dict));

    containerReserve(dict, reserve_count);
}

tr_variant* tr_variantListAdd(tr_variant* list)
{
    TR_ASSERT(tr_variantIsList(list));

    tr_variant* child = containerReserve(list, 1);
    ++list->val.l.count;
    child->key = 0;
    tr_variantInit(child, TR_VARIANT_TYPE_INT);

    return child;
}

tr_variant* tr_variantListAddInt(tr_variant* list, int64_t val)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitInt(child, val);
    return child;
}

tr_variant* tr_variantListAddReal(tr_variant* list, double val)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitReal(child, val);
    return child;
}

tr_variant* tr_variantListAddBool(tr_variant* list, bool val)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitBool(child, val);
    return child;
}

tr_variant* tr_variantListAddStr(tr_variant* list, std::string_view str)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitStr(child, str);
    return child;
}

tr_variant* tr_variantListAddStrView(tr_variant* list, std::string_view str)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitStrView(child, str);
    return child;
}

tr_variant* tr_variantListAddQuark(tr_variant* list, tr_quark const val)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitQuark(child, val);
    return child;
}

tr_variant* tr_variantListAddRaw(tr_variant* list, void const* val, size_t len)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitRaw(child, val, len);
    return child;
}

tr_variant* tr_variantListAddList(tr_variant* list, size_t reserve_count)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitList(child, reserve_count);
    return child;
}

tr_variant* tr_variantListAddDict(tr_variant* list, size_t reserve_count)
{
    tr_variant* child = tr_variantListAdd(list);
    tr_variantInitDict(child, reserve_count);
    return child;
}

tr_variant* tr_variantDictAdd(tr_variant* dict, tr_quark const key)
{
    TR_ASSERT(tr_variantIsDict(dict));

    tr_variant* val = containerReserve(dict, 1);
    ++dict->val.l.count;
    val->key = key;
    tr_variantInit(val, TR_VARIANT_TYPE_INT);

    return val;
}

static tr_variant* dictFindOrAdd(tr_variant* dict, tr_quark const key, int type)
{
    /* see if it already exists, and if so, try to reuse it */
    tr_variant* child = tr_variantDictFind(dict, key);
    if (child != nullptr)
    {
        if (!tr_variantIsType(child, type))
        {
            tr_variantDictRemove(dict, key);
            child = nullptr;
        }
        else if (child->type == TR_VARIANT_TYPE_STR)
        {
            tr_variant_string_clear(&child->val.s);
        }
    }

    /* if it doesn't exist, create it */
    if (child == nullptr)
    {
        child = tr_variantDictAdd(dict, key);
    }

    return child;
}

tr_variant* tr_variantDictAddInt(tr_variant* dict, tr_quark const key, int64_t val)
{
    tr_variant* child = dictFindOrAdd(dict, key, TR_VARIANT_TYPE_INT);
    tr_variantInitInt(child, val);
    return child;
}

tr_variant* tr_variantDictAddBool(tr_variant* dict, tr_quark const key, bool val)
{
    tr_variant* child = dictFindOrAdd(dict, key, TR_VARIANT_TYPE_BOOL);
    tr_variantInitBool(child, val);
    return child;
}

tr_variant* tr_variantDictAddReal(tr_variant* dict, tr_quark const key, double val)
{
    tr_variant* child = dictFindOrAdd(dict, key, TR_VARIANT_TYPE_REAL);
    tr_variantInitReal(child, val);
    return child;
}

tr_variant* tr_variantDictAddQuark(tr_variant* dict, tr_quark const key, tr_quark const val)
{
    tr_variant* child = dictFindOrAdd(dict, key, TR_VARIANT_TYPE_STR);
    tr_variantInitQuark(child, val);
    return child;
}

tr_variant* tr_variantDictAddStr(tr_variant* dict, tr_quark const key, std::string_view str)
{
    tr_variant* child = dictFindOrAdd(dict, key, TR_VARIANT_TYPE_STR);
    tr_variantInitStr(child, str);
    return child;
}

tr_variant* tr_variantDictAddStrView(tr_variant* dict, tr_quark const key, std::string_view str)
{
    tr_variant* child = dictFindOrAdd(dict, key, TR_VARIANT_TYPE_STR);
    tr_variantInitStrView(child, str);
    return child;
}

tr_variant* tr_variantDictAddRaw(tr_variant* dict, tr_quark const key, void const* src, size_t len)
{
    tr_variant* child = dictFindOrAdd(dict, key, TR_VARIANT_TYPE_STR);
    tr_variantInitRaw(child, src, len);
    return child;
}

tr_variant* tr_variantDictAddList(tr_variant* dict, tr_quark const key, size_t reserve_count)
{
    tr_variant* child = tr_variantDictAdd(dict, key);
    tr_variantInitList(child, reserve_count);
    return child;
}

tr_variant* tr_variantDictAddDict(tr_variant* dict, tr_quark const key, size_t reserve_count)
{
    tr_variant* child = tr_variantDictAdd(dict, key);
    tr_variantInitDict(child, reserve_count);
    return child;
}

tr_variant* tr_variantDictSteal(tr_variant* dict, tr_quark const key, tr_variant* value)
{
    tr_variant* child = tr_variantDictAdd(dict, key);
    *child = *value;
    child->key = key;
    tr_variantInit(value, value->type);
    return child;
}

bool tr_variantDictRemove(tr_variant* dict, tr_quark const key)
{
    bool removed = false;

    if (int const i = dictIndexOf(dict, key); i >= 0)
    {
        int const last = (int)dict->val.l.count - 1;

        tr_variantFree(&dict->val.l.vals[i]);

        if (i != last)
        {
            dict->val.l.vals[i] = dict->val.l.vals[last];
        }

        --dict->val.l.count;

        removed = true;
    }

    return removed;
}

/***
****  BENC WALKING
***/

class WalkNode
{
public:
    explicit WalkNode(tr_variant const* v_in)
    {
        assign(v_in);
    }

    tr_variant const* nextChild()
    {
        if (!tr_variantIsContainer(&v) || (child_index >= v.val.l.count))
        {
            return nullptr;
        }

        auto idx = child_index++;
        if (!sorted.empty())
        {
            idx = sorted[idx];
        }

        return v.val.l.vals + idx;
    }

    bool is_visited = false;

    // shallow bitwise copy of the variant passed to the constructor
    tr_variant v = {};

protected:
    friend class VariantWalker;

    void assign(tr_variant const* v_in)
    {
        is_visited = false;
        v = *v_in;
        child_index = 0;
        sorted.clear();
    }

    struct ByKey
    {
        std::string_view key;
        size_t idx;
    };

    void sort(std::vector<ByKey>& sortbuf)
    {
        if (!tr_variantIsDict(&v))
        {
            return;
        }

        auto const n = v.val.l.count;
        auto const* children = v.val.l.vals;

        sortbuf.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            sortbuf[i] = { tr_quark_get_string(children[i].key), i };
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
    size_t child_index = 0;

    // When `v` is a dict, this is its children's indices sorted by key.
    // Bencoded dicts must be sorted, so this is useful when writing benc.
    std::vector<size_t> sorted;
};

class VariantWalker
{
public:
    void emplace(tr_variant const* v_in, bool sort_dicts)
    {
        if (size == std::size(stack))
        {
            stack.emplace_back(v_in);
        }
        else
        {
            stack[size].assign(v_in);
        }

        ++size;

        if (sort_dicts)
        {
            top().sort(sortbuf);
        }
    }

    void pop()
    {
        TR_ASSERT(size > 0);
        if (size > 0)
        {
            --size;
        }
    }

    [[nodiscard]] bool empty() const
    {
        return size == 0;
    }

    WalkNode& top()
    {
        TR_ASSERT(size > 0);
        return stack[size - 1];
    }

private:
    size_t size = 0;
    std::vector<WalkNode> stack;
    std::vector<WalkNode::ByKey> sortbuf;
};

/**
 * This function's previous recursive implementation was
 * easier to read, but was vulnerable to a smash-stacking
 * attack via maliciously-crafted data. (#667)
 */
void tr_variantWalk(tr_variant const* v_in, struct VariantWalkFuncs const* walkFuncs, void* user_data, bool sort_dicts)
{
    auto stack = VariantWalker{};
    stack.emplace(v_in, sort_dicts);

    while (!stack.empty())
    {
        auto& node = stack.top();
        tr_variant const* v = nullptr;

        if (!node.is_visited)
        {
            v = &node.v;
            node.is_visited = true;
        }
        else if ((v = node.nextChild()) != nullptr)
        {
            if (tr_variantIsDict(&node.v))
            {
                auto tmp = tr_variant{};
                tr_variantInitQuark(&tmp, v->key);
                walkFuncs->stringFunc(&tmp, user_data);
            }
        }
        else // finished with this node
        {
            if (tr_variantIsContainer(&node.v))
            {
                walkFuncs->containerEndFunc(&node.v, user_data);
            }

            stack.pop();
            continue;
        }

        if (v != nullptr)
        {
            switch (v->type)
            {
            case TR_VARIANT_TYPE_INT:
                walkFuncs->intFunc(v, user_data);
                break;

            case TR_VARIANT_TYPE_BOOL:
                walkFuncs->boolFunc(v, user_data);
                break;

            case TR_VARIANT_TYPE_REAL:
                walkFuncs->realFunc(v, user_data);
                break;

            case TR_VARIANT_TYPE_STR:
                walkFuncs->stringFunc(v, user_data);
                break;

            case TR_VARIANT_TYPE_LIST:
                if (v == &node.v)
                {
                    walkFuncs->listBeginFunc(v, user_data);
                }
                else
                {
                    stack.emplace(v, sort_dicts);
                }
                break;

            case TR_VARIANT_TYPE_DICT:
                if (v == &node.v)
                {
                    walkFuncs->dictBeginFunc(v, user_data);
                }
                else
                {
                    stack.emplace(v, sort_dicts);
                }
                break;

            default:
                /* did caller give us an uninitialized val? */
                tr_logAddError(_("Invalid metadata"));
                break;
            }
        }
    }
}

/****
*****
****/

static void freeDummyFunc(tr_variant const* /*v*/, void* /*buf*/)
{
}

static void freeStringFunc(tr_variant const* v, void* /*user_data*/)
{
    tr_variant_string_clear(&((tr_variant*)v)->val.s);
}

static void freeContainerEndFunc(tr_variant const* v, void* /*user_data*/)
{
    tr_free(v->val.l.vals);
}

static struct VariantWalkFuncs const freeWalkFuncs = {
    freeDummyFunc, //
    freeDummyFunc, //
    freeDummyFunc, //
    freeStringFunc, //
    freeDummyFunc, //
    freeDummyFunc, //
    freeContainerEndFunc, //
};

void tr_variantFree(tr_variant* v)
{
    if (tr_variantIsSomething(v))
    {
        tr_variantWalk(v, &freeWalkFuncs, nullptr, false);
    }
}

/***
****
***/

static void tr_variantListCopy(tr_variant* target, tr_variant const* src)
{
    int i = 0;
    tr_variant const* val = nullptr;

    while ((val = tr_variantListChild(const_cast<tr_variant*>(src), i)) != nullptr)
    {
        if (tr_variantIsBool(val))
        {
            bool boolVal = false;
            tr_variantGetBool(val, &boolVal);
            tr_variantListAddBool(target, boolVal);
        }
        else if (tr_variantIsReal(val))
        {
            double realVal = 0;
            tr_variantGetReal(val, &realVal);
            tr_variantListAddReal(target, realVal);
        }
        else if (tr_variantIsInt(val))
        {
            int64_t intVal = 0;
            tr_variantGetInt(val, &intVal);
            tr_variantListAddInt(target, intVal);
        }
        else if (tr_variantIsString(val))
        {
            auto sv = std::string_view{};
            (void)tr_variantGetStrView(val, &sv);
            tr_variantListAddRaw(target, std::data(sv), std::size(sv));
        }
        else if (tr_variantIsDict(val))
        {
            tr_variantMergeDicts(tr_variantListAddDict(target, 0), val);
        }
        else if (tr_variantIsList(val))
        {
            tr_variantListCopy(tr_variantListAddList(target, 0), val);
        }
        else
        {
            tr_logAddWarn("tr_variantListCopy skipping item");
        }

        ++i;
    }
}

static size_t tr_variantDictSize(tr_variant const* dict)
{
    return tr_variantIsDict(dict) ? dict->val.l.count : 0;
}

bool tr_variantDictChild(tr_variant* dict, size_t n, tr_quark* key, tr_variant** val)
{
    TR_ASSERT(tr_variantIsDict(dict));

    bool success = false;

    if (tr_variantIsDict(dict) && n < dict->val.l.count)
    {
        *key = dict->val.l.vals[n].key;
        *val = dict->val.l.vals + n;
        success = true;
    }

    return success;
}

void tr_variantMergeDicts(tr_variant* target, tr_variant const* source)
{
    TR_ASSERT(tr_variantIsDict(target));
    TR_ASSERT(tr_variantIsDict(source));

    size_t const sourceCount = tr_variantDictSize(source);

    tr_variantDictReserve(target, sourceCount + tr_variantDictSize(target));

    for (size_t i = 0; i < sourceCount; ++i)
    {
        auto key = tr_quark{};
        tr_variant* val = nullptr;
        if (tr_variantDictChild(const_cast<tr_variant*>(source), i, &key, &val))
        {
            tr_variant* t = nullptr;

            // if types differ, ensure that target will overwrite source
            auto const* const target_child = tr_variantDictFind(target, key);
            if ((target_child != nullptr) && !tr_variantIsType(target_child, val->type))
            {
                tr_variantDictRemove(target, key);
            }

            if (tr_variantIsBool(val))
            {
                bool boolVal = false;
                tr_variantGetBool(val, &boolVal);
                tr_variantDictAddBool(target, key, boolVal);
            }
            else if (tr_variantIsReal(val))
            {
                double realVal = 0;
                tr_variantGetReal(val, &realVal);
                tr_variantDictAddReal(target, key, realVal);
            }
            else if (tr_variantIsInt(val))
            {
                int64_t intVal = 0;
                tr_variantGetInt(val, &intVal);
                tr_variantDictAddInt(target, key, intVal);
            }
            else if (tr_variantIsString(val))
            {
                auto sv = std::string_view{};
                (void)tr_variantGetStrView(val, &sv);
                tr_variantDictAddRaw(target, key, std::data(sv), std::size(sv));
            }
            else if (tr_variantIsDict(val) && tr_variantDictFindDict(target, key, &t))
            {
                tr_variantMergeDicts(t, val);
            }
            else if (tr_variantIsList(val))
            {
                if (tr_variantDictFind(target, key) == nullptr)
                {
                    tr_variantListCopy(tr_variantDictAddList(target, key, tr_variantListSize(val)), val);
                }
            }
            else if (tr_variantIsDict(val))
            {
                tr_variant* target_dict = tr_variantDictFind(target, key);

                if (target_dict == nullptr)
                {
                    target_dict = tr_variantDictAddDict(target, key, tr_variantDictSize(val));
                }

                if (tr_variantIsDict(target_dict))
                {
                    tr_variantMergeDicts(target_dict, val);
                }
            }
            else
            {
                tr_logAddDebug(fmt::format("tr_variantMergeDicts skipping '{}'", tr_quark_get_string(key)));
            }
        }
    }
}

/***
****
***/

struct evbuffer* tr_variantToBuf(tr_variant const* v, tr_variant_fmt fmt)
{
    struct locale_context locale_ctx;
    struct evbuffer* buf = evbuffer_new();

    /* parse with LC_NUMERIC="C" to ensure a "." decimal separator */
    use_numeric_locale(&locale_ctx, "C");

    evbuffer_expand(buf, 4096); /* alloc a little memory to start off with */

    switch (fmt)
    {
    case TR_VARIANT_FMT_BENC:
        tr_variantToBufBenc(v, buf);
        break;

    case TR_VARIANT_FMT_JSON:
        tr_variantToBufJson(v, buf, false);
        break;

    case TR_VARIANT_FMT_JSON_LEAN:
        tr_variantToBufJson(v, buf, true);
        break;
    }

    /* restore the previous locale */
    restore_locale(&locale_ctx);
    return buf;
}

std::string tr_variantToStr(tr_variant const* v, tr_variant_fmt fmt)
{
    auto* const buf = tr_variantToBuf(v, fmt);
    auto const n = evbuffer_get_length(buf);
    auto str = std::string{};
    str.resize(n);
    evbuffer_copyout(buf, std::data(str), n);
    evbuffer_free(buf);
    return str;
}

int tr_variantToFile(tr_variant const* v, tr_variant_fmt fmt, std::string_view filename)
{
    auto error_code = int{ 0 };
    auto const contents = tr_variantToStr(v, fmt);

    tr_error* error = nullptr;
    tr_saveFile(filename, contents, &error);
    if (error != nullptr)
    {
        tr_logAddError(fmt::format(
            _("Couldn't save '{path}': {error} ({error_code})"),
            fmt::arg("path", filename),
            fmt::arg("error", error->message),
            fmt::arg("error_code", error->code)));
        error_code = error->code;
        tr_error_clear(&error);
    }

    return error_code;
}

/***
****
***/

bool tr_variantFromBuf(tr_variant* setme, int opts, std::string_view buf, char const** setme_end, tr_error** error)
{
    // supported formats: benc, json
    TR_ASSERT((opts & (TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_JSON)) != 0);

    // parse with LC_NUMERIC="C" to ensure a "." decimal separator
    auto locale_ctx = locale_context{};
    use_numeric_locale(&locale_ctx, "C");

    *setme = {};

    auto const success = ((opts & TR_VARIANT_PARSE_BENC) != 0) ? tr_variantParseBenc(*setme, opts, buf, setme_end, error) :
                                                                 tr_variantParseJson(*setme, opts, buf, setme_end, error);

    if (!success)
    {
        tr_variantFree(setme);
    }

    /* restore the previous locale */
    restore_locale(&locale_ctx);

    return success;
}

bool tr_variantFromFile(tr_variant* setme, tr_variant_parse_opts opts, std::string_view filename, tr_error** error)
{
    // can't do inplace when this function is allocating & freeing the memory...
    TR_ASSERT((opts & TR_VARIANT_PARSE_INPLACE) == 0);

    auto buf = std::vector<char>{};
    if (!tr_loadFile(filename, buf, error))
    {
        return false;
    }

    auto const sv = std::string_view{ std::data(buf), std::size(buf) };
    return tr_variantFromBuf(setme, opts, sv, nullptr, error);
}
