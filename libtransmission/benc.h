// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <array>
#include <cerrno>
#include <cstddef> // size_t
#include <cstdint> // int64_t
#include <optional>
#include <string>
#include <string_view>
#include <utility> // make_pair

#include "libtransmission/error.h"

namespace transmission::benc
{

namespace impl
{

std::optional<int64_t> ParseInt(std::string_view* benc);

std::optional<std::string_view> ParseString(std::string_view* benc);

} // namespace impl

struct Handler
{
    class Context
    {
    public:
        constexpr Context(char const* stream_begin_in, tr_error& error_in)
            : error{ error_in }
            , stream_begin_{ stream_begin_in }
        {
        }

        [[nodiscard]] constexpr std::pair<long, long> tokenSpan() const
        {
            return { token_begin_ - stream_begin_, token_end_ - stream_begin_ };
        }

        [[nodiscard]] constexpr auto raw() const
        {
            return std::string_view{ token_begin_, size_t(token_end_ - token_begin_) };
        }

        constexpr void setTokenSpan(char const* a, size_t len)
        {
            token_begin_ = a;
            token_end_ = token_begin_ + len;
        }

        tr_error& error;

    private:
        char const* token_begin_ = nullptr;
        char const* token_end_ = nullptr;
        char const* const stream_begin_;
    };

    virtual ~Handler() = default;

    virtual bool Int64(int64_t, Context const& context) = 0;
    virtual bool String(std::string_view, Context const& context) = 0;

    virtual bool StartDict(Context const& context) = 0;
    virtual bool Key(std::string_view, Context const& context) = 0;
    virtual bool EndDict(Context const& context) = 0;

    virtual bool StartArray(Context const& context) = 0;
    virtual bool EndArray(Context const& context) = 0;
};

template<std::size_t MaxDepth>
struct BasicHandler : public Handler
{
    bool Int64(int64_t /*value*/, Context const& /*context*/) override
    {
        return true;
    }

    bool String(std::string_view /*value*/, Context const& /*context*/) override
    {
        return true;
    }

    bool StartDict(Context const& /*context*/) override
    {
        push();
        return true;
    }

    bool Key(std::string_view key, Context const& /*context*/) override
    {
        keys_[depth_] = key;
        return true;
    }

    bool EndDict(Context const& /*context*/) override
    {
        pop();
        return true;
    }

    bool StartArray(Context const& /*context*/) override
    {
        push();
        return true;
    }

    bool EndArray(Context const& /*context*/) override
    {
        pop();
        return true;
    }

    [[nodiscard]] constexpr auto key(size_t i) const
    {
        return keys_[i];
    }

    [[nodiscard]] constexpr auto depth() const
    {
        return depth_;
    }

    [[nodiscard]] constexpr auto currentKey() const
    {
        return key(depth());
    }

    static auto constexpr ArrayKey = std::nullopt;

protected:
    [[nodiscard]] std::string path() const
    {
        auto ret = std::string{};
        for (size_t i = 0; i <= depth(); ++i)
        {
            ret += '[';
            ret += key(i).value_or(std::string_view{});
            ret += ']';
        }
        return ret;
    }

    template<typename... Args>
    [[nodiscard]] bool pathStartsWith(Args... args) const noexcept
    {
        auto i = 1U;
        return (depth() >= sizeof...(args)) && ((key(i++) == args) && ...);
    }

    template<typename... Args>
    [[nodiscard]] bool pathIs(Args... args) const noexcept
    {
        auto i = 1U;
        return (depth() == sizeof...(args)) && ((key(i++) == args) && ...);
    }

private:
    constexpr void push() noexcept
    {
        ++depth_;
        keys_[depth_] = {};
    }

    constexpr void pop() noexcept
    {
        --depth_;
    }

    size_t depth_ = 0;
    std::array<std::optional<std::string_view>, MaxDepth> keys_;
};

template<std::size_t MaxDepth>
struct ParserStack
{
    enum class ParentType
    {
        Array,
        Dict
    };
    struct Node
    {
        ParentType parent_type;
        size_t n_children_walked;
    };
    std::array<Node, MaxDepth> stack;
    std::size_t depth = 0;

    constexpr void clear() noexcept
    {
        depth = 0;
    }

    [[nodiscard]] constexpr auto empty() const noexcept
    {
        return depth == 0;
    }

    constexpr void tokenWalked()
    {
        ++stack[depth].n_children_walked;
    }

    [[nodiscard]] constexpr Node& current()
    {
        return stack[depth];
    }

    [[nodiscard]] constexpr Node& current() const
    {
        return stack[depth];
    }

    [[nodiscard]] constexpr bool expectingDictKey() const
    {
        return depth > 0 && stack[depth].parent_type == ParentType::Dict && (stack[depth].n_children_walked % 2) == 0;
    }

    [[nodiscard]] constexpr std::optional<ParentType> parentType() const
    {
        if (depth == 0)
        {
            return {};
        }

        return stack[depth].parent_type;
    }

    std::optional<ParentType> pop(tr_error& error)
    {
        if (depth == 0)
        {
            error.set(EILSEQ, "Cannot pop empty stack");
            return {};
        }

        if (stack[depth].parent_type == ParentType::Dict && ((stack[depth].n_children_walked % 2) != 0))
        {
            error.set(EILSEQ, "Premature end-of-dict found. Malformed benc?");
            return {};
        }

        auto const ret = stack[depth].parent_type;
        --depth;
        return ret;
    }

    bool push(ParentType parent_type, tr_error& error)
    {
        if (depth + 1 >= std::size(stack))
        {
            error.set(E2BIG, "Max stack depth reached; unable to continue parsing");
            return false;
        }

        ++depth;
        current() = { parent_type, 0 };
        return true;
    }
};

template<size_t MaxDepth>
bool parse(
    std::string_view benc,
    ParserStack<MaxDepth>& stack,
    Handler& handler,
    char const** setme_end = nullptr,
    tr_error* error = nullptr)
{
    // ensure `error` isn't a nullptr
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    stack.clear();
    auto const* const stream_begin = std::data(benc);
    auto context = Handler::Context{ stream_begin, *error };

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

        auto const* const front = std::data(benc);
        switch (benc.front())
        {
        case 'i': // int
            if (auto const value = impl::ParseInt(&benc); !value)
            {
                err = EILSEQ;
                error->set(err, "Malformed benc? Unable to parse integer");
            }
            else
            {
                context.setTokenSpan(front, std::data(benc) - front);

                if (!handler.Int64(*value, context))
                {
                    err = ECANCELED;
                }
                else
                {
                    stack.tokenWalked();
                }
            }
            break;

        case 'l': // list
        case 'd': // dict
            {
                bool ok = benc.front() == 'l' ? stack.push(ParserStack<MaxDepth>::ParentType::Array, *error) :
                                                stack.push(ParserStack<MaxDepth>::ParentType::Dict, *error);
                if (!ok)
                {
                    err = EILSEQ;
                    break;
                }

                context.setTokenSpan(front, 1);
                ok = benc.front() == 'l' ? handler.StartArray(context) : handler.StartDict(context);
                if (!ok)
                {
                    err = ECANCELED;
                    break;
                }

                benc.remove_prefix(1);
                break;
            }
        case 'e': // end of list or dict
            benc.remove_prefix(1);

            if (auto const parent_type = stack.pop(*error); !parent_type)
            {
                err = EILSEQ;
            }
            else
            {
                stack.tokenWalked();
                context.setTokenSpan(front, 1);

                if (auto const ok = *parent_type == ParserStack<MaxDepth>::ParentType::Array ? handler.EndArray(context) :
                                                                                               handler.EndDict(context);
                    !ok)
                {
                    err = ECANCELED;
                }
            }
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
        case '9': // string
            if (auto const sv = impl::ParseString(&benc); !sv)
            {
                err = EILSEQ;
                error->set(err, "Malformed benc? Unable to parse string");
            }
            else
            {
                context.setTokenSpan(front, std::data(benc) - front);
                if (bool const ok = stack.expectingDictKey() ? handler.Key(*sv, context) : handler.String(*sv, context); !ok)
                {
                    err = ECANCELED;
                }
                else
                {
                    stack.tokenWalked();
                }
            }
            break;

        default: // invalid bencoded text... march past it
            benc.remove_prefix(1);
            break;
        }

        if (stack.depth == 0)
        {
            break;
        }
    }

    if (setme_end != nullptr)
    {
        *setme_end = std::data(benc);
    }

    if (err != 0)
    {
        errno = err;
        return false;
    }

    if (stack.depth != 0)
    {
        err = EILSEQ;
        error->set(err, "premature end-of-data reached");
        errno = err;
        return false;
    }

    if (stack.stack[0].n_children_walked == 0)
    {
        err = EILSEQ;
        error->set(err, "no bencoded data to parse");
        errno = err;
        return false;
    }

    return true;
}

} // namespace transmission::benc
