// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno> /* EILSEQ, EINVAL */
#include <cmath> /* fabs() */
#include <cstddef> // std::byte
#include <cstdint> // uint16_t
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator> // std::back_inserter
#include <string>
#include <string_view>
#include <utility>

#define UTF_CPP_CPLUSPLUS 201703L
#include <utf8.h>

#include <fmt/core.h>
#include <fmt/compile.h>

#include <jsonsl.h>

#define LIBTRANSMISSION_VARIANT_MODULE

#include "libtransmission/error.h"
#include "libtransmission/quark.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-buffer.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"

using namespace std::literals;

namespace
{
[[nodiscard]] constexpr size_t variant_size(tr_variant const& var) noexcept
{
    switch (var.index())
    {
    case tr_variant::MapIndex:
        return std::size(*var.get_if<tr_variant::Map>());

    case tr_variant::VectorIndex:
        return std::size(*var.get_if<tr_variant::Vector>());

    default:
        return {};
    }
}

namespace parse_helpers
{
/* arbitrary value... this is much deeper than our code goes */
auto constexpr MaxDepth = size_t{ 64 };

struct json_wrapper_data
{
    bool has_content;
    size_t size;
    std::string_view key;
    std::string keybuf;
    std::string strbuf;
    tr_error* error;
    std::deque<tr_variant*> stack;
    tr_variant* top;
    bool inplace = false;

    /* A very common pattern is for a container's children to be similar,
     * e.g. they may all be objects with the same set of keys. So when
     * a container is popped off the stack, remember its size to use as
     * a preallocation heuristic for the next container at that depth. */
    std::array<size_t, MaxDepth> preallocGuess;
};

tr_variant* get_node(struct jsonsl_st* jsn)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    auto* parent = std::empty(data->stack) ? nullptr : data->stack.back();

    tr_variant* node = nullptr;
    if (parent == nullptr)
    {
        node = data->top;
    }
    else if (parent->holds_alternative<tr_variant::Vector>())
    {
        node = tr_variantListAdd(parent);
    }
    else if (parent->holds_alternative<tr_variant::Map>() && !std::empty(data->key))
    {
        node = tr_variantDictAdd(parent, tr_quark_new(data->key));
        data->key = ""sv;
    }

    return node;
}

void error_handler(jsonsl_t jsn, jsonsl_error_t error, jsonsl_state_st* /*state*/, jsonsl_char_t const* buf)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    tr_error_set(
        &data->error,
        EILSEQ,
        fmt::format(
            _("Couldn't parse JSON at position {position} '{text}': {error} ({error_code})"),
            fmt::arg("position", jsn->pos),
            fmt::arg("text", std::string_view{ buf, std::min(size_t{ 16U }, data->size - jsn->pos) }),
            fmt::arg("error", jsonsl_strerror(error)),
            fmt::arg("error_code", static_cast<int>(error))));
}

int error_callback(jsonsl_t jsn, jsonsl_error_t error, struct jsonsl_state_st* state, jsonsl_char_t* at)
{
    error_handler(jsn, error, state, at);
    return 0; /* bail */
}

void action_callback_PUSH(jsonsl_t jsn, jsonsl_action_t /*action*/, struct jsonsl_state_st* state, jsonsl_char_t const* /*buf*/)
{
    auto* const data = static_cast<json_wrapper_data*>(jsn->data);

    if ((state->type == JSONSL_T_LIST) || (state->type == JSONSL_T_OBJECT))
    {
        data->has_content = true;
        tr_variant* node = get_node(jsn);
        data->stack.push_back(node);

        size_t const depth = std::size(data->stack);
        size_t const n = depth < MaxDepth ? data->preallocGuess[depth] : 0;
        if (state->type == JSONSL_T_LIST)
        {
            *node = tr_variant::make_vector(n);
        }
        else
        {
            *node = tr_variant::make_map(n);
        }
    }
}

/* like sscanf(in+2, "%4x", &val) but less slow */
[[nodiscard]] constexpr bool decode_hex_string(char const* in, std::uint16_t& setme)
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

    setme = val;
    return true;
}

template<typename Iter>
void decode_single_uchar(char const*& in, char const* const in_end, Iter& buf16_out_it)
{
    static auto constexpr EscapedUcharLength = 6U;
    if (in_end - in >= EscapedUcharLength && decode_hex_string(in, *buf16_out_it))
    {
        in += EscapedUcharLength;
        ++buf16_out_it;
    }
}

[[nodiscard]] bool decode_escaped_uchar_sequence(char const*& in, char const* const in_end, std::string& buf)
{
    auto buf16 = std::array<std::uint16_t, 2>{};
    auto buf16_out_it = std::begin(buf16);

    decode_single_uchar(in, in_end, buf16_out_it);
    if (in[0] == '\\' && in[1] == 'u')
    {
        decode_single_uchar(in, in_end, buf16_out_it);
    }

    if (buf16_out_it == std::begin(buf16))
    {
        return false;
    }

    try
    {
        utf8::utf16to8(std::begin(buf16), buf16_out_it, std::back_inserter(buf));
    }
    catch (utf8::exception const&) // invalid codepoint
    {
        buf.push_back('?');
    }

    return true;
}

[[nodiscard]] std::string_view extract_escaped_string(char const* in, size_t in_len, std::string& buf)
{
    char const* const in_end = in + in_len;

    buf.clear();

    while (in < in_end)
    {
        bool unescaped = false;

        if (*in == '\\' && in_end - in >= 2)
        {
            switch (in[1])
            {
            case 'b':
                buf.push_back('\b');
                in += 2;
                unescaped = true;
                break;

            case 'f':
                buf.push_back('\f');
                in += 2;
                unescaped = true;
                break;

            case 'n':
                buf.push_back('\n');
                in += 2;
                unescaped = true;
                break;

            case 'r':
                buf.push_back('\r');
                in += 2;
                unescaped = true;
                break;

            case 't':
                buf.push_back('\t');
                in += 2;
                unescaped = true;
                break;

            case '/':
                buf.push_back('/');
                in += 2;
                unescaped = true;
                break;

            case '"':
                buf.push_back('"');
                in += 2;
                unescaped = true;
                break;

            case '\\':
                buf.push_back('\\');
                in += 2;
                unescaped = true;
                break;

            case 'u':
                if (decode_escaped_uchar_sequence(in, in_end, buf))
                {
                    unescaped = true;
                    break;
                }
            }
        }

        if (!unescaped)
        {
            buf.push_back(*in);
            ++in;
        }
    }

    return buf;
}

[[nodiscard]] std::pair<std::string_view, bool> extract_string(jsonsl_t jsn, struct jsonsl_state_st* state, std::string& buf)
{
    // figure out where the string is
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
        return std::make_pair(std::string_view{ in_begin, in_len }, true);
    }

    return std::make_pair(extract_escaped_string(in_begin, in_len, buf), false);
}

void action_callback_POP(jsonsl_t jsn, jsonsl_action_t /*action*/, struct jsonsl_state_st* state, jsonsl_char_t const* /*buf*/)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    if (state->type == JSONSL_T_STRING)
    {
        auto const [str, inplace] = extract_string(jsn, state, data->strbuf);
        if (inplace && data->inplace)
        {
            tr_variantInitStrView(get_node(jsn), str);
        }
        else
        {
            tr_variantInitStr(get_node(jsn), str);
        }
        data->has_content = true;
    }
    else if (state->type == JSONSL_T_HKEY)
    {
        data->has_content = true;
        auto const [key, inplace] = extract_string(jsn, state, data->keybuf);
        data->key = key;
    }
    else if (state->type == JSONSL_T_LIST || state->type == JSONSL_T_OBJECT)
    {
        auto const depth = std::size(data->stack);
        auto const* const v = data->stack.back();
        data->stack.pop_back();
        if (depth < MaxDepth)
        {
            data->preallocGuess[depth] = variant_size(*v);
        }
    }
    else if (state->type == JSONSL_T_SPECIAL)
    {
        if ((state->special_flags & JSONSL_SPECIALf_NUMNOINT) != 0)
        {
            auto sv = std::string_view{ jsn->base + state->pos_begin, jsn->pos - state->pos_begin };
            tr_variantInitReal(get_node(jsn), tr_num_parse<double>(sv).value_or(0.0));
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

} // namespace parse_helpers
} // namespace

std::optional<tr_variant> tr_variant_serde::parse_json(std::string_view input)
{
    using namespace parse_helpers;

    auto top = tr_variant{};

    auto data = json_wrapper_data{};
    data.error = nullptr;
    data.size = std::size(input);
    data.has_content = false;
    data.key = ""sv;
    data.inplace = parse_inplace_;
    data.preallocGuess = {};
    data.stack = {};
    data.top = &top;

    auto jsn = jsonsl_new(MaxDepth);
    jsn->action_callback_PUSH = action_callback_PUSH;
    jsn->action_callback_POP = action_callback_POP;
    jsn->error_callback = error_callback;
    jsn->data = &data;
    jsonsl_enable_all_callbacks(jsn);

    // parse it
    jsonsl_feed(jsn, static_cast<jsonsl_char_t const*>(std::data(input)), std::size(input));

    // EINVAL if there was no content
    if (data.error == nullptr && !data.has_content)
    {
        tr_error_set(&data.error, EINVAL, "No content");
    }

    end_ = std::data(input) + jsn->pos;

    if (data.error != nullptr)
    {
        tr_error_propagate(&error_, &data.error);
    }

    // cleanup
    jsonsl_destroy(jsn);

    if (error_ == nullptr)
    {
        return std::optional<tr_variant>{ std::move(top) };
    }

    return {};
}

// ---

namespace
{
namespace to_string_helpers
{
struct ParentState
{
    bool is_map = false;
    bool is_list = false;
    size_t child_index;
    size_t child_count;
};

struct JsonWalk
{
    explicit JsonWalk(bool do_indent)
        : doIndent{ do_indent }
    {
    }

    std::deque<ParentState> parents;
    libtransmission::StackBuffer<1024U * 8U, std::byte> out;
    bool doIndent;
};

void jsonIndent(struct JsonWalk* data)
{
    static auto buf = std::array<char, 1024>{};

    if (buf.front() == '\0')
    {
        memset(std::data(buf), ' ', std::size(buf));
        buf[0] = '\n';
    }

    if (data->doIndent)
    {
        data->out.add(std::data(buf), std::size(data->parents) * 4 + 1);
    }
}

void jsonChildFunc(struct JsonWalk* data)
{
    if (std::empty(data->parents))
    {
        return;
    }

    auto& parent_state = data->parents.back();

    if (parent_state.is_map)
    {
        int const i = parent_state.child_index;
        ++parent_state.child_index;

        if (i % 2 == 0)
        {
            data->out.add(data->doIndent ? ": "sv : ":"sv);
        }
        else
        {
            bool const is_last = parent_state.child_index == parent_state.child_count;
            if (!is_last)
            {
                data->out.push_back(',');
                jsonIndent(data);
            }
        }
    }
    else if (parent_state.is_list)
    {
        ++parent_state.child_index;
        if (bool const is_last = parent_state.child_index == parent_state.child_count; !is_last)
        {
            data->out.push_back(',');
            jsonIndent(data);
        }
    }
}

void jsonPushParent(struct JsonWalk* data, tr_variant const& v)
{
    auto const is_dict = v.holds_alternative<tr_variant::Map>();
    auto const is_list = v.holds_alternative<tr_variant::Vector>();
    auto const n_children = variant_size(v) * (is_dict ? 2U : 1U);
    data->parents.push_back({ is_dict, is_list, 0, n_children });
}

void jsonPopParent(struct JsonWalk* data)
{
    data->parents.pop_back();
}

void jsonIntFunc(tr_variant const& /*var*/, int64_t const val, void* vdata)
{
    auto buf = std::array<char, 64>{};
    auto const* const out = fmt::format_to(std::data(buf), FMT_COMPILE("{:d}"), val);
    auto* const data = static_cast<JsonWalk*>(vdata);
    data->out.add(std::data(buf), static_cast<size_t>(out - std::data(buf)));
    jsonChildFunc(data);
}

void jsonBoolFunc(tr_variant const& /*var*/, bool const val, void* vdata)
{
    auto* data = static_cast<struct JsonWalk*>(vdata);
    data->out.add(val ? "true"sv : "false"sv);
    jsonChildFunc(data);
}

void jsonRealFunc(tr_variant const& /*var*/, double const val, void* vdata)
{
    auto* const data = static_cast<struct JsonWalk*>(vdata);

    auto const [buf, buflen] = data->out.reserve_space(64);
    auto* walk = reinterpret_cast<char*>(buf);
    auto const* const begin = walk;

    if (fabs(val - (int)val) < 0.00001)
    {
        walk = fmt::format_to(walk, FMT_COMPILE("{:.0f}"), val);
    }
    else
    {
        walk = fmt::format_to(walk, FMT_COMPILE("{:.4f}"), val);
    }

    data->out.commit_space(walk - begin);

    jsonChildFunc(data);
}

[[nodiscard]] char* write_escaped_char(char* buf, char const* const end, std::string_view& sv)
{
    auto u16buf = std::array<std::uint16_t, 2>{};

    auto const* const begin8 = std::data(sv);
    auto const* const end8 = begin8 + std::size(sv);
    auto const* walk8 = begin8;
    utf8::next(walk8, end8);
    auto const end16 = utf8::utf8to16(begin8, walk8, std::begin(u16buf));

    for (auto it = std::cbegin(u16buf); it != end16; ++it)
    {
        buf = fmt::format_to_n(buf, end - buf - 1, FMT_COMPILE("\\u{:04x}"), *it).out;
    }

    sv.remove_prefix(walk8 - begin8 - 1);
    return buf;
}

void jsonStringFunc(tr_variant const& /*var*/, std::string_view sv, void* vdata)
{
    auto* const data = static_cast<struct JsonWalk*>(vdata);

    auto& out = data->out;
    auto const [buf, buflen] = out.reserve_space(std::size(sv) * 6 + 2);
    auto* walk = reinterpret_cast<char*>(buf);
    auto const* const begin = walk;
    auto const* const end = begin + buflen;

    *walk++ = '"';

    for (; !std::empty(sv); sv.remove_prefix(1))
    {
        switch (sv.front())
        {
        case '\b':
            *walk++ = '\\';
            *walk++ = 'b';
            break;

        case '\f':
            *walk++ = '\\';
            *walk++ = 'f';
            break;

        case '\n':
            *walk++ = '\\';
            *walk++ = 'n';
            break;

        case '\r':
            *walk++ = '\\';
            *walk++ = 'r';
            break;

        case '\t':
            *walk++ = '\\';
            *walk++ = 't';
            break;

        case '"':
            *walk++ = '\\';
            *walk++ = '"';
            break;

        case '\\':
            *walk++ = '\\';
            *walk++ = '\\';
            break;

        default:
            if (isprint((unsigned char)sv.front()) != 0)
            {
                *walk++ = sv.front();
            }
            else
            {
                try
                {
                    walk = write_escaped_char(walk, end, sv);
                }
                catch (utf8::exception const&)
                {
                    *walk++ = '?';
                }
            }
            break;
        }
    }

    *walk++ = '"';
    TR_ASSERT(walk <= end);
    out.commit_space(walk - begin);

    jsonChildFunc(data);
}

void jsonDictBeginFunc(tr_variant const& var, void* vdata)
{
    auto* const data = static_cast<struct JsonWalk*>(vdata);

    jsonPushParent(data, var);
    data->out.push_back('{');

    if (variant_size(var) != 0U)
    {
        jsonIndent(data);
    }
}

void jsonListBeginFunc(tr_variant const& var, void* vdata)
{
    auto* const data = static_cast<struct JsonWalk*>(vdata);

    jsonPushParent(data, var);
    data->out.push_back('[');

    if (variant_size(var) != 0U)
    {
        jsonIndent(data);
    }
}

void jsonContainerEndFunc(tr_variant const& var, void* vdata)
{
    auto* const data = static_cast<struct JsonWalk*>(vdata);

    jsonPopParent(data);

    jsonIndent(data);

    if (var.holds_alternative<tr_variant::Map>())
    {
        data->out.push_back('}');
    }
    else /* list */
    {
        data->out.push_back(']');
    }

    jsonChildFunc(data);
}

} // namespace to_string_helpers
} // namespace

std::string tr_variant_serde::to_json_string(tr_variant const& var) const
{
    using namespace to_string_helpers;

    static auto constexpr Funcs = WalkFuncs{
        jsonIntFunc, //
        jsonBoolFunc, //
        jsonRealFunc, //
        jsonStringFunc, //
        jsonDictBeginFunc, //
        jsonListBeginFunc, //
        jsonContainerEndFunc, //
    };

    auto data = JsonWalk{ !compact_ };
    walk(var, Funcs, &data, true);

    auto& buf = data.out;
    if (!std::empty(buf))
    {
        buf.push_back('\n');
    }
    return buf.to_string();
}
