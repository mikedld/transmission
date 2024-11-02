// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <climits>
#include <cstring>
#include <cwchar>
#include <map>
#include <iterator>
#include <string>
#include <string_view>

#include <fmt/core.h>
#include <fmt/xchar.h> // for wchar_t support

#include <windows.h>

#include "libtransmission/transmission.h"

#include "libtransmission/error.h"
#include "libtransmission/subprocess.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h"

using namespace std::literals;

namespace
{

enum class tr_app_type : uint8_t
{
    EXE,
    BATCH
};

void set_system_error(tr_error* error, DWORD code, std::string_view what)
{
    if (error == nullptr)
    {
        return;
    }

    if (auto const message = tr_win32_format_message(code); !std::empty(message))
    {
        error->set(code, fmt::format("{:s} failed: {:s}", what, message));
    }
    else
    {
        error->set(code, fmt::format("{:s} failed: Unknown error: {:#08x}", what, code));
    }
}

constexpr bool to_bool(BOOL value) noexcept
{
    return value != FALSE;
}

// "The sort is case-insensitive, Unicode order, without regard to locale" © MSDN
class WStrICompare
{
public:
    [[nodiscard]] static auto compare(std::wstring_view a, std::wstring_view b) noexcept // <=>
    {
        int diff = wcsnicmp(std::data(a), std::data(b), std::min(std::size(a), std::size(b)));

        if (diff == 0)
        {
            diff = tr_compare_3way(std::size(a), std::size(b));
        }

        return diff;
    }

    [[nodiscard]] auto operator()(std::wstring_view a, std::wstring_view b) const noexcept // <
    {
        return compare(a, b) < 0;
    }
};

using SortedWideEnv = std::map<std::wstring, std::wstring, WStrICompare>;

/*
 * Var1=Value1\0
 * Var2=Value2\0
 * Var3=Value3\0
 * ...
 * VarN=ValueN\0\0
 */
auto to_env_string(SortedWideEnv const& wide_env)
{
    auto ret = std::vector<wchar_t>{};

    for (auto const& [key, val] : wide_env)
    {
        fmt::format_to(std::back_inserter(ret), L"{:s}={:s}", key, val);
        ret.insert(std::end(ret), L'\0');
    }

    ret.insert(std::end(ret), L'\0');

    return ret;
}

/*
 * Var1=Value1\0
 * Var2=Value2\0
 * Var3=Value3\0
 * ...
 * VarN=ValueN\0\0
 */
auto parse_env_string(wchar_t const* env)
{
    auto sorted = SortedWideEnv{};

    for (;;)
    {
        auto const line = std::wstring_view{ env };
        if (std::empty(line))
        {
            break;
        }

        if (auto const pos = line.find(L'='); pos != std::string_view::npos)
        {
            sorted.insert_or_assign(std::wstring{ line.substr(0, pos) }, std::wstring{ line.substr(pos + 1) });
        }

        env += std::size(line) + 1 /*'\0'*/;
    }

    return sorted;
}

auto get_current_env()
{
    auto env = SortedWideEnv{};

    if (auto* pwch = GetEnvironmentStringsW(); pwch != nullptr)
    {
        env = parse_env_string(pwch);

        FreeEnvironmentStringsW(pwch);
    }

    return env;
}

void append_argument(std::string& arguments, char const* argument)
{
    TR_ASSERT(argument != nullptr);

    if (!std::empty(arguments))
    {
        arguments += ' ';
    }

    if (*argument != '\0' && strpbrk(argument, " \t\n\v\"") == nullptr)
    {
        arguments += argument;
        return;
    }

    arguments += '"';

    for (char const* src = argument; *src != '\0';)
    {
        size_t backslash_count = 0;

        while (*src == '\\')
        {
            ++backslash_count;
            ++src;
        }

        switch (*src)
        {
        case '\0':
            backslash_count = backslash_count * 2;
            break;

        case '"':
            backslash_count = backslash_count * 2 + 1;
            break;

        default:
            break;
        }

        if (backslash_count != 0)
        {
            arguments.append(backslash_count, '\\');
        }

        if (*src != '\0')
        {
            arguments += *src++;
        }
    }

    arguments += '"';
}

bool contains_batch_metachars(char const* text)
{
    /* First part - chars explicitly documented by `cmd.exe /?` as "special" */
    return strpbrk(
               text,
               "&<>()@^|"
               "%!^\"") != nullptr;
}

auto get_app_type(char const* app)
{
    auto const lower = tr_strlower(app);

    if (tr_strv_ends_with(lower, ".cmd") || tr_strv_ends_with(lower, ".bat"))
    {
        return tr_app_type::BATCH;
    }

    /* TODO: Support other types? */

    return tr_app_type::EXE;
}

void append_app_launcher_arguments(tr_app_type app_type, std::string& args)
{
    switch (app_type)
    {
    case tr_app_type::EXE:
        break;

    case tr_app_type::BATCH:
        append_argument(args, "cmd.exe");
        append_argument(args, "/d");
        append_argument(args, "/e:off");
        append_argument(args, "/v:off");
        append_argument(args, "/s");
        append_argument(args, "/c");
        break;

    default:
        TR_ASSERT_MSG(false, "unsupported application type");
        break;
    }
}

std::wstring construct_cmd_line(char const* const* cmd)
{
    auto const app_type = get_app_type(cmd[0]);

    auto args = std::string{};

    append_app_launcher_arguments(app_type, args);

    for (size_t i = 0; cmd[i] != nullptr; ++i)
    {
        if (app_type == tr_app_type::BATCH && i > 0 && contains_batch_metachars(cmd[i]))
        {
            /* FIXME: My attempts to escape them one or another way didn't lead to anything good so far */
            args.clear();
            break;
        }

        append_argument(args, cmd[i]);
    }

    if (!std::empty(args))
    {
        return tr_win32_utf8_to_native(args);
    }

    return {};
}

} // namespace

bool tr_spawn_async(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    std::string_view work_dir,
    tr_error* error)
{
    // full_env = current_env + env;
    auto full_env = get_current_env();
    for (auto const& [key, val] : env)
    {
        full_env.insert_or_assign(tr_win32_utf8_to_native(key), tr_win32_utf8_to_native(val));
    }

    auto cmd_line = construct_cmd_line(cmd);
    if (std::empty(cmd_line))
    {
        set_system_error(error, ERROR_INVALID_PARAMETER, "Constructing command line");
        return false;
    }

    auto const current_dir = tr_win32_utf8_to_native(work_dir);

    auto si = STARTUPINFOW{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;

    bool const ret = to_bool(CreateProcessW(
        nullptr,
        std::data(cmd_line),
        nullptr,
        nullptr,
        FALSE,
        NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_DEFAULT_ERROR_MODE,
        std::empty(full_env) ? nullptr : to_env_string(full_env).data(),
        std::empty(current_dir) ? nullptr : current_dir.c_str(),
        &si,
        &pi));

    if (ret)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
    {
        set_system_error(error, GetLastError(), "Call to CreateProcess()");
    }

    return ret;
}
