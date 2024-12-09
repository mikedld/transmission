// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno> /* EILSEQ, EINVAL */
#include <cstddef> // std::byte
#include <cstdint> // uint16_t
#include <optional>
#include <stack>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <fmt/core.h>

#include <rapidjson/encodedstream.h>
#include <rapidjson/encodings.h>
#include <rapidjson/error/en.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/reader.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#define LIBTRANSMISSION_VARIANT_MODULE

#include "libtransmission/error.h"
#include "libtransmission/quark.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"

namespace
{
namespace parse_helpers
{
struct json_to_variant_handler : public rapidjson::BaseReaderHandler<>
{
    static_assert(std::is_same_v<Ch, char>);

    explicit json_to_variant_handler(tr_variant* const top)
    {
        stack_.emplace(top);
    }

    bool Null()
    {
        *get_leaf() = nullptr;
        return true;
    }

    bool Bool(bool const val)
    {
        *get_leaf() = val;
        return true;
    }

    bool Int(int const val)
    {
        return Int64(val);
    }

    bool Uint(unsigned const val)
    {
        return Uint64(val);
    }

    bool Int64(int64_t const val)
    {
        *get_leaf() = val;
        return true;
    }

    bool Uint64(uint64_t const val)
    {
        return Int64(val);
    }

    bool Double(double const val)
    {
        *get_leaf() = val;
        return true;
    }

    bool String(Ch const* const str, rapidjson::SizeType const len, bool const copy)
    {
        *get_leaf() = copy ? tr_variant{ std::string_view{ str, len } } : tr_variant::unmanaged_string({ str, len });
        return true;
    }

    bool StartObject()
    {
        tr_variantInitDict(push_stack(), prealloc_guess());
        return true;
    }

    bool Key(Ch const* const str, rapidjson::SizeType const len, bool const copy)
    {
        if (copy)
        {
            key_buf_.assign(str, len);
            cur_key_ = key_buf_;
        }
        else
        {
            cur_key_ = std::string_view{ str, len };
        }
        return true;
    }

    bool EndObject(rapidjson::SizeType const len)
    {
        pop_stack(len);
        return true;
    }

    bool StartArray()
    {
        tr_variantInitList(push_stack(), prealloc_guess());
        return true;
    }

    bool EndArray(rapidjson::SizeType const len)
    {
        pop_stack(len);
        return true;
    }

private:
    [[nodiscard]] size_t prealloc_guess() const noexcept
    {
        auto const depth = std::size(stack_);
        return depth < MaxDepth ? prealloc_guess_[depth] : 0;
    }

    tr_variant* push_stack() noexcept
    {
        return stack_.emplace(get_leaf());
    }

    void pop_stack(rapidjson::SizeType const len) noexcept
    {
#ifdef TR_ENABLE_ASSERTS
        if (auto* top = stack_.top(); top->holds_alternative<tr_variant::Vector>())
        {
            TR_ASSERT(std::size(*top->get_if<tr_variant::Vector>()) == len);
        }
        else if (top->holds_alternative<tr_variant::Map>())
        {
            TR_ASSERT(std::size(*top->get_if<tr_variant::Map>()) == len);
        }
#endif

        auto const depth = std::size(stack_);
        stack_.pop();
        TR_ASSERT(!std::empty(stack_));
        if (depth < MaxDepth)
        {
            prealloc_guess_[depth] = len;
        }
    }

    tr_variant* get_leaf()
    {
        auto* const parent = stack_.top();
        TR_ASSERT(parent != nullptr);

        if (parent->holds_alternative<tr_variant::Vector>())
        {
            return tr_variantListAdd(parent);
        }
        if (parent->holds_alternative<tr_variant::Map>())
        {
            TR_ASSERT(!std::empty(cur_key_));
            auto tmp = std::string_view{};
            std::swap(cur_key_, tmp);
            return tr_variantDictAdd(parent, tr_quark_new(tmp));
        }

        return parent;
    }

    /* arbitrary value... this is much deeper than our code goes */
    static auto constexpr MaxDepth = size_t{ 64 };

    /* A very common pattern is for a container's children to be similar,
     * e.g. they may all be objects with the same set of keys. So when
     * a container is popped off the stack, remember its size to use as
     * a preallocation heuristic for the next container at that depth. */
    std::array<size_t, MaxDepth> prealloc_guess_{};

    std::string key_buf_;
    std::string_view cur_key_;
    std::stack<tr_variant*> stack_;
};
} // namespace parse_helpers
} // namespace

std::optional<tr_variant> tr_variant_serde::parse_json(std::string_view input)
{
    auto* begin = std::data(input);
    if (begin == nullptr)
    {
        // RapidJSON will dereference a nullptr otherwise
        begin = "";
    }

    auto const size = std::size(input);
    auto top = tr_variant{};
    auto handler = parse_helpers::json_to_variant_handler{ &top };
    auto ms = rapidjson::MemoryStream{ begin, size };
    auto eis = rapidjson::AutoUTFInputStream<unsigned, rapidjson::MemoryStream>{ ms };
    auto reader = rapidjson::GenericReader<rapidjson::AutoUTF<unsigned>, rapidjson::UTF8<char>>{};
    reader.Parse<rapidjson::kParseStopWhenDoneFlag>(eis, handler);

    // Due to the nature of how AutoUTFInputStream works, when AutoUTFInputStream
    // is used with MemoryStream, the read cursor position is always 1 ahead of
    // the current character (unless the end of stream is reached).
    auto const pos = eis.Peek() == '\0' ? eis.Tell() : eis.Tell() - 1U;
    end_ = begin + pos;

    if (!reader.HasParseError())
    {
        return std::optional<tr_variant>{ std::move(top) };
    }

    if (auto err_code = reader.GetParseErrorCode(); err_code == rapidjson::kParseErrorDocumentEmpty)
    {
        error_.set(EINVAL, "No content");
    }
    else
    {
        error_.set(
            EILSEQ,
            fmt::format(
                _("Couldn't parse JSON at position {position} '{text}': {error} ({error_code})"),
                fmt::arg("position", pos),
                fmt::arg("text", std::string_view{ begin + pos, std::min(size_t{ 16U }, size - pos) }),
                fmt::arg("error", rapidjson::GetParseError_En(err_code)),
                fmt::arg("error_code", static_cast<std::underlying_type_t<decltype(err_code)>>(err_code))));
    }

    return {};
}

// ---

namespace
{
namespace to_string_helpers
{
// implements RapidJSON's Stream concept, so that the library can output
// directly to a std::string, and we can avoid some copying by copy elision
// http://rapidjson.org/md_doc_stream.html
struct string_output_stream
{
    using Ch = char;

    explicit string_output_stream(std::string& str)
        : str_ref_{ str }
    {
    }

    [[nodiscard]] static Ch Peek()
    {
        TR_ASSERT(false);
        return 0;
    }

    [[nodiscard]] static Ch Take()
    {
        TR_ASSERT(false);
        return 0;
    }

    static size_t Tell()
    {
        TR_ASSERT(false);
        return 0U;
    }

    static Ch* PutBegin()
    {
        TR_ASSERT(false);
        return nullptr;
    }

    void Put(Ch const c)
    {
        str_ref_ += c;
    }

    static void Flush()
    {
    }

    static size_t PutEnd(Ch* /*begin*/)
    {
        TR_ASSERT(false);
        return 0U;
    }

private:
    std::string& str_ref_;
};

using writer_var_t = std::variant<rapidjson::Writer<string_output_stream>, rapidjson::PrettyWriter<string_output_stream>>;

void jsonNullFunc(tr_variant const& /*var*/, std::nullptr_t /*val*/, void* vdata)
{
    std::visit([](auto&& writer) { writer.Null(); }, *static_cast<writer_var_t*>(vdata));
}

void jsonIntFunc(tr_variant const& /*var*/, int64_t const val, void* vdata)
{
    std::visit([val](auto&& writer) { writer.Int64(val); }, *static_cast<writer_var_t*>(vdata));
}

void jsonBoolFunc(tr_variant const& /*var*/, bool const val, void* vdata)
{
    std::visit([val](auto&& writer) { writer.Bool(val); }, *static_cast<writer_var_t*>(vdata));
}

void jsonRealFunc(tr_variant const& /*var*/, double const val, void* vdata)
{
    std::visit([val](auto&& writer) { writer.Double(val); }, *static_cast<writer_var_t*>(vdata));
}

void jsonStringFunc(tr_variant const& /*var*/, std::string_view sv, void* vdata)
{
    std::visit([sv](auto&& writer) { writer.String(std::data(sv), std::size(sv)); }, *static_cast<writer_var_t*>(vdata));
}

void jsonDictBeginFunc(tr_variant const& /*var*/, void* vdata)
{
    std::visit([](auto&& writer) { writer.StartObject(); }, *static_cast<writer_var_t*>(vdata));
}

void jsonListBeginFunc(tr_variant const& /*var*/, void* vdata)
{
    std::visit([](auto&& writer) { writer.StartArray(); }, *static_cast<writer_var_t*>(vdata));
}

void jsonContainerEndFunc(tr_variant const& var, void* vdata)
{
    auto& writer_var = *static_cast<writer_var_t*>(vdata);

    if (var.holds_alternative<tr_variant::Map>())
    {
        std::visit([](auto&& writer) { writer.EndObject(); }, writer_var);
    }
    else /* list */
    {
        std::visit([](auto&& writer) { writer.EndArray(); }, writer_var);
    }
}

} // namespace to_string_helpers
} // namespace

std::string tr_variant_serde::to_json_string(tr_variant const& var) const
{
    using namespace to_string_helpers;

    static auto constexpr Funcs = WalkFuncs{
        jsonNullFunc, //
        jsonIntFunc, //
        jsonBoolFunc, //
        jsonRealFunc, //
        jsonStringFunc, //
        jsonDictBeginFunc, //
        jsonListBeginFunc, //
        jsonContainerEndFunc, //
    };

    auto out = std::string{};
    out.reserve(rapidjson::StringBuffer::kDefaultCapacity);
    auto stream = string_output_stream{ out };
    auto writer = writer_var_t{};
    if (compact_)
    {
        writer.emplace<0>(stream);
    }
    else
    {
        writer.emplace<1>(stream);
    }
    walk(var, Funcs, &writer, true);

    return out;
}
