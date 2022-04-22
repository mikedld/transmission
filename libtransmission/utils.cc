// This file Copyright © 2009-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::sort
#include <array> // std::array
#include <cerrno>
#include <cfloat> // DBL_DIG
#include <chrono>
#include <clocale> // localeconv()
#include <cstdint> // SIZE_MAX
#include <cstdlib> // getenv()
#include <cstring> /* strerror() */
#include <ctime> // nanosleep()
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h> /* Sleep(), GetEnvironmentVariable() */

#include <shellapi.h> /* CommandLineToArgv() */
#include <ws2tcpip.h> /* WSAStartup() */
#endif

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#define UTF_CPP_CPLUSPLUS 201703L
#include <utf8.h>

#include <event2/buffer.h>
#include <event2/event.h>

#include <fmt/format.h>

#include "transmission.h"

#include "error-types.h"
#include "error.h"
#include "file.h"
#include "log.h"
#include "mime-types.h"
#include "net.h" // ntohl()
#include "platform-quota.h" /* tr_device_info_create(), tr_device_info_get_disk_space(), tr_device_info_free() */
#include "tr-assert.h"
#include "tr-strbuf.h"
#include "utils.h"
#include "variant.h"

using namespace std::literals;

time_t __tr_current_time = 0;

/***
****
***/

struct timeval tr_gettimeofday()
{
    auto const d = std::chrono::system_clock::now().time_since_epoch();
    auto const s = std::chrono::duration_cast<std::chrono::seconds>(d);
    auto ret = timeval{};
    ret.tv_sec = s.count();
    ret.tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(d - s).count();
    return ret;
}

/***
****
***/

void* tr_malloc(size_t size)
{
    return size != 0 ? malloc(size) : nullptr;
}

void* tr_malloc0(size_t size)
{
    return size != 0 ? calloc(1, size) : nullptr;
}

void* tr_realloc(void* p, size_t size)
{
    void* result = size != 0 ? realloc(p, size) : nullptr;

    if (result == nullptr)
    {
        tr_free(p);
    }

    return result;
}

void tr_free(void* p)
{
    if (p != nullptr)
    {
        free(p);
    }
}

void* tr_memdup(void const* src, size_t byteCount)
{
    return memcpy(tr_malloc(byteCount), src, byteCount);
}

/***
****
***/

void tr_timerAdd(struct event& timer, int seconds, int microseconds)
{
    auto tv = timeval{};
    tv.tv_sec = seconds;
    tv.tv_usec = microseconds;

    TR_ASSERT(tv.tv_sec >= 0);
    TR_ASSERT(tv.tv_usec >= 0);
    TR_ASSERT(tv.tv_usec < 1000000);

    evtimer_add(&timer, &tv);
}

void tr_timerAddMsec(struct event& timer, int msec)
{
    int const seconds = msec / 1000;
    int const usec = (msec % 1000) * 1000;
    tr_timerAdd(timer, seconds, usec);
}

/**
***
**/

uint8_t* tr_loadFile(std::string_view path_in, size_t* size, tr_error** error)
{
    auto const path = tr_pathbuf{ path_in };

    /* try to stat the file */
    auto info = tr_sys_path_info{};
    tr_error* my_error = nullptr;
    if (!tr_sys_path_get_info(path.c_str(), 0, &info, &my_error))
    {
        tr_logAddError(fmt::format(
            _("Couldn't read '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", my_error->message),
            fmt::arg("error_code", my_error->code)));
        tr_error_propagate(error, &my_error);
        return nullptr;
    }

    if (info.type != TR_SYS_PATH_IS_FILE)
    {
        tr_logAddError(fmt::format(_("Couldn't read '{path}': Not a regular file"), fmt::arg("path", path)));
        tr_error_set(error, TR_ERROR_EISDIR, "Not a regular file"sv);
        return nullptr;
    }

    /* file size should be able to fit into size_t */
    if constexpr (sizeof(info.size) > sizeof(*size))
    {
        TR_ASSERT(info.size <= SIZE_MAX);
    }

    /* Load the torrent file into our buffer */
    auto const fd = tr_sys_file_open(path.c_str(), TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, &my_error);
    if (fd == TR_BAD_SYS_FILE)
    {
        tr_logAddError(fmt::format(
            _("Couldn't read '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", my_error->message),
            fmt::arg("error_code", my_error->code)));
        tr_error_propagate(error, &my_error);
        return nullptr;
    }

    auto* buf = static_cast<uint8_t*>(tr_malloc(info.size + 1));
    if (!tr_sys_file_read(fd, buf, info.size, nullptr, &my_error))
    {
        tr_logAddError(fmt::format(
            _("Couldn't read '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", my_error->message),
            fmt::arg("error_code", my_error->code)));
        tr_sys_file_close(fd);
        tr_free(buf);
        tr_error_propagate(error, &my_error);
        return nullptr;
    }

    tr_sys_file_close(fd);
    buf[info.size] = '\0';
    *size = info.size;
    return buf;
}

bool tr_loadFile(std::string_view path_in, std::vector<char>& setme, tr_error** error)
{
    auto const path = tr_pathbuf{ path_in };

    /* try to stat the file */
    auto info = tr_sys_path_info{};
    tr_error* my_error = nullptr;
    if (!tr_sys_path_get_info(path.c_str(), 0, &info, &my_error))
    {
        tr_logAddError(fmt::format(
            _("Couldn't read '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", my_error->message),
            fmt::arg("error_code", my_error->code)));
        tr_error_propagate(error, &my_error);
        return false;
    }

    if (info.type != TR_SYS_PATH_IS_FILE)
    {
        tr_logAddError(fmt::format(_("Couldn't read '{path}': Not a regular file"), fmt::arg("path", path)));
        tr_error_set(error, TR_ERROR_EISDIR, "Not a regular file"sv);
        return false;
    }

    /* Load the torrent file into our buffer */
    auto const fd = tr_sys_file_open(path.c_str(), TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, &my_error);
    if (fd == TR_BAD_SYS_FILE)
    {
        tr_logAddError(fmt::format(
            _("Couldn't read '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", my_error->message),
            fmt::arg("error_code", my_error->code)));
        tr_error_propagate(error, &my_error);
        return false;
    }

    setme.resize(info.size);
    if (!tr_sys_file_read(fd, std::data(setme), info.size, nullptr, &my_error))
    {
        tr_logAddError(fmt::format(
            _("Couldn't read '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", my_error->message),
            fmt::arg("error_code", my_error->code)));
        tr_sys_file_close(fd);
        tr_error_propagate(error, &my_error);
        return false;
    }

    tr_sys_file_close(fd);
    return true;
}

bool tr_saveFile(std::string_view filename_in, std::string_view contents, tr_error** error)
{
    auto const filename = tr_pathbuf{ filename_in };
    // follow symlinks to find the "real" file, to make sure the temporary
    // we build with tr_sys_file_open_temp() is created on the right partition
    if (char* const real_filename = tr_sys_path_resolve(filename.c_str()); real_filename != nullptr)
    {
        if (filename_in != real_filename)
        {
            auto const saved = tr_saveFile(real_filename, contents, error);
            tr_free(real_filename);
            return saved;
        }

        tr_free(real_filename);
    }

    // Write it to a temp file first.
    // This is a safeguard against edge cases, e.g. disk full, crash while writing, etc.
    auto tmp = tr_pathbuf{ filename.sv(), ".tmp.XXXXXX"sv };
    auto const fd = tr_sys_file_open_temp(std::data(tmp), error);
    if (fd == TR_BAD_SYS_FILE)
    {
        return false;
    }

    // Save the contents. This might take >1 pass.
    auto ok = bool{ true };
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
    if (!tr_sys_file_close(fd, error) || !ok || !tr_sys_path_rename(tmp.c_str(), filename.c_str(), error))
    {
        return false;
    }

    tr_logAddTrace(fmt::format("Saved '{}'", filename));
    return true;
}

tr_disk_space tr_dirSpace(std::string_view dir)
{
    if (std::empty(dir))
    {
        errno = EINVAL;
        return { -1, -1 };
    }

    return tr_device_info_get_disk_space(tr_device_info_create(dir));
}

/****
*****
****/

char* tr_strvDup(std::string_view in)
{
    auto const n = std::size(in);
    auto* const ret = tr_new(char, n + 1);
    std::copy(std::begin(in), std::end(in), ret);
    ret[n] = '\0';
    return ret;
}

char* tr_strdup(void const* in)
{
    return in == nullptr ? nullptr : tr_strvDup(static_cast<char const*>(in));
}

extern "C"
{
    int DoMatch(char const* text, char const* p);
}

/* User-level routine. returns whether or not 'text' and 'p' matched */
bool tr_wildmat(std::string_view text, std::string_view pattern)
{
    // TODO(ckerr): replace wildmat with base/strings/pattern.cc
    // wildmat wants these to be zero-terminated.
    return pattern == "*"sv || DoMatch(std::string{ text }.c_str(), std::string{ pattern }.c_str()) != 0;
}

char const* tr_strerror(int i)
{
    char const* ret = strerror(i);

    if (ret == nullptr)
    {
        ret = "Unknown Error";
    }

    return ret;
}

/****
*****
****/

std::string_view tr_strvStrip(std::string_view str)
{
    auto constexpr test = [](auto ch)
    {
        return isspace(ch);
    };

    auto const it = std::find_if_not(std::begin(str), std::end(str), test);
    str.remove_prefix(std::distance(std::begin(str), it));

    auto const rit = std::find_if_not(std::rbegin(str), std::rend(str), test);
    str.remove_suffix(std::distance(std::rbegin(str), rit));

    return str;
}

bool tr_str_has_suffix(char const* str, char const* suffix)
{
    if (str == nullptr)
    {
        return false;
    }

    if (suffix == nullptr)
    {
        return true;
    }

    auto const str_len = strlen(str);
    auto const suffix_len = strlen(suffix);

    if (str_len < suffix_len)
    {
        return false;
    }

    return evutil_ascii_strncasecmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

/****
*****
****/

uint64_t tr_time_msec()
{
    auto const tv = tr_gettimeofday();
    return uint64_t(tv.tv_sec) * 1000 + (tv.tv_usec / 1000);
}

void tr_wait_msec(long int msec)
{
#ifdef _WIN32

    Sleep((DWORD)msec);

#else

    struct timespec ts;
    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;
    nanosleep(&ts, nullptr);

#endif
}

/***
****
***/

/*
 * Copy src to string dst of size siz. At most siz-1 characters
 * will be copied. Always NUL terminates (unless siz == 0).
 * Returns strlen (src); if retval >= siz, truncation occurred.
 */
size_t tr_strlcpy(void* vdst, void const* vsrc, size_t siz)
{
    auto* dst = static_cast<char*>(vdst);
    auto const* const src = static_cast<char const*>(vsrc);

    TR_ASSERT(dst != nullptr);
    TR_ASSERT(src != nullptr);

#ifdef HAVE_STRLCPY

    return strlcpy(dst, src, siz);

#else

    auto* d = dst;
    auto const* s = src;
    size_t n = siz;

    /* Copy as many bytes as will fit */
    if (n != 0)
    {
        while (--n != 0)
        {
            if ((*d++ = *s++) == '\0')
            {
                break;
            }
        }
    }

    /* Not enough room in dst, add NUL and traverse rest of src */
    if (n == 0)
    {
        if (siz != 0)
        {
            *d = '\0'; /* NUL-terminate dst */
        }

        while (*s++ != '\0')
        {
        }
    }

    return s - (char const*)src - 1; /* count does not include NUL */

#endif
}

/***
****
***/

double tr_getRatio(uint64_t numerator, uint64_t denominator)
{
    if (denominator > 0)
    {
        return numerator / (double)denominator;
    }

    if (numerator > 0)
    {
        return TR_RATIO_INF;
    }

    return TR_RATIO_NA;
}

/***
****
***/

void tr_removeElementFromArray(void* array, size_t index_to_remove, size_t sizeof_element, size_t nmemb)
{
    auto* a = static_cast<char*>(array);

    memmove(
        a + sizeof_element * index_to_remove,
        a + sizeof_element * (index_to_remove + 1),
        sizeof_element * (--nmemb - index_to_remove));
}

/***
****
***/

bool tr_utf8_validate(std::string_view sv, char const** good_end)
{
    auto const* begin = std::data(sv);
    auto const* const end = begin + std::size(sv);
    auto const* walk = begin;
    auto all_good = false;

    try
    {
        while (walk < end)
        {
            utf8::next(walk, end);
        }

        all_good = true;
    }
    catch (utf8::exception const&)
    {
        all_good = false;
    }

    if (good_end != nullptr)
    {
        *good_end = walk;
    }

    return all_good;
}

static char* strip_non_utf8(std::string_view sv)
{
    auto* const ret = tr_new(char, std::size(sv) + 1);
    if (ret != nullptr)
    {
        auto const it = utf8::unchecked::replace_invalid(std::data(sv), std::data(sv) + std::size(sv), ret, '?');
        *it = '\0';
    }
    return ret;
}

static char* to_utf8(std::string_view sv)
{
#ifdef HAVE_ICONV
    size_t const buflen = std::size(sv) * 4 + 10;
    auto* const out = tr_new(char, buflen);

    auto constexpr Encodings = std::array<char const*, 2>{ "CURRENT", "ISO-8859-15" };
    for (auto const* test_encoding : Encodings)
    {
        iconv_t cd = iconv_open("UTF-8", test_encoding);
        if (cd == (iconv_t)-1) // NOLINT(performance-no-int-to-ptr)
        {
            continue;
        }

#ifdef ICONV_SECOND_ARGUMENT_IS_CONST
        auto const* inbuf = std::data(sv);
#else
        auto* inbuf = const_cast<char*>(std::data(sv));
#endif
        char* outbuf = out;
        size_t inbytesleft = std::size(sv);
        size_t outbytesleft = buflen;
        auto const rv = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
        iconv_close(cd);
        if (rv != size_t(-1))
        {
            char* const ret = tr_strvDup({ out, buflen - outbytesleft });
            tr_free(out);
            return ret;
        }
    }

    tr_free(out);

#endif

    return strip_non_utf8(sv);
}

std::string& tr_strvUtf8Clean(std::string_view cleanme, std::string& setme)
{
    if (tr_utf8_validate(cleanme, nullptr))
    {
        setme = cleanme;
    }
    else
    {
        auto* const tmp = to_utf8(cleanme);
        setme.assign(tmp != nullptr ? tmp : "");
        tr_free(tmp);
    }

    return setme;
}

#ifdef _WIN32

char* tr_win32_native_to_utf8(wchar_t const* text, int text_size)
{
    return tr_win32_native_to_utf8_ex(text, text_size, 0, 0, nullptr);
}

char* tr_win32_native_to_utf8_ex(
    wchar_t const* text,
    int text_size,
    int extra_chars_before,
    int extra_chars_after,
    int* real_result_size)
{
    char* ret = nullptr;
    int size;

    if (text_size == -1)
    {
        text_size = wcslen(text);
    }

    size = WideCharToMultiByte(CP_UTF8, 0, text, text_size, nullptr, 0, nullptr, nullptr);

    if (size == 0)
    {
        goto fail;
    }

    ret = tr_new(char, size + extra_chars_before + extra_chars_after + 1);
    size = WideCharToMultiByte(CP_UTF8, 0, text, text_size, ret + extra_chars_before, size, nullptr, nullptr);

    if (size == 0)
    {
        goto fail;
    }

    ret[size + extra_chars_before + extra_chars_after] = '\0';

    if (real_result_size != nullptr)
    {
        *real_result_size = size;
    }

    return ret;

fail:
    tr_free(ret);

    return nullptr;
}

wchar_t* tr_win32_utf8_to_native(char const* text, int text_size)
{
    return tr_win32_utf8_to_native_ex(text, text_size, 0, 0, nullptr);
}

wchar_t* tr_win32_utf8_to_native_ex(
    char const* text,
    int text_size,
    int extra_chars_before,
    int extra_chars_after,
    int* real_result_size)
{
    wchar_t* ret = nullptr;
    int size;

    if (text_size == -1)
    {
        text_size = strlen(text);
    }

    size = MultiByteToWideChar(CP_UTF8, 0, text, text_size, nullptr, 0);

    if (size == 0)
    {
        goto fail;
    }

    ret = tr_new(wchar_t, size + extra_chars_before + extra_chars_after + 1);
    size = MultiByteToWideChar(CP_UTF8, 0, text, text_size, ret + extra_chars_before, size);

    if (size == 0)
    {
        goto fail;
    }

    ret[size + extra_chars_before + extra_chars_after] = L'\0';

    if (real_result_size != nullptr)
    {
        *real_result_size = size;
    }

    return ret;

fail:
    tr_free(ret);

    return nullptr;
}

char* tr_win32_format_message(uint32_t code)
{
    wchar_t* wide_text = nullptr;
    DWORD wide_size;
    char* text = nullptr;
    size_t text_size;

    wide_size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        (LPWSTR)&wide_text,
        0,
        nullptr);

    if (wide_size == 0)
    {
        return tr_strvDup(fmt::format(FMT_STRING("Unknown error ({:#08x})"), code));
    }

    if (wide_size != 0 && wide_text != nullptr)
    {
        text = tr_win32_native_to_utf8(wide_text, wide_size);
    }

    LocalFree(wide_text);

    if (text != nullptr)
    {
        /* Most (all?) messages contain "\r\n" in the end, chop it */
        text_size = strlen(text);

        while (text_size > 0 && isspace((uint8_t)text[text_size - 1]))
        {
            text[--text_size] = '\0';
        }
    }

    return text;
}

void tr_win32_make_args_utf8(int* argc, char*** argv)
{
    int my_argc;
    wchar_t** my_wide_argv;

    my_wide_argv = CommandLineToArgvW(GetCommandLineW(), &my_argc);

    if (my_wide_argv == nullptr)
    {
        return;
    }

    TR_ASSERT(*argc == my_argc);

    char** my_argv = tr_new(char*, my_argc + 1);
    int processed_argc = 0;

    for (int i = 0; i < my_argc; ++i, ++processed_argc)
    {
        my_argv[i] = tr_win32_native_to_utf8(my_wide_argv[i], -1);

        if (my_argv[i] == nullptr)
        {
            break;
        }
    }

    if (processed_argc < my_argc)
    {
        for (int i = 0; i < processed_argc; ++i)
        {
            tr_free(my_argv[i]);
        }

        tr_free(my_argv);
    }
    else
    {
        my_argv[my_argc] = nullptr;

        *argc = my_argc;
        *argv = my_argv;

        /* TODO: Add atexit handler to cleanup? */
    }

    LocalFree(my_wide_argv);
}

int tr_main_win32(int argc, char** argv, int (*real_main)(int, char**))
{
    tr_win32_make_args_utf8(&argc, &argv);
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    return (*real_main)(argc, argv);
}

#endif

/***
****
***/

struct number_range
{
    int low;
    int high;
};

/**
 * This should be a single number (ex. "6") or a range (ex. "6-9").
 * Anything else is an error and will return failure.
 */
static bool parseNumberSection(std::string_view str, number_range& range)
{
    auto constexpr Delimiter = "-"sv;

    auto const first = tr_parseNum<size_t>(str);
    if (!first)
    {
        return false;
    }

    range.low = range.high = *first;
    if (std::empty(str))
    {
        return true;
    }

    if (!tr_strvStartsWith(str, Delimiter))
    {
        return false;
    }

    str.remove_prefix(std::size(Delimiter));
    auto const second = tr_parseNum<size_t>(str);
    if (!second)
    {
        return false;
    }

    range.high = *second;
    return true;
}

/**
 * Given a string like "1-4" or "1-4,6,9,14-51", this allocates and returns an
 * array of setmeCount ints of all the values in the array.
 * For example, "5-8" will return [ 5, 6, 7, 8 ] and setmeCount will be 4.
 * It's the caller's responsibility to call tr_free () on the returned array.
 * If a fragment of the string can't be parsed, nullptr is returned.
 */
std::vector<int> tr_parseNumberRange(std::string_view str)
{
    auto values = std::set<int>{};
    auto token = std::string_view{};
    auto range = number_range{};
    while (tr_strvSep(&str, &token, ',') && parseNumberSection(token, range))
    {
        for (auto i = range.low; i <= range.high; ++i)
        {
            values.insert(i);
        }
    }

    return { std::begin(values), std::end(values) };
}

/***
****
***/

double tr_truncd(double x, int precision)
{
    auto buf = std::array<char, 128>{};
    auto const [out, len] = fmt::format_to_n(std::data(buf), std::size(buf) - 1, "{:.{}f}", x, DBL_DIG);
    *out = '\0';

    if (auto* const pt = strstr(std::data(buf), localeconv()->decimal_point); pt != nullptr)
    {
        pt[precision != 0 ? precision + 1 : 0] = '\0';
    }

    return atof(std::data(buf));
}

std::string tr_strpercent(double x)
{
    if (x < 5.0)
    {
        return fmt::format("{:.2f}", tr_truncd(x, 2));
    }

    if (x < 100.0)
    {
        return fmt::format("{:.1f}", tr_truncd(x, 1));
    }

    return fmt::format("{:.0f}", x);
}

std::string tr_strratio(double ratio, char const* infinity)
{
    if ((int)ratio == TR_RATIO_NA)
    {
        return _("None");
    }

    if ((int)ratio == TR_RATIO_INF)
    {
        auto buf = std::array<char, 64>{};
        tr_strlcpy(std::data(buf), infinity, std::size(buf));
        return std::data(buf);
    }

    return tr_strpercent(ratio);
}

/***
****
***/

bool tr_moveFile(std::string_view oldpath_in, std::string_view newpath_in, tr_error** error)
{
    auto const oldpath = tr_pathbuf{ oldpath_in };
    auto const newpath = tr_pathbuf{ newpath_in };

    // make sure the old file exists
    auto info = tr_sys_path_info{};
    if (!tr_sys_path_get_info(oldpath, 0, &info, error))
    {
        tr_error_prefix(error, "Unable to get information on old file: ");
        return false;
    }
    if (info.type != TR_SYS_PATH_IS_FILE)
    {
        tr_error_set(error, TR_ERROR_EINVAL, "Old path does not point to a file."sv);
        return false;
    }

    // ensure the target directory exists
    if (auto const newdir = tr_sys_path_dirname(newpath, error);
        std::empty(newdir) || !tr_sys_dir_create(newdir.c_str(), TR_SYS_DIR_CREATE_PARENTS, 0777, error))
    {
        tr_error_prefix(error, "Unable to create directory for new file: ");
        return false;
    }

    /* they might be on the same filesystem... */
    if (tr_sys_path_rename(oldpath, newpath))
    {
        return true;
    }

    /* Otherwise, copy the file. */
    if (!tr_sys_path_copy(oldpath, newpath, error))
    {
        tr_error_prefix(error, "Unable to copy: ");
        return false;
    }

    if (tr_error* my_error = nullptr; !tr_sys_path_remove(oldpath, &my_error))
    {
        tr_logAddError(fmt::format(
            _("Couldn't remove '{path}': {error} ({error_code})"),
            fmt::arg("path", oldpath),
            fmt::arg("error", my_error->message),
            fmt::arg("error_code", my_error->code)));
        tr_error_free(my_error);
    }

    return true;
}

/***
****
***/

uint64_t tr_htonll(uint64_t x)
{
#ifdef HAVE_HTONLL

    return htonll(x);

#else

    /* fallback code by bdonlan at https://stackoverflow.com/questions/809902/64-bit-ntohl-in-c/875505#875505 */
    union
    {
        uint32_t lx[2];
        uint64_t llx;
    } u;
    u.lx[0] = htonl(x >> 32);
    u.lx[1] = htonl(x & 0xFFFFFFFFULL);
    return u.llx;

#endif
}

uint64_t tr_ntohll(uint64_t x)
{
#ifdef HAVE_NTOHLL

    return ntohll(x);

#else

    /* fallback code by bdonlan at https://stackoverflow.com/questions/809902/64-bit-ntohl-in-c/875505#875505 */
    union
    {
        uint32_t lx[2];
        uint64_t llx;
    } u;
    u.llx = x;
    return ((uint64_t)ntohl(u.lx[0]) << 32) | (uint64_t)ntohl(u.lx[1]);

#endif
}

/***
****
****
****
***/

struct formatter_unit
{
    std::array<char, 16> name;
    uint64_t value;
};

using formatter_units = std::array<formatter_unit, 4>;

enum
{
    TR_FMT_KB,
    TR_FMT_MB,
    TR_FMT_GB,
    TR_FMT_TB
};

static void formatter_init(
    formatter_units& units,
    uint64_t kilo,
    char const* kb,
    char const* mb,
    char const* gb,
    char const* tb)
{
    uint64_t value = kilo;
    tr_strlcpy(std::data(units[TR_FMT_KB].name), kb, std::size(units[TR_FMT_KB].name));
    units[TR_FMT_KB].value = value;

    value *= kilo;
    tr_strlcpy(std::data(units[TR_FMT_MB].name), mb, std::size(units[TR_FMT_MB].name));
    units[TR_FMT_MB].value = value;

    value *= kilo;
    tr_strlcpy(std::data(units[TR_FMT_GB].name), gb, std::size(units[TR_FMT_GB].name));
    units[TR_FMT_GB].value = value;

    value *= kilo;
    tr_strlcpy(std::data(units[TR_FMT_TB].name), tb, std::size(units[TR_FMT_TB].name));
    units[TR_FMT_TB].value = value;
}

static char* formatter_get_size_str(formatter_units const& u, char* buf, uint64_t bytes, size_t buflen)
{
    formatter_unit const* unit = nullptr;

    if (bytes < u[1].value)
    {
        unit = std::data(u);
    }
    else if (bytes < u[2].value)
    {
        unit = &u[1];
    }
    else if (bytes < u[3].value)
    {
        unit = &u[2];
    }
    else
    {
        unit = &u[3];
    }

    double value = double(bytes) / unit->value;
    auto const* const units = std::data(unit->name);

    auto precision = int{};
    if (unit->value == 1)
    {
        precision = 0;
    }
    else if (value < 100)
    {
        precision = 2;
    }
    else
    {
        precision = 1;
    }

    auto const [out, len] = fmt::format_to_n(buf, buflen - 1, "{:.{}f} {:s}", value, precision, units);
    *out = '\0';
    return buf;
}

static formatter_units size_units;

void tr_formatter_size_init(uint64_t kilo, char const* kb, char const* mb, char const* gb, char const* tb)
{
    formatter_init(size_units, kilo, kb, mb, gb, tb);
}

std::string tr_formatter_size_B(uint64_t bytes)
{
    auto buf = std::array<char, 64>{};
    return formatter_get_size_str(size_units, std::data(buf), bytes, std::size(buf));
}

static formatter_units speed_units;

size_t tr_speed_K = 0;

void tr_formatter_speed_init(size_t kilo, char const* kb, char const* mb, char const* gb, char const* tb)
{
    tr_speed_K = kilo;
    formatter_init(speed_units, kilo, kb, mb, gb, tb);
}

std::string tr_formatter_speed_KBps(double KBps)
{
    auto speed = KBps;

    if (speed <= 999.95) // 0.0 KB to 999.9 KB
    {
        return fmt::format("{:d} {:s}", int(speed), std::data(speed_units[TR_FMT_KB].name));
    }

    double const K = speed_units[TR_FMT_KB].value;
    speed /= K;

    if (speed <= 99.995) // 0.98 MB to 99.99 MB
    {
        return fmt::format("{:.2f} {:s}", speed, std::data(speed_units[TR_FMT_MB].name));
    }

    if (speed <= 999.95) // 100.0 MB to 999.9 MB
    {
        return fmt::format("{:.1f} {:s}", speed, std::data(speed_units[TR_FMT_MB].name));
    }

    return fmt::format("{:.1f} {:s}", speed / K, std::data(speed_units[TR_FMT_GB].name));
}

static formatter_units mem_units;

size_t tr_mem_K = 0;

void tr_formatter_mem_init(size_t kilo, char const* kb, char const* mb, char const* gb, char const* tb)
{
    tr_mem_K = kilo;
    formatter_init(mem_units, kilo, kb, mb, gb, tb);
}

std::string tr_formatter_mem_B(size_t bytes_per_second)
{
    auto buf = std::array<char, 64>{};
    return formatter_get_size_str(mem_units, std::data(buf), bytes_per_second, std::size(buf));
}

void tr_formatter_get_units(void* vdict)
{
    auto* dict = static_cast<tr_variant*>(vdict);

    tr_variantDictReserve(dict, 6);

    tr_variantDictAddInt(dict, TR_KEY_memory_bytes, mem_units[TR_FMT_KB].value);
    tr_variant* l = tr_variantDictAddList(dict, TR_KEY_memory_units, std::size(mem_units));
    for (auto const& unit : mem_units)
    {
        tr_variantListAddStr(l, std::data(unit.name));
    }

    tr_variantDictAddInt(dict, TR_KEY_size_bytes, size_units[TR_FMT_KB].value);
    l = tr_variantDictAddList(dict, TR_KEY_size_units, std::size(size_units));
    for (auto const& unit : size_units)
    {
        tr_variantListAddStr(l, std::data(unit.name));
    }

    tr_variantDictAddInt(dict, TR_KEY_speed_bytes, speed_units[TR_FMT_KB].value);
    l = tr_variantDictAddList(dict, TR_KEY_speed_units, std::size(speed_units));
    for (auto const& unit : speed_units)
    {
        tr_variantListAddStr(l, std::data(unit.name));
    }
}

/***
****  ENVIRONMENT
***/

bool tr_env_key_exists(char const* key)
{
    TR_ASSERT(key != nullptr);

#ifdef _WIN32
    return GetEnvironmentVariableA(key, nullptr, 0) != 0;
#else
    return getenv(key) != nullptr;
#endif
}

int tr_env_get_int(char const* key, int default_value)
{
    TR_ASSERT(key != nullptr);

#ifdef _WIN32

    char value[16];

    if (GetEnvironmentVariableA(key, value, TR_N_ELEMENTS(value)) > 1)
    {
        return atoi(value);
    }

#else

    if (char const* value = getenv(key); !tr_str_is_empty(value))
    {
        return atoi(value);
    }

#endif

    return default_value;
}

char* tr_env_get_string(char const* key, char const* default_value)
{
    TR_ASSERT(key != nullptr);

#ifdef _WIN32

    wchar_t* wide_key = tr_win32_utf8_to_native(key, -1);
    char* value = nullptr;

    if (wide_key != nullptr)
    {
        DWORD const size = GetEnvironmentVariableW(wide_key, nullptr, 0);

        if (size != 0)
        {
            wchar_t* const wide_value = tr_new(wchar_t, size);

            if (GetEnvironmentVariableW(wide_key, wide_value, size) == size - 1)
            {
                value = tr_win32_native_to_utf8(wide_value, size);
            }

            tr_free(wide_value);
        }

        tr_free(wide_key);
    }

    if (value == nullptr && default_value != nullptr)
    {
        value = tr_strdup(default_value);
    }

    return value;

#else

    char const* value = getenv(key);

    if (value == nullptr)
    {
        value = default_value;
    }

    return value != nullptr ? tr_strvDup(value) : nullptr;

#endif
}

/***
****
***/

void tr_net_init()
{
    static bool initialized = false;

    if (!initialized)
    {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

        initialized = true;
    }
}

/// mime-type

std::string_view tr_get_mime_type_for_filename(std::string_view filename)
{
    auto constexpr compare = [](mime_type_suffix const& entry, auto const& suffix)
    {
        return entry.suffix < suffix;
    };

    if (auto const pos = filename.rfind('.'); pos != std::string_view::npos)
    {
        auto const suffix_lc = tr_strlower(filename.substr(pos + 1));
        auto const it = std::lower_bound(std::begin(mime_type_suffixes), std::end(mime_type_suffixes), suffix_lc, compare);
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
