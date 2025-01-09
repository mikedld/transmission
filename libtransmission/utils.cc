// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // for std::sort, std::transform
#include <array> // std::array
#include <cctype>
#include <cfloat> // DBL_DIG
#include <chrono>
#include <cstdint> // SIZE_MAX
#include <cstdlib> // getenv()
#include <cstring> /* strerror() */
#include <ctime>
#include <exception>
#include <iostream>
#include <iterator> // for std::back_inserter
#include <locale>
#include <memory>
#include <optional>
#include <stdexcept> // std::runtime_error
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h> /* Sleep(), GetEnvironmentVariable() */
#include <ws2tcpip.h> /* htonl(), ntohl() */

#include <shellapi.h> /* CommandLineToArgv() */
#else
#include <arpa/inet.h>
#include <sys/stat.h> /* umask() */
#endif

#define UTF_CPP_CPLUSPLUS 201703L
#include <utf8.h>

#include <curl/curl.h>

#include <fmt/core.h>

#include <fast_float/fast_float.h>
#include <wildmat.h>

#include "libtransmission/transmission.h"

#include "libtransmission/error-types.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/mime-types.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"
#include "libtransmission/values.h"

using namespace std::literals;
using namespace libtransmission::Values;

time_t libtransmission::detail::tr_time::current_time = {};

// ---

namespace libtransmission::Values
{

// default values; can be overridden by client apps
Config::Units<MemoryUnits> Config::Memory{ Config::Base::Kibi, "B"sv, "KiB"sv, "MiB"sv, "GiB"sv, "TiB"sv };
Config::Units<SpeedUnits> Config::Speed{ Config::Base::Kilo, "B/s"sv, "kB/s"sv, "MB/s"sv, "GB/s"sv, "TB/s"sv };
Config::Units<StorageUnits> Config::Storage{ Config::Base::Kilo, "B"sv, "kB"sv, "MB"sv, "GB"sv, "TB"sv };

} // namespace libtransmission::Values

// ---

#if defined(_WIN32) && defined(__clang_analyzer__)
// See https://github.com/llvm/llvm-project/issues/44701
#define WORKAROUND_CLANG_TIDY_GH44701
#endif

std::optional<std::locale> tr_locale_set_global(char const* locale_name) noexcept
{
#ifndef WORKAROUND_CLANG_TIDY_GH44701
    try
#endif
    {
        return tr_locale_set_global(std::locale{ locale_name });
    }
#ifndef WORKAROUND_CLANG_TIDY_GH44701
    catch (std::runtime_error const&)
    {
        return {};
    }
#endif
}

std::optional<std::locale> tr_locale_set_global(std::locale const& locale) noexcept
{
#ifndef WORKAROUND_CLANG_TIDY_GH44701
    try
#endif
    {
        auto old_locale = std::locale::global(locale);

        std::cout.imbue(std::locale{});
        std::cerr.imbue(std::locale{});

        return old_locale;
    }
#ifndef WORKAROUND_CLANG_TIDY_GH44701
    catch (std::exception const&)
    {
        return {};
    }
#endif
}

// ---

bool tr_file_read(std::string_view filename, std::vector<char>& contents, tr_error* error)
{
    auto const szfilename = tr_pathbuf{ filename };

    /* try to stat the file */
    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    auto const info = tr_sys_path_get_info(szfilename, 0, error);
    if (*error)
    {
        tr_logAddError(fmt::format(
            fmt::runtime(_("Couldn't read '{path}': {error} ({error_code})")),
            fmt::arg("path", filename),
            fmt::arg("error", error->message()),
            fmt::arg("error_code", error->code())));
        return false;
    }

    if (!info || !info->isFile())
    {
        tr_logAddError(fmt::format(fmt::runtime(_("Couldn't read '{path}': Not a regular file")), fmt::arg("path", filename)));
        error->set(TR_ERROR_EISDIR, "Not a regular file"sv);
        return false;
    }

    /* Load the torrent file into our buffer */
    auto const fd = tr_sys_file_open(szfilename, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
    if (fd == TR_BAD_SYS_FILE)
    {
        tr_logAddError(fmt::format(
            fmt::runtime(_("Couldn't read '{path}': {error} ({error_code})")),
            fmt::arg("path", filename),
            fmt::arg("error", error->message()),
            fmt::arg("error_code", error->code())));
        return false;
    }

    contents.resize(info->size);
    if (!tr_sys_file_read(fd, std::data(contents), info->size, nullptr, error))
    {
        tr_logAddError(fmt::format(
            fmt::runtime(_("Couldn't read '{path}': {error} ({error_code})")),
            fmt::arg("path", filename),
            fmt::arg("error", error->message()),
            fmt::arg("error_code", error->code())));
        tr_sys_file_close(fd);
        return false;
    }

    tr_sys_file_close(fd);
    return true;
}

bool tr_file_save(std::string_view filename, std::string_view contents, tr_error* error)
{
    // follow symlinks to find the "real" file, to make sure the temporary
    // we build with tr_sys_file_open_temp() is created on the right partition
    if (auto const realname = tr_sys_path_resolve(filename); !std::empty(realname) && realname != filename)
    {
        return tr_file_save(realname, contents, error);
    }

    // Write it to a temp file first.
    // This is a safeguard against edge cases, e.g. disk full, crash while writing, etc.
    auto tmp = tr_pathbuf{ filename, ".tmp.XXXXXX"sv };
    auto const fd = tr_sys_file_open_temp(std::data(tmp), error);
    if (fd == TR_BAD_SYS_FILE)
    {
        return false;
    }
#ifndef _WIN32
    // set file mode per settings umask()
    {
        auto const val = ::umask(0);
        ::umask(val);
        fchmod(fd, 0666 & ~val);
    }
#endif

    // Save the contents. This might take >1 pass.
    auto ok = true;
    while (!std::empty(contents))
    {
        auto n_written = uint64_t{};
        if (!tr_sys_file_write(fd, std::data(contents), std::size(contents), &n_written, error))
        {
            ok = false;
            break;
        }
        contents.remove_prefix(n_written);
    }

    // If we saved it to disk successfully, move it from '.tmp' to the correct filename
    if (!tr_sys_file_close(fd, error) || !ok || !tr_sys_path_rename(tmp, tr_pathbuf{ filename }, error))
    {
        return false;
    }

    tr_logAddTrace(fmt::format("Saved '{}'", filename));
    return true;
}

// ---

size_t tr_strv_to_buf(std::string_view src, char* buf, size_t buflen)
{
    size_t const len = std::size(src);

    if (buflen >= len)
    {
        auto const out = std::copy(std::begin(src), std::end(src), buf);

        if (buflen > len)
        {
            *out = '\0';
        }
    }

    return len;
}

/* User-level routine. returns whether or not 'text' and 'p' matched */
bool tr_wildmat(std::string_view text, std::string_view pattern)
{
    // TODO(ckerr): replace wildmat with base/strings/pattern.cc
    // wildmat wants these to be zero-terminated.
    return pattern == "*"sv || DoMatch(std::string{ text }.c_str(), std::string{ pattern }.c_str()) > 0;
}

char const* tr_strerror(int errnum)
{
    if (char const* const ret = strerror(errnum); ret != nullptr)
    {
        return ret;
    }

    return "Unknown Error";
}

// ---

std::string_view tr_strv_strip(std::string_view str)
{
    auto constexpr Test = [](auto ch)
    {
        return isspace(static_cast<unsigned char>(ch));
    };

    auto const it = std::find_if_not(std::begin(str), std::end(str), Test);
    str.remove_prefix(std::distance(std::begin(str), it));

    auto const rit = std::find_if_not(std::rbegin(str), std::rend(str), Test);
    str.remove_suffix(std::distance(std::rbegin(str), rit));

    return str;
}

// ---

uint64_t tr_time_msec()
{
    return std::chrono::system_clock::now().time_since_epoch() / 1ms;
}

// ---

double tr_getRatio(uint64_t numerator, uint64_t denominator)
{
    if (denominator > 0)
    {
        return numerator / static_cast<double>(denominator);
    }

    if (numerator > 0)
    {
        return TR_RATIO_INF;
    }

    return TR_RATIO_NA;
}

// ---

#if !(defined(__APPLE__) && defined(__clang__))

std::string tr_strv_convert_utf8(std::string_view sv)
{
    return tr_strv_replace_invalid(sv);
}

#endif

std::string tr_strv_replace_invalid(std::string_view sv, uint32_t replacement)
{
    // stripping characters after first \0
    if (auto first_null = sv.find('\0'); first_null != std::string::npos)
    {
        sv = { std::data(sv), first_null };
    }
    auto out = std::string{};
    out.reserve(std::size(sv));
    utf8::unchecked::replace_invalid(std::data(sv), std::data(sv) + std::size(sv), std::back_inserter(out), replacement);
    return out;
}

#ifdef _WIN32

std::string tr_win32_native_to_utf8(std::wstring_view in)
{
    auto out = std::string{};
    out.resize(WideCharToMultiByte(CP_UTF8, 0, std::data(in), std::size(in), nullptr, 0, nullptr, nullptr));
    [[maybe_unused]] auto
        len = WideCharToMultiByte(CP_UTF8, 0, std::data(in), std::size(in), std::data(out), std::size(out), nullptr, nullptr);
    TR_ASSERT(len == std::size(out));
    return out;
}

std::wstring tr_win32_utf8_to_native(std::string_view in)
{
    auto out = std::wstring{};
    out.resize(MultiByteToWideChar(CP_UTF8, 0, std::data(in), std::size(in), nullptr, 0));
    [[maybe_unused]] auto len = MultiByteToWideChar(CP_UTF8, 0, std::data(in), std::size(in), std::data(out), std::size(out));
    TR_ASSERT(len == std::size(out));
    return out;
}

std::string tr_win32_format_message(uint32_t code)
{
    wchar_t* wide_text = nullptr;
    auto const wide_size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<LPWSTR>(&wide_text),
        0,
        nullptr);

    if (wide_size == 0)
    {
        return fmt::format("Unknown error ({:#08x})", code);
    }

    auto text = std::string{};

    if (wide_size != 0 && wide_text != nullptr)
    {
        text = tr_win32_native_to_utf8({ wide_text, wide_size });
    }

    LocalFree(wide_text);

    // Most (all?) messages contain "\r\n" in the end, chop it
    while (!std::empty(text) && isspace(text.back()) != 0)
    {
        text.resize(text.size() - 1);
    }

    return text;
}

namespace
{
namespace tr_main_win32_impl
{

std::optional<std::vector<std::string>> win32MakeUtf8Argv()
{
    int argc = 0;
    auto argv = std::vector<std::string>{};
    if (wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc); wargv != nullptr)
    {
        for (int i = 0; i < argc; ++i)
        {
            if (wargv[i] == nullptr)
            {
                break;
            }

            auto str = tr_win32_native_to_utf8(wargv[i]);
            if (std::empty(str))
            {
                break;
            }

            argv.emplace_back(std::move(str));
        }

        LocalFree(reinterpret_cast<HLOCAL>(wargv));
    }

    if (static_cast<int>(std::size(argv)) == argc)
    {
        return argv;
    }

    return {};
}

} // namespace tr_main_win32_impl
} // namespace

int tr_main_win32(int argc, char** argv, int (*real_main)(int, char**))
{
    using namespace tr_main_win32_impl;

    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    // build an argv from GetCommandLineW + CommandLineToArgvW
    if (auto argv_strs = win32MakeUtf8Argv(); argv_strs)
    {
        auto argv_cstrs = std::vector<char*>{};
        argv_cstrs.reserve(std::size(*argv_strs));
        std::transform(
            std::begin(*argv_strs),
            std::end(*argv_strs),
            std::back_inserter(argv_cstrs),
            [](auto& str) { return std::data(str); });
        argv_cstrs.push_back(nullptr); // argv is nullptr-terminated
        return (*real_main)(std::size(*argv_strs), std::data(argv_cstrs));
    }

    return (*real_main)(argc, argv);
}

#endif

// ---

namespace
{
namespace tr_num_parse_range_impl
{

struct number_range
{
    int low;
    int high;
};

/**
 * This should be a single number (ex. "6") or a range (ex. "6-9").
 * Anything else is an error and will return failure.
 */
bool parseNumberSection(std::string_view str, number_range& range)
{
    auto constexpr Delimiter = "-"sv;

    auto const first = tr_num_parse<int>(str, &str);
    if (!first)
    {
        return false;
    }

    range.low = range.high = *first;
    if (std::empty(str))
    {
        return true;
    }

    if (!tr_strv_starts_with(str, Delimiter))
    {
        return false;
    }

    str.remove_prefix(std::size(Delimiter));
    auto const second = tr_num_parse<int>(str);
    if (!second)
    {
        return false;
    }

    range.high = *second;
    return true;
}

} // namespace tr_num_parse_range_impl
} // namespace

/**
 * Given a string like "1-4" or "1-4,6,9,14-51", this allocates and returns an
 * array of setmeCount ints of all the values in the array.
 * For example, "5-8" will return [ 5, 6, 7, 8 ] and setmeCount will be 4.
 * If a fragment of the string can't be parsed, nullptr is returned.
 */
std::vector<int> tr_num_parse_range(std::string_view str)
{
    using namespace tr_num_parse_range_impl;

    auto values = std::vector<int>{};
    auto token = std::string_view{};
    auto range = number_range{};
    while (tr_strv_sep(&str, &token, ',') && parseNumberSection(token, range))
    {
        for (auto i = range.low; i <= range.high; ++i)
        {
            values.emplace_back(i);
        }
    }

    std::sort(std::begin(values), std::end(values));
    values.erase(std::unique(std::begin(values), std::end(values)), std::end(values));
    return values;
}

// ---

double tr_truncd(double x, int decimal_places)
{
    auto buf = std::array<char, 128>{};
    auto const [out, len] = fmt::format_to_n(std::data(buf), std::size(buf) - 1, "{:.{}f}", x, DBL_DIG);
    *out = '\0';

    if (auto* const pt = strchr(std::data(buf), '.'); pt != nullptr)
    {
        pt[decimal_places != 0 ? decimal_places + 1 : 0] = '\0';
    }

    return tr_num_parse<double>(std::data(buf)).value_or(0.0);
}

std::string tr_strpercent(double x)
{
    if (x < 5.0)
    {
        return fmt::format("{:.2Lf}", tr_truncd(x, 2));
    }

    if (x < 100.0)
    {
        return fmt::format("{:.1Lf}", tr_truncd(x, 1));
    }

    return fmt::format("{:.0Lf}", x);
}

std::string tr_strratio(double ratio, std::string_view infinity)
{
    if ((int)ratio == TR_RATIO_NA)
    {
        return _("None");
    }

    if ((int)ratio == TR_RATIO_INF)
    {
        return std::string{ infinity };
    }

    return tr_strpercent(ratio);
}

// ---

bool tr_file_move(std::string_view oldpath_in, std::string_view newpath_in, bool allow_copy, tr_error* error)
{
    auto const oldpath = tr_pathbuf{ oldpath_in };
    auto const newpath = tr_pathbuf{ newpath_in };

    auto local_error = tr_error{};
    if (error == nullptr)
    {
        error = &local_error;
    }

    // make sure the old file exists
    auto const info = tr_sys_path_get_info(oldpath, 0, error);
    if (!info)
    {
        error->prefix_message("Unable to get information on old file: ");
        return false;
    }
    if (!info->isFile())
    {
        error->set(TR_ERROR_EINVAL, "Old path does not point to a file."sv);
        return false;
    }

    // ensure the target directory exists
    auto newdir = tr_pathbuf{ newpath };
    newdir.popdir();
    if (!tr_sys_dir_create(newdir, TR_SYS_DIR_CREATE_PARENTS, 0777, error))
    {
        error->prefix_message("Unable to create directory for new file: ");
        return false;
    }

    /* they might be on the same filesystem... */
    if (tr_sys_path_rename(oldpath, newpath, error))
    {
        return true;
    }

    if (!allow_copy)
    {
        error->prefix_message("Unable to move file: ");
        return false;
    }

    /* Otherwise, copy the file. */
    if (!tr_sys_path_copy(oldpath, newpath, error))
    {
        error->prefix_message("Unable to copy: ");
        return false;
    }

    if (auto log_error = tr_error{}; !tr_sys_path_remove(oldpath, &log_error))
    {
        tr_logAddError(fmt::format(
            fmt::runtime(_("Couldn't remove '{path}': {error} ({error_code})")),
            fmt::arg("path", oldpath),
            fmt::arg("error", log_error.message()),
            fmt::arg("error_code", log_error.code())));
    }

    return true;
}

// ---

uint64_t tr_htonll(uint64_t hostlonglong)
{
#ifdef HAVE_HTONLL

    return htonll(hostlonglong);

#else

    /* fallback code by bdonlan at https://stackoverflow.com/questions/809902/64-bit-ntohl-in-c/875505#875505 */
    union
    {
        std::array<uint32_t, 2> lx;
        uint64_t llx;
    } u = {};
    u.lx[0] = htonl(hostlonglong >> 32);
    u.lx[1] = htonl(hostlonglong & 0xFFFFFFFFULL);
    return u.llx;

#endif
}

uint64_t tr_ntohll(uint64_t netlonglong)
{
#ifdef HAVE_NTOHLL

    return ntohll(netlonglong);

#else

    /* fallback code by bdonlan at https://stackoverflow.com/questions/809902/64-bit-ntohl-in-c/875505#875505 */
    union
    {
        std::array<uint32_t, 2> lx;
        uint64_t llx;
    } u = {};
    u.llx = netlonglong;
    return ((uint64_t)ntohl(u.lx[0]) << 32) | (uint64_t)ntohl(u.lx[1]);

#endif
}

// --- ENVIRONMENT

bool tr_env_key_exists(char const* key)
{
    TR_ASSERT(key != nullptr);

#ifdef _WIN32
    return GetEnvironmentVariableA(key, nullptr, 0) != 0;
#else
    return getenv(key) != nullptr;
#endif
}

std::string tr_env_get_string(std::string_view key, std::string_view default_value)
{
#ifdef _WIN32

    if (auto const wide_key = tr_win32_utf8_to_native(key); !std::empty(wide_key))
    {
        if (auto const size = GetEnvironmentVariableW(wide_key.c_str(), nullptr, 0); size != 0)
        {
            auto wide_val = std::wstring{};
            wide_val.resize(size);
            if (GetEnvironmentVariableW(wide_key.c_str(), std::data(wide_val), std::size(wide_val)) == std::size(wide_val) - 1)
            {
                TR_ASSERT(wide_val.back() == L'\0');
                wide_val.resize(std::size(wide_val) - 1);
                return tr_win32_native_to_utf8(wide_val);
            }
        }
    }

#else

    auto const szkey = tr_strbuf<char, 256>{ key };

    if (auto const* const value = getenv(szkey); value != nullptr)
    {
        return value;
    }

#endif

    return std::string{ default_value };
}

// ---

namespace
{
namespace tr_net_init_impl
{
class tr_net_init_mgr
{
private:
    tr_net_init_mgr()
    {
        // try to init curl with default settings (currently ssl support + win32 sockets)
        // but if that fails, we need to init win32 sockets as a bare minimum
        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
        {
            curl_global_init(CURL_GLOBAL_WIN32);
        }
    }

public:
    tr_net_init_mgr(tr_net_init_mgr const&) = delete;
    tr_net_init_mgr(tr_net_init_mgr&&) = delete;
    tr_net_init_mgr& operator=(tr_net_init_mgr const&) = delete;
    tr_net_init_mgr& operator=(tr_net_init_mgr&&) = delete;
    ~tr_net_init_mgr()
    {
        curl_global_cleanup();
    }

    static void create()
    {
        if (!instance)
        {
            instance = std::unique_ptr<tr_net_init_mgr>{ new tr_net_init_mgr };
        }
    }

private:
    static std::unique_ptr<tr_net_init_mgr> instance;
};

std::unique_ptr<tr_net_init_mgr> tr_net_init_mgr::instance;
} // namespace tr_net_init_impl
} // namespace

void tr_lib_init()
{
    tr_net_init_impl::tr_net_init_mgr::create();
}

// --- mime-type

std::string_view tr_get_mime_type_for_filename(std::string_view filename)
{
    auto constexpr Compare = [](mime_type_suffix const& entry, auto const& suffix)
    {
        return entry.suffix < suffix;
    };

    if (auto const pos = filename.rfind('.'); pos != std::string_view::npos)
    {
        auto const suffix_lc = tr_strlower(filename.substr(pos + 1));
        auto const it = std::lower_bound(std::begin(mime_type_suffixes), std::end(mime_type_suffixes), suffix_lc, Compare);
        if (it != std::end(mime_type_suffixes) && suffix_lc == it->suffix)
        {
            return it->mime_type;
        }
    }

    // https://developer.mozilla.org/en-US/docs/Web/HTTP/Basics_of_HTTP/MIME_types/Common_types
    // application/octet-stream is the default value.
    // An unknown file type should use this type.
    auto constexpr Fallback = "application/octet-stream"sv;
    return Fallback;
}

// --- parseNum()

#if defined(__GNUC__) && !__has_include(<charconv>)

#include <iomanip> // std::setbase
#include <sstream>

template<typename T, std::enable_if_t<std::is_integral_v<T>, bool> = true>
[[nodiscard]] std::optional<T> tr_num_parse(std::string_view str, std::string_view* remainder, int base)
{
    auto val = T{};
    auto const tmpstr = std::string(std::data(str), std::min(std::size(str), size_t{ 64 }));
    auto sstream = std::stringstream{ tmpstr };
    auto const oldpos = sstream.tellg();
    /* The base parameter only works for bases 8, 10 and 16.
       All other bases will be converted to 0 which activates the
       prefix based parsing and therefore decimal in our usual cases.
       This differs from the from_chars solution below. */
    sstream >> std::setbase(base) >> val;
    auto const newpos = sstream.tellg();
    if ((newpos == oldpos) || (sstream.fail() && !sstream.eof()))
    {
        return std::nullopt;
    }
    if (remainder != nullptr)
    {
        *remainder = str;
        remainder->remove_prefix(sstream.eof() ? std::size(str) : newpos - oldpos);
    }
    return val;
}

#else // #if defined(__GNUC__) && !__has_include(<charconv>)

#include <charconv> // std::from_chars()

template<typename T, std::enable_if_t<std::is_integral_v<T>, bool>>
[[nodiscard]] std::optional<T> tr_num_parse(std::string_view str, std::string_view* remainder, int base)
{
    auto val = T{};
    auto const* const begin_ch = std::data(str);
    auto const* const end_ch = begin_ch + std::size(str);
    /* The base parameter works for any base from 2 to 36 (inclusive).
       This is different from the behaviour of the stringstream
       based solution above. */
    auto const result = std::from_chars(begin_ch, end_ch, val, base);
    if (result.ec != std::errc{})
    {
        return std::nullopt;
    }
    if (remainder != nullptr)
    {
        *remainder = str;
        remainder->remove_prefix(result.ptr - std::data(str));
    }
    return val;
}

#endif // #if defined(__GNUC__) && !__has_include(<charconv>)

template std::optional<long long> tr_num_parse(std::string_view str, std::string_view* remainder, int base);
template std::optional<long> tr_num_parse(std::string_view str, std::string_view* remainder, int base);
template std::optional<int> tr_num_parse(std::string_view str, std::string_view* remainder, int base);
template std::optional<char> tr_num_parse(std::string_view str, std::string_view* remainder, int base);

template std::optional<unsigned long long> tr_num_parse(std::string_view str, std::string_view* remainder, int base);
template std::optional<unsigned long> tr_num_parse(std::string_view str, std::string_view* remainder, int base);
template std::optional<unsigned int> tr_num_parse(std::string_view str, std::string_view* remainder, int base);
template std::optional<unsigned short> tr_num_parse(std::string_view str, std::string_view* remainder, int base);
template std::optional<unsigned char> tr_num_parse(std::string_view str, std::string_view* remainder, int base);

template<typename T, std::enable_if_t<std::is_floating_point_v<T>, bool>>
[[nodiscard]] std::optional<T> tr_num_parse(std::string_view str, std::string_view* remainder)
{
    auto const* const begin_ch = std::data(str);
    auto const* const end_ch = begin_ch + std::size(str);
    auto val = T{};
    auto const result = fast_float::from_chars(begin_ch, end_ch, val);
    if (result.ec != std::errc{})
    {
        return std::nullopt;
    }
    if (remainder != nullptr)
    {
        *remainder = str;
        remainder->remove_prefix(result.ptr - std::data(str));
    }
    return val;
}

template std::optional<double> tr_num_parse(std::string_view sv, std::string_view* remainder);
