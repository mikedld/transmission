/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <array>
#include <cctype>
#include <deque>
#include <cerrno> /* EILSEQ, EINVAL */
#include <cmath> /* fabs() */
#include <cstdio>
#include <cstring>

#include <event2/buffer.h> /* evbuffer_add() */

#define LIBTRANSMISSION_VARIANT_MODULE

#include "transmission.h"
#include "ConvertUTF.h"
#include "jsonsl.h"
#include "log.h"
#include "tr-assert.h"
#include "utils.h"
#include "variant.h"
#include "variant-common.h"

/* arbitrary value... this is much deeper than our code goes */
#define MAX_DEPTH 64

struct json_wrapper_data
{
    bool has_content;
    char const* key;
    evbuffer* keybuf;
    evbuffer* strbuf;
    int error;
    size_t keylen;
    std::deque<tr_variant*> stack;
    tr_variant* top;
    int parse_opts;

    /* A very common pattern is for a container's children to be similar,
     * e.g. they may all be objects with the same set of keys. So when
     * a container is popped off the stack, remember its size to use as
     * a preallocation heuristic for the next container at that depth. */
    std::array<size_t, MAX_DEPTH> preallocGuess;
};

static tr_variant* get_node(struct jsonsl_st* jsn)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    auto* parent = std::empty(data->stack) ? nullptr : data->stack.back();

    tr_variant* node = nullptr;
    if (parent == nullptr)
    {
        node = data->top;
    }
    else if (tr_variantIsList(parent))
    {
        node = tr_variantListAdd(parent);
    }
    else if (tr_variantIsDict(parent) && data->key != nullptr)
    {
        node = tr_variantDictAdd(parent, tr_quark_new(std::string_view{ data->key, data->keylen }));

        data->key = nullptr;
        data->keylen = 0;
    }

    return node;
}

static void error_handler(jsonsl_t jsn, jsonsl_error_t error, jsonsl_state_st* /*state*/, jsonsl_char_t const* buf)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    tr_logAddError("JSON parse failed at pos %zu: %s -- remaining text \"%.16s\"", jsn->pos, jsonsl_strerror(error), buf);

    data->error = EILSEQ;
}

static int error_callback(jsonsl_t jsn, jsonsl_error_t error, struct jsonsl_state_st* state, jsonsl_char_t* at)
{
    error_handler(jsn, error, state, at);
    return 0; /* bail */
}

static void action_callback_PUSH(
    jsonsl_t jsn,
    jsonsl_action_t /*action*/,
    struct jsonsl_state_st* state,
    jsonsl_char_t const* /*buf*/)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    if ((state->type == JSONSL_T_LIST) || (state->type == JSONSL_T_OBJECT))
    {
        data->has_content = true;
        tr_variant* node = get_node(jsn);
        data->stack.push_back(node);

        int const depth = std::size(data->stack);
        size_t const n = depth < MAX_DEPTH ? data->preallocGuess[depth] : 0;
        if (state->type == JSONSL_T_LIST)
        {
            tr_variantInitList(node, n);
        }
        else
        {
            tr_variantInitDict(node, n);
        }
    }
}

/* like sscanf(in+2, "%4x", &val) but less slow */
static bool decode_hex_string(char const* in, unsigned int* setme)
{
    TR_ASSERT(in != nullptr);

    unsigned int val = 0;
    char const* const end = in + 6;

    TR_ASSERT(in[0] == '\\');
    TR_ASSERT(in[1] == 'u');
    in += 2;

    do
    {
        val <<= 4;

        if ('0' <= *in && *in <= '9')
        {
            val += *in - '0';
        }
        else if ('a' <= *in && *in <= 'f')
        {
            val += *in - 'a' + 10U;
        }
        else if ('A' <= *in && *in <= 'F')
        {
            val += *in - 'A' + 10U;
        }
        else
        {
            return false;
        }
    } while (++in != end);

    *setme = val;
    return true;
}

static char* extract_escaped_string(char const* in, size_t in_len, size_t* len, struct evbuffer* buf)
{
    char const* const in_end = in + in_len;

    evbuffer_drain(buf, evbuffer_get_length(buf));

    while (in < in_end)
    {
        bool unescaped = false;

        if (*in == '\\' && in_end - in >= 2)
        {
            switch (in[1])
            {
            case 'b':
                evbuffer_add(buf, "\b", 1);
                in += 2;
                unescaped = true;
                break;

            case 'f':
                evbuffer_add(buf, "\f", 1);
                in += 2;
                unescaped = true;
                break;

            case 'n':
                evbuffer_add(buf, "\n", 1);
                in += 2;
                unescaped = true;
                break;

            case 'r':
                evbuffer_add(buf, "\r", 1);
                in += 2;
                unescaped = true;
                break;

            case 't':
                evbuffer_add(buf, "\t", 1);
                in += 2;
                unescaped = true;
                break;

            case '/':
                evbuffer_add(buf, "/", 1);
                in += 2;
                unescaped = true;
                break;

            case '"':
                evbuffer_add(buf, "\"", 1);
                in += 2;
                unescaped = true;
                break;

            case '\\':
                evbuffer_add(buf, "\\", 1);
                in += 2;
                unescaped = true;
                break;

            case 'u':
                {
                    if (in_end - in >= 6)
                    {
                        unsigned int val = 0;

                        if (decode_hex_string(in, &val))
                        {
                            UTF32 str32_buf[2] = { val, 0 };
                            UTF32 const* str32_walk = str32_buf;
                            UTF32 const* str32_end = str32_buf + 1;
                            UTF8 str8_buf[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                            UTF8* str8_walk = str8_buf;
                            UTF8* str8_end = str8_buf + 8;

                            if (ConvertUTF32toUTF8(&str32_walk, str32_end, &str8_walk, str8_end, {}) == 0)
                            {
                                evbuffer_add(buf, str8_buf, str8_walk - str8_buf);
                                unescaped = true;
                            }

                            in += 6;
                            break;
                        }
                    }
                }
            }
        }

        if (!unescaped)
        {
            evbuffer_add(buf, in, 1);
            ++in;
        }
    }

    *len = evbuffer_get_length(buf);
    return (char*)evbuffer_pullup(buf, -1);
}

static char const* extract_string(jsonsl_t jsn, struct jsonsl_state_st* state, size_t* len, struct evbuffer* buf)
{
    /* figure out where the string is */
    char const* in_begin = jsn->base + state->pos_begin;
    if (*in_begin == '"')
    {
        in_begin++;
    }

    char const* const in_end = jsn->base + state->pos_cur;
    size_t const in_len = in_end - in_begin;

    if (memchr(in_begin, '\\', in_len) == nullptr)
    {
        /* it's not escaped */
        *len = in_len;
        return in_begin;
    }

    return extract_escaped_string(in_begin, in_len, len, buf);
}

static void action_callback_POP(
    jsonsl_t jsn,
    jsonsl_action_t /*action*/,
    struct jsonsl_state_st* state,
    jsonsl_char_t const* /*buf*/)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    if (state->type == JSONSL_T_STRING)
    {
        auto len = size_t{};
        char const* str = extract_string(jsn, state, &len, data->strbuf);
        if ((data->parse_opts & TR_VARIANT_PARSE_INPLACE) != 0)
        {
            tr_variantInitStrView(get_node(jsn), { str, len });
        }
        else
        {
            tr_variantInitStr(get_node(jsn), { str, len });
        }
        data->has_content = true;
    }
    else if (state->type == JSONSL_T_HKEY)
    {
        data->has_content = true;
        data->key = extract_string(jsn, state, &data->keylen, data->keybuf);
    }
    else if (state->type == JSONSL_T_LIST || state->type == JSONSL_T_OBJECT)
    {
        int const depth = std::size(data->stack);
        auto* v = data->stack.back();
        data->stack.pop_back();
        if (depth < MAX_DEPTH)
        {
            data->preallocGuess[depth] = v->val.l.count;
        }
    }
    else if (state->type == JSONSL_T_SPECIAL)
    {
        if ((state->special_flags & JSONSL_SPECIALf_NUMNOINT) != 0)
        {
            char const* begin = jsn->base + state->pos_begin;
            data->has_content = true;
            tr_variantInitReal(get_node(jsn), strtod(begin, nullptr));
        }
        else if ((state->special_flags & JSONSL_SPECIALf_NUMERIC) != 0)
        {
            char const* begin = jsn->base + state->pos_begin;
            data->has_content = true;
            tr_variantInitInt(get_node(jsn), std::strtoll(begin, nullptr, 10));
        }
        else if ((state->special_flags & JSONSL_SPECIALf_BOOLEAN) != 0)
        {
            bool const b = (state->special_flags & JSONSL_SPECIALf_TRUE) != 0;
            data->has_content = true;
            tr_variantInitBool(get_node(jsn), b);
        }
        else if ((state->special_flags & JSONSL_SPECIALf_NULL) != 0)
        {
            data->has_content = true;
            tr_variantInitQuark(get_node(jsn), TR_KEY_NONE);
        }
    }
}

int tr_variantParseJson(tr_variant& setme, int parse_opts, std::string_view benc, char const** setme_end)
{
    TR_ASSERT((parse_opts & TR_VARIANT_PARSE_JSON) != 0);

    auto data = json_wrapper_data{};

    jsonsl_t jsn = jsonsl_new(MAX_DEPTH);
    jsn->action_callback_PUSH = action_callback_PUSH;
    jsn->action_callback_POP = action_callback_POP;
    jsn->error_callback = error_callback;
    jsn->data = &data;
    jsonsl_enable_all_callbacks(jsn);

    data.error = 0;
    data.has_content = false;
    data.key = nullptr;
    data.keybuf = evbuffer_new();
    data.parse_opts = parse_opts;
    data.preallocGuess = {};
    data.stack = {};
    data.strbuf = evbuffer_new();
    data.top = &setme;

    /* parse it */
    jsonsl_feed(jsn, static_cast<jsonsl_char_t const*>(std::data(benc)), std::size(benc));

    /* EINVAL if there was no content */
    if (data.error == 0 && !data.has_content)
    {
        data.error = EINVAL;
    }

    /* maybe set the end ptr */
    if (setme_end != nullptr)
    {
        *setme_end = std::data(benc) + jsn->pos;
    }

    /* cleanup */
    int const error = data.error;
    evbuffer_free(data.keybuf);
    evbuffer_free(data.strbuf);
    jsonsl_destroy(jsn);
    return error;
}

/****
*****
****/

struct ParentState
{
    int variantType;
    int childIndex;
    int childCount;
};

struct jsonWalk
{
    bool doIndent;
    std::deque<ParentState> parents;
    struct evbuffer* out;
};

static void jsonIndent(struct jsonWalk* data)
{
    static char buf[1024] = { '\0' };

    if (*buf == '\0')
    {
        memset(buf, ' ', sizeof(buf));
        buf[0] = '\n';
    }

    if (data->doIndent)
    {
        evbuffer_add(data->out, buf, std::size(data->parents) * 4 + 1);
    }
}

static void jsonChildFunc(struct jsonWalk* data)
{
    if (!std::empty(data->parents))
    {
        auto& pstate = data->parents.back();

        switch (pstate.variantType)
        {
        case TR_VARIANT_TYPE_DICT:
            {
                int const i = pstate.childIndex;
                ++pstate.childIndex;

                if (i % 2 == 0)
                {
                    evbuffer_add(data->out, ": ", data->doIndent ? 2 : 1);
                }
                else
                {
                    bool const is_last = pstate.childIndex == pstate.childCount;
                    if (!is_last)
                    {
                        evbuffer_add(data->out, ",", 1);
                        jsonIndent(data);
                    }
                }

                break;
            }

        case TR_VARIANT_TYPE_LIST:
            {
                ++pstate.childIndex;
                bool const is_last = pstate.childIndex == pstate.childCount;
                if (!is_last)
                {
                    evbuffer_add(data->out, ",", 1);
                    jsonIndent(data);
                }

                break;
            }

        default:
            break;
        }
    }
}

static void jsonPushParent(struct jsonWalk* data, tr_variant const* v)
{
    int const n_children = tr_variantIsDict(v) ? v->val.l.count * 2 : v->val.l.count;
    data->parents.push_back({ v->type, 0, n_children });
}

static void jsonPopParent(struct jsonWalk* data)
{
    data->parents.pop_back();
}

static void jsonIntFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);
    evbuffer_add_printf(data->out, "%" PRId64, val->val.i);
    jsonChildFunc(data);
}

static void jsonBoolFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);

    if (val->val.b)
    {
        evbuffer_add(data->out, "true", 4);
    }
    else
    {
        evbuffer_add(data->out, "false", 5);
    }

    jsonChildFunc(data);
}

static void jsonRealFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);

    if (fabs(val->val.d - (int)val->val.d) < 0.00001)
    {
        evbuffer_add_printf(data->out, "%d", (int)val->val.d);
    }
    else
    {
        evbuffer_add_printf(data->out, "%.4f", tr_truncd(val->val.d, 4));
    }

    jsonChildFunc(data);
}

static void jsonStringFunc(tr_variant const* val, void* vdata)
{
    struct evbuffer_iovec vec[1];
    auto* data = static_cast<struct jsonWalk*>(vdata);

    auto sv = std::string_view{};
    (void)!tr_variantGetStrView(val, &sv);
    auto const* it = reinterpret_cast<unsigned char const*>(std::data(sv));
    auto const* const end = it + std::size(sv);

    evbuffer_reserve_space(data->out, std::size(sv) * 4, vec, 1);
    auto* out = static_cast<char*>(vec[0].iov_base);
    char const* const outend = out + vec[0].iov_len;

    char* outwalk = out;
    *outwalk++ = '"';

    for (; it != end; ++it)
    {
        switch (*it)
        {
        case '\b':
            *outwalk++ = '\\';
            *outwalk++ = 'b';
            break;

        case '\f':
            *outwalk++ = '\\';
            *outwalk++ = 'f';
            break;

        case '\n':
            *outwalk++ = '\\';
            *outwalk++ = 'n';
            break;

        case '\r':
            *outwalk++ = '\\';
            *outwalk++ = 'r';
            break;

        case '\t':
            *outwalk++ = '\\';
            *outwalk++ = 't';
            break;

        case '"':
            *outwalk++ = '\\';
            *outwalk++ = '"';
            break;

        case '\\':
            *outwalk++ = '\\';
            *outwalk++ = '\\';
            break;

        default:
            if (isprint(*it))
            {
                *outwalk++ = *it;
            }
            else
            {
                UTF8 const* tmp = it;
                UTF32 buf[1] = { 0 };
                UTF32* u32 = buf;
                ConversionResult result = ConvertUTF8toUTF32(&tmp, end, &u32, buf + 1, {});

                if ((result == conversionOK || result == targetExhausted) && tmp != it)
                {
                    outwalk += tr_snprintf(outwalk, outend - outwalk, "\\u%04x", (unsigned int)buf[0]);
                    it = tmp - 1;
                }
            }

            break;
        }
    }

    *outwalk++ = '"';
    vec[0].iov_len = outwalk - out;
    evbuffer_commit_space(data->out, vec, 1);

    jsonChildFunc(data);
}

static void jsonDictBeginFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);

    jsonPushParent(data, val);
    evbuffer_add(data->out, "{", 1);

    if (val->val.l.count != 0)
    {
        jsonIndent(data);
    }
}

static void jsonListBeginFunc(tr_variant const* val, void* vdata)
{
    size_t const nChildren = tr_variantListSize(val);
    auto* data = static_cast<struct jsonWalk*>(vdata);

    jsonPushParent(data, val);
    evbuffer_add(data->out, "[", 1);

    if (nChildren != 0)
    {
        jsonIndent(data);
    }
}

static void jsonContainerEndFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);

    jsonPopParent(data);

    jsonIndent(data);

    if (tr_variantIsDict(val))
    {
        evbuffer_add(data->out, "}", 1);
    }
    else /* list */
    {
        evbuffer_add(data->out, "]", 1);
    }

    jsonChildFunc(data);
}

static struct VariantWalkFuncs const walk_funcs = {
    jsonIntFunc, //
    jsonBoolFunc, //
    jsonRealFunc, //
    jsonStringFunc, //
    jsonDictBeginFunc, //
    jsonListBeginFunc, //
    jsonContainerEndFunc, //
};

void tr_variantToBufJson(tr_variant const* top, struct evbuffer* buf, bool lean)
{
    struct jsonWalk data;

    data.doIndent = !lean;
    data.out = buf;

    tr_variantWalk(top, &walk_funcs, &data, true);

    if (evbuffer_get_length(buf) != 0)
    {
        evbuffer_add_printf(buf, "\n");
    }
}
