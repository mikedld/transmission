/*
 * This file Copyright (C) 2009-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <list>
#include <string>
#include <string_view>

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600 /* needed for recursive locks. */
#endif
#ifndef __USE_UNIX98
#define __USE_UNIX98 /* some older Linuxes need it spelt out for them */
#endif

#ifdef __HAIKU__
#include <limits.h> /* PATH_MAX */
#endif

#ifdef _WIN32
#include <process.h> /* _beginthreadex(), _endthreadex() */
#include <windows.h>
#include <shlobj.h> /* SHGetKnownFolderPath(), FOLDERID_... */
#else
#include <unistd.h> /* getuid() */
#ifdef BUILD_MAC_CLIENT
#include <CoreFoundation/CoreFoundation.h>
#endif
#ifdef __HAIKU__
#include <FindDirectory.h>
#endif
#include <pthread.h>
#endif

#include "transmission.h"
#include "file.h"
#include "log.h"
#include "platform.h"
#include "session.h"
#include "tr-assert.h"
#include "utils.h"

using namespace std::literals;

static char* tr_buildPath(char const* first_element, ...)
{
    // pass 1: allocate enough space for the string
    va_list vl;
    va_start(vl, first_element);
    auto bufLen = size_t{};
    for (char const* element = first_element; element != nullptr;)
    {
        bufLen += strlen(element) + 1;
        element = va_arg(vl, char const*);
    }
    va_end(vl);
    auto* const buf = tr_new(char, bufLen);
    if (buf == nullptr)
    {
        return nullptr;
    }

    /* pass 2: build the string piece by piece */
    char* pch = buf;
    va_start(vl, first_element);
    for (char const* element = first_element; element != nullptr;)
    {
        size_t const elementLen = strlen(element);
        pch = std::copy_n(element, elementLen, pch);
        *pch++ = TR_PATH_DELIMITER;
        element = va_arg(vl, char const*);
    }
    va_end(vl);

    // if nonempty, eat the unwanted trailing slash
    if (pch != buf)
    {
        --pch;
    }

    // zero-terminate the string
    *pch++ = '\0';

    /* sanity checks & return */
    TR_ASSERT(pch - buf == (ptrdiff_t)bufLen);
    return buf;
}

/***
****  THREADS
***/

#ifdef _WIN32
using tr_thread_id = DWORD;
#else
using tr_thread_id = pthread_t;
#endif

static tr_thread_id tr_getCurrentThread(void)
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return pthread_self();
#endif
}

static bool tr_areThreadsEqual(tr_thread_id a, tr_thread_id b)
{
#ifdef _WIN32
    return a == b;
#else
    return pthread_equal(a, b) != 0;
#endif
}

/** @brief portability wrapper around OS-dependent threads */
struct tr_thread
{
    void (*func)(void*);
    void* arg;
    tr_thread_id thread;

#ifdef _WIN32
    HANDLE thread_handle;
#endif
};

bool tr_amInThread(tr_thread const* t)
{
    return tr_areThreadsEqual(tr_getCurrentThread(), t->thread);
}

#ifdef _WIN32
#define ThreadFuncReturnType unsigned WINAPI
#else
#define ThreadFuncReturnType void*
#endif

static ThreadFuncReturnType ThreadFunc(void* _t)
{
#ifndef _WIN32
    pthread_detach(pthread_self());
#endif

    auto* t = static_cast<tr_thread*>(_t);

    t->func(t->arg);

    tr_free(t);

#ifdef _WIN32
    _endthreadex(0);
    return 0;
#else
    return nullptr;
#endif
}

tr_thread* tr_threadNew(void (*func)(void*), void* arg)
{
    auto* t = static_cast<tr_thread*>(tr_new0(tr_thread, 1));

    t->func = func;
    t->arg = arg;

#ifdef _WIN32

    {
        unsigned int id;
        t->thread_handle = (HANDLE)_beginthreadex(nullptr, 0, &ThreadFunc, t, 0, &id);
        t->thread = (DWORD)id;
    }

#else

    pthread_create(&t->thread, nullptr, (void* (*)(void*))ThreadFunc, t);

#endif

    return t;
}

/***
****  PATHS
***/

#ifndef _WIN32
#include <pwd.h>
#endif

#ifdef _WIN32

static char* win32_get_known_folder_ex(REFKNOWNFOLDERID folder_id, DWORD flags)
{
    char* ret = nullptr;
    PWSTR path;

    if (SHGetKnownFolderPath(folder_id, flags | KF_FLAG_DONT_UNEXPAND, nullptr, &path) == S_OK)
    {
        ret = tr_win32_native_to_utf8(path, -1);
        CoTaskMemFree(path);
    }

    return ret;
}

static char* win32_get_known_folder(REFKNOWNFOLDERID folder_id)
{
    return win32_get_known_folder_ex(folder_id, KF_FLAG_DONT_VERIFY);
}

#endif

static char const* getHomeDir(void)
{
    static char const* home = nullptr;

    if (home == nullptr)
    {
        home = tr_env_get_string("HOME", nullptr);

        if (home == nullptr)
        {
#ifdef _WIN32

            home = win32_get_known_folder(FOLDERID_Profile);

#else

            struct passwd pwent;
            struct passwd* pw = nullptr;
            char buf[4096];
            getpwuid_r(getuid(), &pwent, buf, sizeof buf, &pw);
            if (pw != nullptr)
            {
                home = tr_strdup(pw->pw_dir);
            }

#endif
        }

        if (home == nullptr)
        {
            home = tr_strdup("");
        }
    }

    return home;
}

void tr_setConfigDir(tr_session* session, std::string_view config_dir)
{
#if defined(__APPLE__) || defined(_WIN32)
    auto constexpr ResumeSubdir = "Resume"sv;
    auto constexpr TorrentSubdir = "Torrents"sv;
#else
    auto constexpr ResumeSubdir = "resume"sv;
    auto constexpr TorrentSubdir = "torrents"sv;
#endif

    session->config_dir = config_dir;
    session->resume_dir = tr_strvPath(config_dir, ResumeSubdir);
    session->torrent_dir = tr_strvPath(config_dir, TorrentSubdir);
    tr_sys_dir_create(session->resume_dir.c_str(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);
    tr_sys_dir_create(session->torrent_dir.c_str(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);
}

char const* tr_sessionGetConfigDir(tr_session const* session)
{
    return session->config_dir.c_str();
}

char const* tr_getTorrentDir(tr_session const* session)
{
    return session->torrent_dir.c_str();
}

char const* tr_getResumeDir(tr_session const* session)
{
    return session->resume_dir.c_str();
}

char const* tr_getDefaultConfigDir(char const* appname)
{
    static char const* s = nullptr;

    if (tr_str_is_empty(appname))
    {
        appname = "Transmission";
    }

    if (s == nullptr)
    {
        s = tr_env_get_string("TRANSMISSION_HOME", nullptr);

        if (s == nullptr)
        {
#ifdef __APPLE__

            s = tr_buildPath(getHomeDir(), "Library", "Application Support", appname, nullptr);

#elif defined(_WIN32)

            char* appdata = win32_get_known_folder(FOLDERID_LocalAppData);
            s = tr_buildPath(appdata, appname, nullptr);
            tr_free(appdata);

#elif defined(__HAIKU__)

            char buf[PATH_MAX];
            find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, buf, sizeof(buf));
            s = tr_buildPath(buf, appname, nullptr);

#else

            char* const xdg_config_home = tr_env_get_string("XDG_CONFIG_HOME", nullptr);

            if (xdg_config_home != nullptr)
            {
                s = tr_buildPath(xdg_config_home, appname, nullptr);
                tr_free(xdg_config_home);
            }
            else
            {
                s = tr_buildPath(getHomeDir(), ".config", appname, nullptr);
            }

#endif
        }
    }

    return s;
}

static std::string getUserDirsFilename()
{
    char* const config_home = tr_env_get_string("XDG_CONFIG_HOME", nullptr);
    auto config_file = !tr_str_is_empty(config_home) ? tr_strvPath(config_home, "user-dirs.dirs") :
                                                       tr_strvPath(getHomeDir(), ".config", "user-dirs.dirs");

    tr_free(config_home);
    return config_file;
}

static std::string getXdgEntryFromUserDirs(std::string_view key)
{
    auto content = std::vector<char>{};
    if (!tr_loadFile(content, getUserDirsFilename()) && std::empty(content))
    {
        return {};
    }

    // search for key="val" and extract val
    auto const search = tr_strvJoin(key, "=\""sv);
    auto begin = std::search(std::begin(content), std::end(content), std::begin(search), std::end(search));
    if (begin == std::end(content))
    {
        return {};
    }
    std::advance(begin, std::size(search));
    auto const end = std::find(begin, std::end(content), '"');
    if (end == std::end(content))
    {
        return {};
    }
    auto val = std::string{ begin, end };

    // if val contains "$HOME", replace that with getHomeDir()
    auto constexpr Home = "$HOME"sv;
    auto const it = std::search(std::begin(val), std::end(val), std::begin(Home), std::end(Home));
    if (it != std::end(val))
    {
        val.replace(it, it + std::size(Home), std::string_view{ getHomeDir() });
    }

    return val;
}

char const* tr_getDefaultDownloadDir(void)
{
    static char const* user_dir = nullptr;

    if (user_dir == nullptr)
    {
        auto const xdg_user_dir = getXdgEntryFromUserDirs("XDG_DOWNLOAD_DIR"sv);
        if (!std::empty(xdg_user_dir))
        {
            user_dir = tr_strvDup(xdg_user_dir);
        }

#ifdef _WIN32
        if (user_dir == nullptr)
        {
            user_dir = win32_get_known_folder(FOLDERID_Downloads);
        }
#endif

        if (user_dir == nullptr)
        {
#ifdef __HAIKU__
            user_dir = tr_buildPath(getHomeDir(), "Desktop", nullptr);
#else
            user_dir = tr_buildPath(getHomeDir(), "Downloads", nullptr);
#endif
        }
    }

    return user_dir;
}

/***
****
***/

static bool isWebClientDir(std::string_view path)
{
    auto tmp = tr_strvPath(path, "index.html");
    bool const ret = tr_sys_path_exists(tmp.c_str(), nullptr);
    tr_logAddInfo(_("Searching for web interface file \"%s\""), tmp.c_str());

    return ret;
}

char const* tr_getWebClientDir([[maybe_unused]] tr_session const* session)
{
    static char const* s = nullptr;

    if (s == nullptr)
    {
        s = tr_env_get_string("CLUTCH_HOME", nullptr);

        if (s == nullptr)
        {
            s = tr_env_get_string("TRANSMISSION_WEB_HOME", nullptr);
        }

        if (s == nullptr)
        {
#ifdef BUILD_MAC_CLIENT /* on Mac, look in the Application Support folder first, then in the app bundle. */

            /* Look in the Application Support folder */
            s = tr_buildPath(tr_sessionGetConfigDir(session), "public_html", nullptr);

            if (!isWebClientDir(s))
            {
                tr_free(const_cast<char*>(s));

                CFURLRef appURL = CFBundleCopyBundleURL(CFBundleGetMainBundle());
                CFStringRef appRef = CFURLCopyFileSystemPath(appURL, kCFURLPOSIXPathStyle);
                CFIndex const appStringLength = CFStringGetMaximumSizeOfFileSystemRepresentation(appRef);

                char* appString = static_cast<char*>(tr_malloc(appStringLength));
                bool const success = CFStringGetFileSystemRepresentation(appRef, appString, appStringLength);
                TR_ASSERT(success);

                CFRelease(appURL);
                CFRelease(appRef);

                /* Fallback to the app bundle */
                s = tr_buildPath(appString, "Contents", "Resources", "public_html", nullptr);

                if (!isWebClientDir(s))
                {
                    tr_free(const_cast<char*>(s));
                    s = nullptr;
                }

                tr_free(appString);
            }

#elif defined(_WIN32)

            /* Generally, Web interface should be stored in a Web subdir of
             * calling executable dir. */

            static KNOWNFOLDERID const* const known_folder_ids[] = {
                &FOLDERID_LocalAppData,
                &FOLDERID_RoamingAppData,
                &FOLDERID_ProgramData,
            };

            for (size_t i = 0; s == nullptr && i < TR_N_ELEMENTS(known_folder_ids); ++i)
            {
                char* dir = win32_get_known_folder(*known_folder_ids[i]);
                char* path = tr_buildPath(dir, "Transmission", "Web", nullptr);
                tr_free(dir);

                if (isWebClientDir(path))
                {
                    s = path;
                }
                else
                {
                    tr_free(path);
                }
            }

            if (s == nullptr) /* check calling module place */
            {
                wchar_t wide_module_path[MAX_PATH];
                GetModuleFileNameW(nullptr, wide_module_path, TR_N_ELEMENTS(wide_module_path));
                char* module_path = tr_win32_native_to_utf8(wide_module_path, -1);
                char* dir = tr_sys_path_dirname(module_path, nullptr);
                tr_free(module_path);

                if (dir != nullptr)
                {
                    char* path = tr_buildPath(dir, "Web", nullptr);
                    tr_free(dir);

                    if (isWebClientDir(path))
                    {
                        s = path;
                    }
                    else
                    {
                        tr_free(path);
                    }
                }
            }

#else /* everyone else, follow the XDG spec */

            auto candidates = std::list<std::string>{};

            /* XDG_DATA_HOME should be the first in the list of candidates */
            char* tmp = tr_env_get_string("XDG_DATA_HOME", nullptr);
            if (!tr_str_is_empty(tmp))
            {
                candidates.emplace_back(tmp);
            }
            else
            {
                candidates.emplace_back(tr_strvPath(getHomeDir(), ".local"sv, "share"sv));
            }
            tr_free(tmp);

            /* XDG_DATA_DIRS are the backup directories */
            {
                char const* const pkg = PACKAGE_DATA_DIR;
                auto* xdg = tr_env_get_string("XDG_DATA_DIRS", "");
                auto const buf = tr_strvJoin(pkg, ":", xdg, ":/usr/local/share:/usr/share");
                tr_free(xdg);

                auto sv = std::string_view{ buf };
                auto token = std::string_view{};
                while (tr_strvSep(&sv, &token, ':'))
                {
                    token = tr_strvStrip(token);
                    if (!std::empty(token))
                    {
                        candidates.emplace_back(token);
                    }
                }
            }

            /* walk through the candidates & look for a match */
            for (auto const& dir : candidates)
            {
                auto const path = tr_strvPath(dir, "transmission"sv, "public_html"sv);
                if (isWebClientDir(path))
                {
                    s = tr_strvDup(path);
                    break;
                }
            }
#endif
        }
    }

    return s;
}

std::string tr_getSessionIdDir()
{
#ifndef _WIN32

    return std::string{ "/tmp"sv };

#else

    char* program_data_dir = win32_get_known_folder_ex(FOLDERID_ProgramData, KF_FLAG_CREATE);
    auto const result = tr_strvPath(program_data_dir, "Transmission");
    tr_free(program_data_dir);
    tr_sys_dir_create(result.c_str(), 0, 0, nullptr);
    return result;

#endif
}
