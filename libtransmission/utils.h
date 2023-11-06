// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cctype>
#include <cstdint> // uint8_t, uint32_t, uint64_t
#include <cstddef> // size_t
#include <ctime> // time_t
#include <locale>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "libtransmission/tr-macros.h"
#include "libtransmission/variant.h"

struct tr_error;
struct tr_variant;

/**
 * @addtogroup utils Utilities
 * @{
 */

#if !defined(_)
#if defined(HAVE_GETTEXT) && !defined(__APPLE__)
#include <libintl.h>
#define _(a) gettext(a)
#else
#define _(a) (a)
#endif
#endif

#if defined(HAVE_NGETTEXT)
#define tr_ngettext ngettext
#else
#define tr_ngettext(singular, plural, count) ((count) == 1 ? (singular) : (plural))
#endif

/* #define DISABLE_GETTEXT */
#ifndef DISABLE_GETTEXT
#if defined(_WIN32)
#define DISABLE_GETTEXT
#endif
#endif
#ifdef DISABLE_GETTEXT
#undef _
#undef tr_ngettext
#define _(a) (a)
#define tr_ngettext(singular, plural, count) ((count) == 1 ? (singular) : (plural))
#endif

std::optional<std::locale> tr_locale_set_global(char const* locale_name) noexcept;

std::optional<std::locale> tr_locale_set_global(std::locale const& locale) noexcept;

// ---

[[nodiscard]] std::string_view tr_get_mime_type_for_filename(std::string_view filename);

bool tr_file_read(std::string_view filename, std::vector<char>& contents, tr_error* error = nullptr);

bool tr_file_move(std::string_view oldpath, std::string_view newpath, tr_error* error = nullptr);

bool tr_file_save(std::string_view filename, std::string_view contents, tr_error* error = nullptr);

template<typename ContiguousRange>
constexpr auto tr_file_save(std::string_view filename, ContiguousRange const& x, tr_error* error = nullptr)
{
    return tr_file_save(filename, std::string_view{ std::data(x), std::size(x) }, error);
}

/** @brief return the current date in milliseconds */
[[nodiscard]] uint64_t tr_time_msec();

#ifdef _WIN32

[[nodiscard]] std::string tr_win32_format_message(uint32_t code);
[[nodiscard]] std::string tr_win32_native_to_utf8(std::wstring_view);
[[nodiscard]] std::wstring tr_win32_utf8_to_native(std::string_view);

int tr_main_win32(int argc, char** argv, int (*real_main)(int, char**));

#define tr_main(...) \
    main_impl(__VA_ARGS__); \
    int main(int argc, char* argv[]) \
    { \
        return tr_main_win32(argc, argv, &main_impl); \
    } \
    int main_impl(__VA_ARGS__)

#else

#define tr_main main

#endif

// ---

[[nodiscard]] constexpr bool tr_str_is_empty(char const* value)
{
    return value == nullptr || *value == '\0';
}

/** @brief Portability wrapper for `strlcpy()` that uses the system implementation if available */
size_t tr_strlcpy(void* dst, void const* src, size_t siz);

/** @brief Convenience wrapper around `strerorr()` guaranteed to not return nullptr
    @param errnum the error number to describe */
[[nodiscard]] char const* tr_strerror(int errnum);

template<typename T>
[[nodiscard]] std::string tr_strlower(T in)
{
    auto out = std::string{ std::move(in) };
    std::for_each(std::begin(out), std::end(out), [](char& ch) { ch = std::tolower(ch); });
    return out;
}

template<typename T>
[[nodiscard]] std::string tr_strupper(T in)
{
    auto out = std::string{ std::move(in) };
    std::for_each(std::begin(out), std::end(out), [](char& ch) { ch = std::toupper(ch); });
    return out;
}

// --- std::string_view utils

/**
 * @brief Rich Salz's classic implementation of shell-style pattern matching for `?`, `\`, `[]`, and `*` characters.
 * @return 1 if the pattern matches, 0 if it doesn't, or -1 if an error occurred
 */
[[nodiscard]] bool tr_wildmat(std::string_view text, std::string_view pattern);

template<typename T>
[[nodiscard]] constexpr bool tr_strv_contains(std::string_view sv, T key) noexcept // c++23
{
    return sv.find(key) != std::string_view::npos;
}

[[nodiscard]] constexpr bool tr_strv_starts_with(std::string_view sv, char key) // c++20
{
    return !std::empty(sv) && sv.front() == key;
}

[[nodiscard]] constexpr bool tr_strv_starts_with(std::string_view sv, std::string_view key) // c++20
{
    return std::size(key) <= std::size(sv) && sv.substr(0, std::size(key)) == key;
}

[[nodiscard]] constexpr bool tr_strv_starts_with(std::wstring_view sv, std::wstring_view key) // c++20
{
    return std::size(key) <= std::size(sv) && sv.substr(0, std::size(key)) == key;
}

[[nodiscard]] constexpr bool tr_strv_ends_with(std::string_view sv, std::string_view key) // c++20
{
    return std::size(key) <= std::size(sv) && sv.substr(std::size(sv) - std::size(key)) == key;
}

[[nodiscard]] constexpr bool tr_strv_ends_with(std::string_view sv, char key) // c++20
{
    return !std::empty(sv) && sv.back() == key;
}

template<typename T>
[[nodiscard]] constexpr int tr_compare_3way(T const& left, T const& right)
{
    if (left < right)
    {
        return -1;
    }

    if (right < left)
    {
        return 1;
    }

    return 0;
}

constexpr std::string_view tr_strv_sep(std::string_view* sv, char delim)
{
    auto pos = sv->find(delim);
    auto const ret = sv->substr(0, pos);
    sv->remove_prefix(pos != std::string_view::npos ? pos + 1 : std::size(*sv));
    return ret;
}

constexpr bool tr_strv_sep(std::string_view* sv, std::string_view* token, char delim)
{
    if (std::empty(*sv))
    {
        return false;
    }

    *token = tr_strv_sep(sv, delim);
    return true;
}

[[nodiscard]] std::string_view tr_strv_strip(std::string_view str);

[[nodiscard]] std::string tr_strv_convert_utf8(std::string_view sv);

[[nodiscard]] std::string tr_strv_replace_invalid(std::string_view sv, uint32_t replacement = 0xFFFD /*�*/);

/**
 * @brief copies `src` into `buf`.
 *
 * - Always returns std::size(src).
 * - `src` will be copied into `buf` iff `buflen >= std::size(src)`
 * - `buf` will also be zero terminated iff `buflen >= std::size(src) + 1`.
 */
size_t tr_strv_to_buf(std::string_view src, char* buf, size_t buflen);

// ---

template<typename T, std::enable_if_t<std::is_integral<T>::value, bool> = true>
[[nodiscard]] std::optional<T> tr_num_parse(std::string_view str, std::string_view* setme_remainder = nullptr, int base = 10);

template<typename T, std::enable_if_t<std::is_floating_point<T>::value, bool> = true>
[[nodiscard]] std::optional<T> tr_num_parse(std::string_view str, std::string_view* setme_remainder = nullptr);

/**
 * @brief Given a string like "1-4" or "1-4,6,9,14-51", this returns a
 *        newly-allocated array of all the integers in the set.
 * @return a vector of integers, which is empty if the string can't be parsed.
 *
 * For example, "5-8" will return [ 5, 6, 7, 8 ] and setmeCount will be 4.
 */
[[nodiscard]] std::vector<int> tr_num_parse_range(std::string_view str);

/**
 * @brief truncate a double value at a given number of decimal places.
 *
 * this can be used to prevent a `printf()` call from rounding up:
 * call with the `decimal_places` argument equal to the number of
 * decimal places in the `printf()`'s precision:
 *
 * - printf("%.2f%%", 99.999) ==> "100.00%"
 *
 * - printf("%.2f%%", tr_truncd(99.999, 2)) ==> "99.99%"
 *             ^                        ^
 *             |   These should match   |
 *             +------------------------+
 */
[[nodiscard]] double tr_truncd(double x, int decimal_places);

/* return a percent formatted string of either x.xx, xx.x or xxx */
[[nodiscard]] std::string tr_strpercent(double x);

/** @brief return `TR_RATIO_NA`, `TR_RATIO_INF`, or a number in [0..1]
    @return `TR_RATIO_NA`, `TR_RATIO_INF`, or a number in [0..1] */
[[nodiscard]] double tr_getRatio(uint64_t numerator, uint64_t denominator);

/** @param ratio    the ratio to convert to a string
    @param infinity the string representation of "infinity" */
[[nodiscard]] std::string tr_strratio(double ratio, char const* infinity);

// ---

namespace libtransmission::detail::tr_time
{
extern time_t current_time;
}

/**
 * @brief very inexpensive form of time(nullptr)
 * @return the current epoch time in seconds
 *
 * This function returns a second counter that is updated once per second.
 * If something blocks the libtransmission thread for more than a second,
 * that counter may be thrown off, so this function is not guaranteed
 * to always be accurate. However, it is *much* faster when 100% accuracy
 * isn't needed
 */
[[nodiscard]] static inline time_t tr_time() noexcept
{
    return libtransmission::detail::tr_time::current_time;
}

/** @brief Private libtransmission function to update `tr_time()`'s counter */
constexpr void tr_timeUpdate(time_t now) noexcept
{
    libtransmission::detail::tr_time::current_time = now;
}

/** @brief Portability wrapper for `htonll()` that uses the system implementation if available */
[[nodiscard]] uint64_t tr_htonll(uint64_t hostlonglong);

/** @brief Portability wrapper for `ntohll()` that uses the system implementation if available */
[[nodiscard]] uint64_t tr_ntohll(uint64_t netlonglong);

// ---

/* example: tr_formatter_size_init(1024, _("KiB"), _("MiB"), _("GiB"), _("TiB")); */

void tr_formatter_size_init(uint64_t kilo, char const* kb, char const* mb, char const* gb, char const* tb);

void tr_formatter_speed_init(size_t kilo, char const* kb, char const* mb, char const* gb, char const* tb);

void tr_formatter_mem_init(size_t kilo, char const* kb, char const* mb, char const* gb, char const* tb);

extern size_t tr_speed_K;
extern size_t tr_mem_K;
extern uint64_t tr_size_K; /* unused? */

/** @brief Format a speed from KBps into a user-readable string of at most 4 significant digits. */
[[nodiscard]] std::string tr_formatter_speed_KBps(double kilo_per_second);
/** @brief Format a speed from KBps into a user-readable string of at most 3 significant digits. */
[[nodiscard]] std::string tr_formatter_speed_compact_KBps(double kilo_per_second);

/** @brief Format a memory size from bytes into a user-readable string. */
[[nodiscard]] std::string tr_formatter_mem_B(size_t bytes);

/** @brief Format a memory size from MB into a user-readable string. */
[[nodiscard]] static inline std::string tr_formatter_mem_MB(double MBps)
{
    return tr_formatter_mem_B((size_t)(MBps * tr_mem_K * tr_mem_K));
}

/** @brief Format a file size from bytes into a user-readable string. */
[[nodiscard]] std::string tr_formatter_size_B(uint64_t bytes);

[[nodiscard]] tr_variant tr_formatter_get_units();

[[nodiscard]] static inline size_t tr_toSpeedBytes(size_t KBps)
{
    return KBps * tr_speed_K;
}

[[nodiscard]] static inline auto tr_toSpeedKBps(size_t Bps)
{
    return Bps / double(tr_speed_K);
}

[[nodiscard]] static inline auto tr_toMemBytes(size_t MB)
{
    return uint64_t(tr_mem_K) * tr_mem_K * MB;
}

[[nodiscard]] static inline auto tr_toMemMB(uint64_t B)
{
    return size_t(B / (tr_mem_K * tr_mem_K));
}

// ---

/** @brief Check if environment variable exists. */
[[nodiscard]] bool tr_env_key_exists(char const* key);

/** @brief Get environment variable value as string. */
[[nodiscard]] std::string tr_env_get_string(std::string_view key, std::string_view default_value = {});

// ---

class tr_net_init_mgr
{
private:
    tr_net_init_mgr();
    TR_DISABLE_COPY_MOVE(tr_net_init_mgr)

public:
    ~tr_net_init_mgr();
    static std::unique_ptr<tr_net_init_mgr> create();

private:
    static bool initialised;
};

/** @brief Initialise libtransmission for each app
 *  @return A manager object to be kept in scope of main() */
std::unique_ptr<tr_net_init_mgr> tr_lib_init();
