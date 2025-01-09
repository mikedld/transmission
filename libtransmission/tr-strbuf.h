// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include <fmt/core.h>

/**
 * A memory buffer which uses a builtin array of N bytes, using heap
 * memory only if its string gets too big. Its main use case is building
 * temporary strings in stack memory.
 *
 * It is a convenience wrapper around `fmt::basic_memory_buffer`.
 */
template<typename Char, size_t N>
class tr_strbuf
{
private:
    fmt::basic_memory_buffer<Char, N> buffer_;

public:
    using value_type = Char;
    using const_reference = Char const&;

    tr_strbuf()
    {
        ensure_sz();
    }

    tr_strbuf(tr_strbuf const& other)
    {
        if (this != &other)
        {
            assign(other.sv());
        }
    }

    tr_strbuf& operator=(tr_strbuf const& other)
    {
        if (this != &other)
        {
            assign(other.sv());
        }

        return *this;
    }

    tr_strbuf(tr_strbuf&& other)
        : buffer_{ std::move(other.buffer_) }
    {
        ensure_sz();
    }

    tr_strbuf& operator=(tr_strbuf&& other)
    {
        buffer_ = std::move(other.buffer_);
        ensure_sz();
        return *this;
    }

    template<typename... Args>
    explicit tr_strbuf(Args const&... args)
    {
        append(args...);
    }

    [[nodiscard]] constexpr auto begin() noexcept
    {
        return buffer_.begin();
    }

    [[nodiscard]] constexpr auto end() noexcept
    {
        return buffer_.end();
    }

    [[nodiscard]] constexpr auto begin() const noexcept
    {
        return buffer_.begin();
    }

    [[nodiscard]] constexpr auto end() const noexcept
    {
        return buffer_.end();
    }

    [[nodiscard]] constexpr Char& at(size_t pos) noexcept
    {
        return buffer_[pos];
    }

    [[nodiscard]] constexpr Char at(size_t pos) const noexcept
    {
        return buffer_[pos];
    }

    [[nodiscard]] constexpr auto size() const noexcept
    {
        return buffer_.size();
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return size() == 0;
    }

    [[nodiscard]] constexpr auto* data() noexcept
    {
        return buffer_.data();
    }

    [[nodiscard]] constexpr auto const* data() const noexcept
    {
        return buffer_.data();
    }

    [[nodiscard]] constexpr auto const* c_str() const noexcept
    {
        return data();
    }

    [[nodiscard]] constexpr auto sv() const noexcept
    {
        return std::basic_string_view<Char>{ data(), size() };
    }

    template<typename ContiguousRange>
    [[nodiscard]] constexpr bool operator==(ContiguousRange const& x) const noexcept
    {
        return sv() == x;
    }

    ///

    [[nodiscard]] constexpr bool ends_with(Char const& x) const noexcept
    {
        auto const n = size();
        return n != 0 && data()[n - 1] == x;
    }

    template<typename ContiguousRange>
    [[nodiscard]] constexpr bool ends_with(ContiguousRange const& x) const noexcept
    {
        auto const x_len = std::size(x);
        auto const len = size();
        return len >= x_len && this->sv().substr(len - x_len) == x;
    }

    [[nodiscard]] constexpr bool ends_with(Char const* x) const noexcept
    {
        return x != nullptr && ends_with(std::basic_string_view<Char>(x));
    }

    ///

    [[nodiscard]] constexpr bool starts_with(Char const& x) const noexcept
    {
        return !empty() && *data() == x;
    }

    template<typename ContiguousRange>
    [[nodiscard]] constexpr bool starts_with(ContiguousRange const& x) const noexcept
    {
        auto const x_len = std::size(x);
        return size() >= x_len && this->sv().substr(0, x_len) == x;
    }

    [[nodiscard]] constexpr bool starts_with(Char const* x) const noexcept
    {
        return x != nullptr && starts_with(std::basic_string_view<Char>(x));
    }

    ///

    void clear()
    {
        buffer_.clear();
        ensure_sz();
    }

    void resize(size_t n)
    {
        buffer_.resize(n);
        ensure_sz();
    }

    ///

    void append(Char const& value)
    {
        buffer_.push_back(value);
        ensure_sz();
    }

    template<typename ContiguousRange>
    void append(ContiguousRange const& args)
    {
        buffer_.append(std::data(args), std::data(args) + std::size(args));
        ensure_sz();
    }

    void append(Char const* sz_value)
    {
        if (sz_value != nullptr)
        {
            append(std::basic_string_view<Char>{ sz_value });
        }
    }

    template<typename... Args>
    void append(Args const&... args)
    {
        (append(args), ...);
    }

    template<typename Arg>
    auto& operator+=(Arg const& arg)
    {
        append(arg);
        return *this;
    }

    ///

    template<typename... Args>
    void assign(Args const&... args)
    {
        clear();
        append(args...);
    }

    template<typename Arg>
    auto& operator=(Arg const& arg)
    {
        assign(arg);
        return *this;
    }

    void push_back(Char const& value)
    {
        append(value);
    }

    ///

    template<typename... Args>
    void join(Char delim, Args const&... args)
    {
        ((append(args), append(delim)), ...);
        resize(size() - 1);
    }

    template<typename ContiguousRange, typename... Args>
    void join(ContiguousRange const& delim, Args const&... args)
    {
        ((append(args), append(delim)), ...);
        resize(size() - std::size(delim));
    }

    template<typename... Args>
    void join(Char const* sz_delim, Args const&... args)
    {
        join(std::basic_string_view<Char>{ sz_delim }, args...);
    }

    [[nodiscard]] constexpr operator std::basic_string_view<Char>() const noexcept
    {
        return sv();
    }

    [[nodiscard]] constexpr operator auto() const noexcept
    {
        return c_str();
    }

    bool popdir() noexcept
    {
        std::string_view tr_sys_path_dirname(std::string_view path);
        auto const parent = tr_sys_path_dirname(sv());
        auto const changed = parent != sv();

        if (changed)
        {
            if (std::data(parent) == std::data(*this))
            {
                resize(std::size(parent));
            }
            else
            {
                assign(parent);
            }
        }

        return changed;
    }

private:
    /**
     * Ensure that the buffer's string is zero-terminated, e.g. for
     * external APIs that require `char*` strings.
     *
     * Note that the added trailing '\0' does not increment `size()`.
     * This is to ensure that `strlen(buf.c_str()) == buf.size()`.
     */
    void ensure_sz()
    {
        auto const n = size();
        buffer_.reserve(n + 1);
        buffer_[n] = '\0';
    }
};

template<typename Char, size_t N>
struct fmt::formatter<tr_strbuf<Char, N>> : formatter<std::basic_string_view<Char>, Char>
{
    template<typename FormatContext>
    constexpr auto format(tr_strbuf<Char, N> const& strbuf, FormatContext& ctx) const
    {
        return formatter<std::basic_string_view<Char>, Char>::format(strbuf.sv(), ctx);
    }
};

/**
 * Good for building short-term URLs.
 * The initial size is big enough to avoid heap allocs in most cases,
 * but that also makes it a poor choice for longer-term storage.
 * https://stackoverflow.com/a/417184
 */
using tr_urlbuf = tr_strbuf<char, 2000>;

/**
 * Good for building short-term filenames.
 * The initial size is big enough to avoid heap allocs in most cases,
 * but that also makes it a poor choice for longer-term storage.
 * https://stackoverflow.com/a/65174437
 */
using tr_pathbuf = tr_strbuf<char, 4096>;
