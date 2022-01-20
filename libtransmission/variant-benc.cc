// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cctype> /* isdigit() */
#include <deque>
#include <cerrno>
#include <string_view>
#include <optional>

#include <event2/buffer.h>

#define LIBTRANSMISSION_VARIANT_MODULE

#include "transmission.h"

#include "tr-assert.h"
#include "quark.h"
#include "utils.h" /* tr_snprintf() */
#include "variant-common.h"
#include "variant.h"

using namespace std::literals;

auto constexpr MaxBencStrLength = size_t{ 128 * 1024 * 1024 }; // arbitrary

/***
****  tr_variantParse()
****  tr_variantLoad()
***/

/**
 * The initial i and trailing e are beginning and ending delimiters.
 * You can have negative numbers such as i-3e. You cannot prefix the
 * number with a zero such as i04e. However, i0e is valid.
 * Example: i3e represents the integer "3"
 *
 * The maximum number of bit of this integer is unspecified,
 * but to handle it as a signed 64bit integer is mandatory to handle
 * "large files" aka .torrent for more that 4Gbyte
 */
std::optional<int64_t> tr_bencParseInt(std::string_view* benc)
{
    auto constexpr Prefix = "i"sv;
    auto constexpr Suffix = "e"sv;

    // find the beginning delimiter
    auto walk = *benc;
    if (std::size(walk) < 3 || !tr_strvStartsWith(walk, Prefix))
    {
        return {};
    }

    // find the ending delimiter
    walk.remove_prefix(std::size(Prefix));
    auto const pos = walk.find(Suffix);
    if (pos == std::string_view::npos)
    {
        return {};
    }

    // leading zeroes are not allowed
    if ((walk[0] == '0' && isdigit(walk[1])) || (walk[0] == '-' && walk[1] == '0' && isdigit(walk[2])))
    {
        return {};
    }

    // parse the string and make sure the next char is `Suffix`
    auto const value = tr_parseNum<int64_t>(walk);
    if (!value || !tr_strvStartsWith(walk, Suffix))
    {
        return {};
    }

    walk.remove_prefix(std::size(Suffix));
    *benc = walk;
    return *value;
}

/**
 * Byte strings are encoded as follows:
 * <string length encoded in base ten ASCII>:<string data>
 * Note that there is no constant beginning delimiter, and no ending delimiter.
 * Example: 4:spam represents the string "spam"
 */
std::optional<std::string_view> tr_bencParseStr(std::string_view* benc)
{
    // find the ':' delimiter
    auto const colon_pos = benc->find(':');
    if (colon_pos == std::string_view::npos)
    {
        return {};
    }

    // get the string length
    auto svtmp = benc->substr(0, colon_pos);
    auto const len = tr_parseNum<size_t>(svtmp);
    if (!len || *len >= MaxBencStrLength)
    {
        return {};
    }

    // do we have `len` bytes of string data?
    svtmp = benc->substr(colon_pos + 1);
    if (std::size(svtmp) < len)
    {
        return {};
    }

    auto const string = svtmp.substr(0, *len);
    *benc = svtmp.substr(*len);
    return string;
}

static tr_variant* get_node(std::deque<tr_variant*>& stack, std::optional<tr_quark>& dict_key, tr_variant* top, int* err)
{
    tr_variant* node = nullptr;

    if (std::empty(stack))
    {
        node = top;
    }
    else
    {
        auto* parent = stack.back();

        if (tr_variantIsList(parent))
        {
            node = tr_variantListAdd(parent);
        }
        else if (dict_key && tr_variantIsDict(parent))
        {
            node = tr_variantDictAdd(parent, *dict_key);
            dict_key.reset();
        }
        else
        {
            *err = EILSEQ;
        }
    }

    return node;
}

/**
 * This function's previous recursive implementation was
 * easier to read, but was vulnerable to a smash-stacking
 * attack via maliciously-crafted bencoded data. (#667)
 */
int tr_variantParseBenc(tr_variant& top, int parse_opts, std::string_view benc, char const** setme_end)
{
    TR_ASSERT((parse_opts & TR_VARIANT_PARSE_BENC) != 0);

    auto stack = std::deque<tr_variant*>{};
    auto key = std::optional<tr_quark>{};

    tr_variantInit(&top, 0);

    int err = 0;
    for (;;)
    {
        if (std::empty(benc))
        {
            err = EILSEQ;
        }

        if (err != 0)
        {
            break;
        }

        switch (benc.front())
        {
        case 'i': // int
            {
                auto const value = tr_bencParseInt(&benc);
                if (!value)
                {
                    break;
                }

                if (tr_variant* const v = get_node(stack, key, &top, &err); v != nullptr)
                {
                    tr_variantInitInt(v, *value);
                }
                break;
            }
        case 'l': // list
            benc.remove_prefix(1);

            if (tr_variant* const v = get_node(stack, key, &top, &err); v != nullptr)
            {
                tr_variantInitList(v, 0);
                stack.push_back(v);
            }
            break;

        case 'd': // dict
            benc.remove_prefix(1);

            if (tr_variant* const v = get_node(stack, key, &top, &err); v != nullptr)
            {
                tr_variantInitDict(v, 0);
                stack.push_back(v);
            }
            break;
        case 'e': // end of list or dict
            benc.remove_prefix(1);

            if (std::empty(stack) || key)
            {
                err = EILSEQ;
                break;
            }

            stack.pop_back();
            break;

        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': // string?
            {
                auto const sv = tr_bencParseStr(&benc);
                if (!sv)
                {
                    benc.remove_prefix(1);
                    break;
                }

                if (!key && !std::empty(stack) && tr_variantIsDict(stack.back()))
                {
                    key = tr_quark_new(*sv);
                }
                else
                {
                    tr_variant* const v = get_node(stack, key, &top, &err);
                    if (v != nullptr)
                    {
                        if ((parse_opts & TR_VARIANT_PARSE_INPLACE) != 0)
                        {
                            tr_variantInitStrView(v, *sv);
                        }
                        else
                        {
                            tr_variantInitStr(v, *sv);
                        }
                    }
                }
                break;
            }
        default: // invalid bencoded text... march past it
            benc.remove_prefix(1);
            break;
        }

        if (std::empty(stack))
        {
            break;
        }
    }

    if (err == 0 && (top.type == 0 || !std::empty(stack)))
    {
        err = EILSEQ;
    }

    if (err == 0)
    {
        if (setme_end != nullptr)
        {
            *setme_end = std::data(benc);
        }
    }
    else if (top.type != 0)
    {
        tr_variantFree(&top);
        tr_variantInit(&top, 0);
    }

    return err;
}

/****
*****
****/

static void saveIntFunc(tr_variant const* val, void* vevbuf)
{
    auto* evbuf = static_cast<struct evbuffer*>(vevbuf);
    evbuffer_add_printf(evbuf, "i%" PRId64 "e", val->val.i);
}

static void saveBoolFunc(tr_variant const* val, void* vevbuf)
{
    auto* evbuf = static_cast<struct evbuffer*>(vevbuf);
    if (val->val.b)
    {
        evbuffer_add(evbuf, "i1e", 3);
    }
    else
    {
        evbuffer_add(evbuf, "i0e", 3);
    }
}

static void saveRealFunc(tr_variant const* val, void* vevbuf)
{
    auto buf = std::array<char, 64>{};
    int const len = tr_snprintf(std::data(buf), std::size(buf), "%f", val->val.d);

    auto* evbuf = static_cast<evbuffer*>(vevbuf);
    evbuffer_add_printf(evbuf, "%d:", len);
    evbuffer_add(evbuf, std::data(buf), len);
}

static void saveStringFunc(tr_variant const* v, void* vevbuf)
{
    auto sv = std::string_view{};
    (void)!tr_variantGetStrView(v, &sv);

    auto* evbuf = static_cast<struct evbuffer*>(vevbuf);
    evbuffer_add_printf(evbuf, "%zu:", std::size(sv));
    evbuffer_add(evbuf, std::data(sv), std::size(sv));
}

static void saveDictBeginFunc(tr_variant const* /*val*/, void* vevbuf)
{
    auto* evbuf = static_cast<struct evbuffer*>(vevbuf);
    evbuffer_add(evbuf, "d", 1);
}

static void saveListBeginFunc(tr_variant const* /*val*/, void* vevbuf)
{
    auto* evbuf = static_cast<struct evbuffer*>(vevbuf);
    evbuffer_add(evbuf, "l", 1);
}

static void saveContainerEndFunc(tr_variant const* /*val*/, void* vevbuf)
{
    auto* evbuf = static_cast<struct evbuffer*>(vevbuf);
    evbuffer_add(evbuf, "e", 1);
}

static struct VariantWalkFuncs const walk_funcs = {
    saveIntFunc, //
    saveBoolFunc, //
    saveRealFunc, //
    saveStringFunc, //
    saveDictBeginFunc, //
    saveListBeginFunc, //
    saveContainerEndFunc, //
};

void tr_variantToBufBenc(tr_variant const* top, struct evbuffer* buf)
{
    tr_variantWalk(top, &walk_funcs, buf, true);
}
