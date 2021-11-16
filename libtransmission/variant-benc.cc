/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <cstdlib>
#include <cctype> /* isdigit() */
#include <deque>
#include <cerrno>
#include <cstdlib> /* strtoul() */
#include <cstring> /* strlen(), memchr() */
#include <string_view>
#include <optional>

#include <event2/buffer.h>

#define LIBTRANSMISSION_VARIANT_MODULE

#include "transmission.h"
#include "utils.h" /* tr_snprintf() */
#include "variant.h"
#include "variant-common.h"

#define MAX_BENC_STR_LENGTH (128 * 1024 * 1024) /* arbitrary */

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
    // find the beginning delimiter
    auto walk = *benc;
    if (std::size(walk) < 3 || walk.front() != 'i')
    {
        return {};
    }

    // find the ending delimiter
    walk.remove_prefix(1);
    auto const pos = walk.find('e');
    if (pos == walk.npos)
    {
        return {};
    }

    // leading zeroes are not allowed
    if ((walk[0] == '0' && isdigit(walk[1])) || (walk[0] == '-' && walk[1] == '0' && isdigit(walk[2])))
    {
        return {};
    }

    errno = 0;
    char* endptr = nullptr;
    auto const value = std::strtoll(std::data(walk), &endptr, 10);
    if (errno != 0 || endptr != std::data(walk) + pos)
    {
        return {};
    }

    walk.remove_prefix(pos + 1);
    *benc = walk;
    return value;
}

#include <iostream> // NOCOMMIT

/**
 * Byte strings are encoded as follows:
 * <string length encoded in base ten ASCII>:<string data>
 * Note that there is no constant beginning delimiter, and no ending delimiter.
 * Example: 4:spam represents the string "spam"
 */
std::optional<std::string_view> tr_bencParseStr(std::string_view* benc)
{
    // find the ':' delimiter
    auto const pos = benc->find(':');
    if (pos == benc->npos)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << " string can't find ':'" << std::endl;
        return {};
    }

    // get the string length
    errno = 0;
    char* ulend = nullptr;
    auto const len = strtoul(std::data(*benc), &ulend, 10);
    if (errno != 0 || ulend != std::data(*benc) + pos || len >= MAX_BENC_STR_LENGTH)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << " string can't find string length" << std::endl;
        return {};
    }

    // do we have `len` bytes of string data?
    auto walk = *benc;
    walk.remove_prefix(pos + 1);
    if (std::size(walk) < len)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << " not enough data" << std::endl;
        return {};
    }

    auto const string = walk.substr(0, len);
    walk.remove_prefix(len);
    *benc = walk;
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
int tr_variantParseBenc(tr_variant& top, std::string_view benc, char const** setme_end)
{
    auto stack = std::deque<tr_variant*>{};
    auto key = std::optional<tr_quark>{};

    tr_variantInit(&top, 0);

    std::cerr << __FILE__ << ':' << __LINE__ << " starting tr_variantParseBenc [" << benc << ']' << std::endl;

    int err = 0;
    for (;;)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << " in loop, benc [" << benc << ']' << std::endl;
        if (std::empty(benc))
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " eilseq" << std::endl;
            err = EILSEQ;
        }

        if (err != 0)
        {
            break;
        }

        if (benc.front() == 'i') // int
        {
            auto const value = tr_bencParseInt(&benc);
            if (!value)
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " int parsing failed" << std::endl;
                break;
            }

            tr_variant* const v = get_node(stack, key, &top, &err);
            if (v != nullptr)
            {
                tr_variantInitInt(v, *value);
            }
        }
        else if (benc.front() == 'l') /* list */
        {
            benc.remove_prefix(1);

            tr_variant* const v = get_node(stack, key, &top, &err);
            if (v != nullptr)
            {
                tr_variantInitList(v, 0);
                stack.push_back(v);
            }
        }
        else if (benc.front() == 'd') /* dict */
        {
            benc.remove_prefix(1);

            tr_variant* const v = get_node(stack, key, &top, &err);
            if (v != nullptr)
            {
                tr_variantInitDict(v, 0);
                stack.push_back(v);
            }
        }
        else if (benc.front() == 'e') /* end of list or dict */
        {
            benc.remove_prefix(1);

            if (std::empty(stack) || key)
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " popped empty stack" << std::endl;
                err = EILSEQ;
                break;
            }

            stack.pop_back();
            if (std::empty(stack))
            {
                break;
            }
        }
        else if (isdigit(benc.front())) /* string? */
        {
            auto const sv = tr_bencParseStr(&benc);
            if (!sv)
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " int parsing failed" << std::endl;
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
                    tr_variantInitStr(v, *sv);
                }
            }
        }
        else /* invalid bencoded text... march past it */
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " invalid char" << std::endl;
            benc.remove_prefix(1);
        }

        if (std::empty(stack))
        {
            break;
        }
    }

    std::cerr << __FILE__ << ':' << __LINE__ << " err " << err << std::endl;
    std::cerr << __FILE__ << ':' << __LINE__ << " top.type " << top.type << std::endl;
    std::cerr << __FILE__ << ':' << __LINE__ << " std::empty(stack) " << std::empty(stack) << std::endl;

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

    std::cerr << __FILE__ << ':' << __LINE__ << " returning err " << err << std::endl;
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
    char buf[128];
    int const len = tr_snprintf(buf, sizeof(buf), "%f", val->val.d);

    auto* evbuf = static_cast<struct evbuffer*>(vevbuf);
    evbuffer_add_printf(evbuf, "%d:", len);
    evbuffer_add(evbuf, buf, len);
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
