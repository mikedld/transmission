// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cassert>
#include <cstdint> // uint64_t
#include <cstdio> // stderr
#include <cstring>
#include <ctime> // time()
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <fmt/core.h>

#include <libtransmission/error.h>
#include <libtransmission/file.h>
#include <libtransmission/tr-macros.h>
#include <libtransmission/tr-strbuf.h>

#include "gtest/gtest.h"
#include "test-fixtures.h"

#if !defined(__OpenBSD__)
#define HAVE_UNIFIED_BUFFER_CACHE
#endif

#ifndef _WIN32
#define NATIVE_PATH_SEP "/"
#else
#define NATIVE_PATH_SEP "\\"
#endif

using namespace std::literals;

namespace libtransmission::test
{

class FileTest : public SessionTest
{
protected:
    auto createTestDir(std::string const& child_name)
    {
        auto test_dir = tr_pathbuf{ session_->configDir(), '/', child_name };
        tr_sys_dir_create(test_dir, 0, 0777);
        return test_dir;
    }

    static bool createSymlink(char const* dst_path, char const* src_path, [[maybe_unused]] bool dst_is_dir)
    {
#ifndef _WIN32

        return symlink(src_path, dst_path) != -1;

#else
        auto const wide_src_path = tr_win32_utf8_to_native(src_path);
        auto const wide_dst_path = tr_win32_utf8_to_native(dst_path);
        return CreateSymbolicLinkW(wide_dst_path.c_str(), wide_src_path.c_str(), dst_is_dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);

#endif
    }

    static bool createHardlink(char const* dst_path, char const* src_path)
    {
#ifndef _WIN32

        return link(src_path, dst_path) != -1;

#else

        auto const wide_src_path = tr_win32_utf8_to_native(src_path);
        auto const wide_dst_path = tr_win32_utf8_to_native(dst_path);
        return CreateHardLinkW(wide_dst_path.c_str(), wide_src_path.c_str(), nullptr);

#endif
    }

    static void clearPathInfo(tr_sys_path_info* info)
    {
        *info = {};
    }

    static bool pathContainsNoSymlinks(char const* path)
    {
        char const* p = path;

        while (*p != '\0')
        {
            char const* slash_pos = strchr(p, '/');

#ifdef _WIN32

            char const* backslash_pos = strchr(p, '\\');

            if (slash_pos == nullptr || (backslash_pos != nullptr && backslash_pos < slash_pos))
            {
                slash_pos = backslash_pos;
            }

#endif

            if (slash_pos == nullptr)
            {
                slash_pos = p + strlen(p) - 1;
            }

            auto const path_part = std::string{ path, static_cast<size_t>(slash_pos - path + 1) };
            auto const info = tr_sys_path_get_info(path_part, TR_SYS_PATH_NO_FOLLOW);
            if (!info || (!info->isFile() && !info->isFolder()))
            {
                return false;
            }

            p = slash_pos + 1;
        }

        return true;
    }

    static bool validatePermissions([[maybe_unused]] char const* path, [[maybe_unused]] unsigned int permissions)
    {
#ifndef _WIN32

        struct stat sb = {};
        return stat(path, &sb) != -1 && (sb.st_mode & 0777) == permissions;

#else

        /* No UNIX permissions on Windows */
        return true;

#endif
    }

    struct XnameTestData
    {
        std::string_view input;
        std::string_view output;
    };

    static void testPathXname(
        XnameTestData const* data,
        size_t data_size,
        std::string_view (*func)(std::string_view, tr_error*))
    {
        for (size_t i = 0; i < data_size; ++i)
        {
            auto error = tr_error{};
            auto const& [input, output] = data[i];
            auto const name = func(input, &error);

            if (!std::empty(data[i].output))
            {
                EXPECT_NE(""sv, name);
                EXPECT_FALSE(error) << error;
                EXPECT_EQ(output, name) << " in [" << input << ']';
            }
            else
            {
                EXPECT_EQ(""sv, name) << " in [" << input << ']';
                EXPECT_TRUE(error);
            }
        }
    }

    static void testDirReadImpl(tr_pathbuf const& path, bool* have1, bool* have2)
    {
        *have1 = *have2 = false;

        auto err = tr_error{};
        auto dd = tr_sys_dir_open(path, &err);
        EXPECT_NE(TR_BAD_SYS_DIR, dd);
        EXPECT_FALSE(err) << err;

        for (;;)
        {
            char const* name = tr_sys_dir_read_name(dd, &err);
            if (name == nullptr)
            {
                break;
            }

            EXPECT_FALSE(err) << err;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            {
                continue;
            }

            if (strcmp(name, "a") == 0)
            {
                *have1 = true;
            }
            else if (strcmp(name, "b") == 0)
            {
                *have2 = true;
            }
            else
            {
                FAIL();
            }
        }

        EXPECT_FALSE(err) << err;

        EXPECT_TRUE(tr_sys_dir_close(dd, &err));
        EXPECT_FALSE(err) << err;
    }
};

TEST_F(FileTest, getInfo)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto const path2 = tr_pathbuf{ test_dir, "/b"sv };

    // Can't get info of non-existent file/directory
    auto err = tr_error{};
    auto info = tr_sys_path_get_info(path1, 0, &err);
    EXPECT_FALSE(info.has_value());
    EXPECT_TRUE(err);
    err = {};

    auto t = time(nullptr);
    createFileWithContents(path1, "test");

    // Good file info
    info = tr_sys_path_get_info(path1, 0, &err);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(TR_SYS_PATH_IS_FILE, info->type);
    EXPECT_EQ(4U, info->size);
    EXPECT_GE(info->last_modified_at, t - 1);
    EXPECT_LE(info->last_modified_at, time(nullptr) + 1);

    tr_sys_path_remove(path1);

    // Good directory info
    t = time(nullptr);
    tr_sys_dir_create(path1, 0, 0777);
    info = tr_sys_path_get_info(path1, 0, &err);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(TR_SYS_PATH_IS_DIRECTORY, info->type);
    EXPECT_NE(uint64_t(-1), info->size);
    EXPECT_GE(info->last_modified_at, t - 1);
    EXPECT_LE(info->last_modified_at, time(nullptr) + 1);
    tr_sys_path_remove(path1);

    if (createSymlink(path1, path2, false))
    {
        // Can't get info of non-existent file/directory
        info = tr_sys_path_get_info(path1, 0, &err);
        EXPECT_FALSE(info.has_value());
        EXPECT_TRUE(err);
        err = {};

        t = time(nullptr);
        createFileWithContents(path2, "test");

        // Good file info
        info = tr_sys_path_get_info(path1, 0, &err);
        EXPECT_TRUE(info.has_value());
        assert(info.has_value());
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(TR_SYS_PATH_IS_FILE, info->type);
        EXPECT_EQ(4, info->size);
        EXPECT_GE(info->last_modified_at, t - 1);
        EXPECT_LE(info->last_modified_at, time(nullptr) + 1);

        // Symlink
        info = tr_sys_path_get_info(path1, TR_SYS_PATH_NO_FOLLOW, &err);
        EXPECT_TRUE(info.has_value());
        assert(info.has_value());
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(TR_SYS_PATH_IS_OTHER, info->type);

        tr_sys_path_remove(path2);
        tr_sys_path_remove(path1);

        // Good directory info
        t = time(nullptr);
        tr_sys_dir_create(path2, 0, 0777);
        EXPECT_TRUE(createSymlink(path1, path2, true)); /* Win32: directory and file symlinks differ :( */
        info = tr_sys_path_get_info(path1, 0, &err);
        EXPECT_TRUE(info.has_value());
        assert(info.has_value());
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(TR_SYS_PATH_IS_DIRECTORY, info->type);
        EXPECT_NE(uint64_t(-1), info->size);
        EXPECT_GE(info->last_modified_at, t - 1);
        EXPECT_LE(info->last_modified_at, time(nullptr) + 1);

        tr_sys_path_remove(path2);
        tr_sys_path_remove(path1);
    }
    else
    {
        fmt::print(stderr, "WARNING: [{:s}] unable to run symlink tests\n", __FUNCTION__);
    }
}

TEST_F(FileTest, readFile)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path = tr_pathbuf{ test_dir, "/a.txt"sv };
    auto constexpr Contents = "hello, world!"sv;
    createFileWithContents(path, Contents);

    auto n_read = uint64_t{};
    auto buf = std::array<char, 64>{};
    auto fd = tr_sys_file_open(path, TR_SYS_FILE_READ, 0);
    EXPECT_NE(TR_BAD_SYS_FILE, fd);

    // successful read
    auto error = tr_error{};
    EXPECT_TRUE(tr_sys_file_read(fd, std::data(buf), std::size(buf), &n_read, &error));
    EXPECT_EQ(Contents, std::string_view(std::data(buf), n_read));
    EXPECT_EQ(std::size(Contents), n_read);
    EXPECT_FALSE(error) << error;

    // successful read_at
    auto const offset = 1U;
    EXPECT_TRUE(tr_sys_file_read_at(fd, std::data(buf), std::size(buf), offset, &n_read, &error));
    auto constexpr Expected = Contents.substr(offset);
    EXPECT_EQ(Expected, std::string_view(std::data(buf), n_read));
    EXPECT_EQ(std::size(Expected), n_read);
    EXPECT_FALSE(error) << error;

    tr_sys_file_close(fd);
}

TEST_F(FileTest, pathExists)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto const path2 = tr_pathbuf{ test_dir, "/b"sv };

    // Non-existent file does not exist
    auto error = tr_error{};
    EXPECT_FALSE(tr_sys_path_exists(path1, &error));
    EXPECT_FALSE(error) << error;

    // Create file and see that it exists
    createFileWithContents(path1, "test");
    EXPECT_TRUE(tr_sys_path_exists(path1, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_path_remove(path1);

    // Create directory and see that it exists
    tr_sys_dir_create(path1, 0, 0777);
    EXPECT_TRUE(tr_sys_path_exists(path1, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_path_remove(path1);

    if (createSymlink(path1, path2, false))
    {
        // Non-existent file does not exist (via symlink)
        EXPECT_FALSE(tr_sys_path_exists(path1, &error));
        EXPECT_FALSE(error) << error;

        // Create file and see that it exists (via symlink)
        createFileWithContents(path2, "test");
        EXPECT_TRUE(tr_sys_path_exists(path1, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path2);
        tr_sys_path_remove(path1);

        /* Create directory and see that it exists (via symlink) */
        tr_sys_dir_create(path2, 0, 0777);
        EXPECT_TRUE(createSymlink(path1, path2, true)); /* Win32: directory and file symlinks differ :( */
        EXPECT_TRUE(tr_sys_path_exists(path1, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path2);
        tr_sys_path_remove(path1);
    }
    else
    {
        fmt::print(stderr, "WARNING: [{:s}] unable to run symlink tests\n", __FUNCTION__);
    }
}

TEST_F(FileTest, pathIsRelative)
{
    EXPECT_TRUE(tr_sys_path_is_relative(""));
    EXPECT_TRUE(tr_sys_path_is_relative("."));
    EXPECT_TRUE(tr_sys_path_is_relative(".."));
    EXPECT_TRUE(tr_sys_path_is_relative("x"));
    EXPECT_TRUE(tr_sys_path_is_relative("\\"));
    EXPECT_TRUE(tr_sys_path_is_relative(":"));

#ifdef _WIN32

    EXPECT_TRUE(tr_sys_path_is_relative("/"));
    EXPECT_TRUE(tr_sys_path_is_relative("\\x"));
    EXPECT_TRUE(tr_sys_path_is_relative("/x"));
    EXPECT_TRUE(tr_sys_path_is_relative("\\x\\y"));
    EXPECT_TRUE(tr_sys_path_is_relative("/x/y"));
    EXPECT_TRUE(tr_sys_path_is_relative("C:x"));
    EXPECT_TRUE(tr_sys_path_is_relative("C:x\\y"));
    EXPECT_TRUE(tr_sys_path_is_relative("C:x/y"));

    EXPECT_FALSE(tr_sys_path_is_relative("\\\\"));
    EXPECT_FALSE(tr_sys_path_is_relative("//"));
    EXPECT_FALSE(tr_sys_path_is_relative("\\\\x"));
    EXPECT_FALSE(tr_sys_path_is_relative("//x"));
    EXPECT_FALSE(tr_sys_path_is_relative("\\\\x\\y"));
    EXPECT_FALSE(tr_sys_path_is_relative("//x/y"));
    EXPECT_FALSE(tr_sys_path_is_relative("\\\\.\\x"));
    EXPECT_FALSE(tr_sys_path_is_relative("//./x"));

    EXPECT_FALSE(tr_sys_path_is_relative("a:"));
    EXPECT_FALSE(tr_sys_path_is_relative("a:\\"));
    EXPECT_FALSE(tr_sys_path_is_relative("a:/"));
    EXPECT_FALSE(tr_sys_path_is_relative("Z:"));
    EXPECT_FALSE(tr_sys_path_is_relative("Z:\\"));
    EXPECT_FALSE(tr_sys_path_is_relative("Z:/"));

#else /* _WIN32 */

    EXPECT_FALSE(tr_sys_path_is_relative("/"));
    EXPECT_FALSE(tr_sys_path_is_relative("/x"));
    EXPECT_FALSE(tr_sys_path_is_relative("/x/y"));
    EXPECT_FALSE(tr_sys_path_is_relative("//x"));

#endif /* _WIN32 */
}

TEST_F(FileTest, pathIsSame)
{
    // NOLINTBEGIN(readability-suspicious-call-argument)

    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto const path2 = tr_pathbuf{ test_dir, "/b"sv };
    auto path3 = tr_pathbuf{ path2, "/c"sv };

    /* Two non-existent files are not the same */
    auto error = tr_error{};
    EXPECT_FALSE(tr_sys_path_is_same(path1, path1, &error));
    EXPECT_FALSE(error) << error;
    EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
    EXPECT_FALSE(error) << error;

    /* Two same files are the same */
    createFileWithContents(path1, "test");
    EXPECT_TRUE(tr_sys_path_is_same(path1, path1, &error));
    EXPECT_FALSE(error) << error;

    /* Existent and non-existent files are not the same */
    EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
    EXPECT_FALSE(error) << error;
    EXPECT_FALSE(tr_sys_path_is_same(path2, path1, &error));
    EXPECT_FALSE(error) << error;

    /* Two separate files (even with same content) are not the same */
    createFileWithContents(path2, "test");
    EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_path_remove(path1);

    /* Two same directories are the same */
    tr_sys_dir_create(path1, 0, 0777);
    EXPECT_TRUE(tr_sys_path_is_same(path1, path1, &error));
    EXPECT_FALSE(error) << error;

    /* File and directory are not the same */
    EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
    EXPECT_FALSE(error) << error;
    EXPECT_FALSE(tr_sys_path_is_same(path2, path1, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_path_remove(path2);

    /* Two separate directories are not the same */
    tr_sys_dir_create(path2, 0, 0777);
    EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_path_remove(path1);
    tr_sys_path_remove(path2);

    if (createSymlink(path1, ".", true))
    {
        /* Directory and symlink pointing to it are the same */
        EXPECT_TRUE(tr_sys_path_is_same(path1, test_dir.data(), &error));
        EXPECT_FALSE(error) << error;
        EXPECT_TRUE(tr_sys_path_is_same(test_dir.data(), path1, &error));
        EXPECT_FALSE(error) << error;

        /* Non-existent file and symlink are not the same */
        EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_is_same(path2, path1, &error));
        EXPECT_FALSE(error) << error;

        /* Symlinks pointing to different directories are not the same */
        createSymlink(path2, "..", true);
        EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_is_same(path2, path1, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path2);

        /* Symlinks pointing to same directory are the same */
        createSymlink(path2, ".", true);
        EXPECT_TRUE(tr_sys_path_is_same(path1, path2, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path2);

        /* Directory and symlink pointing to another directory are not the same */
        tr_sys_dir_create(path2, 0, 0777);
        EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_is_same(path2, path1, &error));
        EXPECT_FALSE(error) << error;

        /* Symlinks pointing to same directory are the same */
        createSymlink(path3, "..", true);
        EXPECT_TRUE(tr_sys_path_is_same(path1, path3, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path1);

        /* File and symlink pointing to directory are not the same */
        createFileWithContents(path1, "test");
        EXPECT_FALSE(tr_sys_path_is_same(path1, path3, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_is_same(path3, path1, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path3);

        /* File and symlink pointing to same file are the same */
        createSymlink(path3, path1, false);
        EXPECT_TRUE(tr_sys_path_is_same(path1, path3, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_TRUE(tr_sys_path_is_same(path3, path1, &error));
        EXPECT_FALSE(error) << error;

        /* Symlinks pointing to non-existent files are not the same */
        tr_sys_path_remove(path1);
        createSymlink(path1, "missing", false);
        tr_sys_path_remove(path3);
        createSymlink(path3, "missing", false);
        EXPECT_FALSE(tr_sys_path_is_same(path1, path3, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_is_same(path3, path1, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path3);

        /* Symlinks pointing to same non-existent file are not the same */
        createSymlink(path3.c_str(), ".." NATIVE_PATH_SEP "missing", false);
        EXPECT_FALSE(tr_sys_path_is_same(path1, path3, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_is_same(path3, path1, &error));
        EXPECT_FALSE(error) << error;

        /* Non-existent file and symlink pointing to non-existent file are not the same */
        tr_sys_path_remove(path3);
        EXPECT_FALSE(tr_sys_path_is_same(path1, path3, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_is_same(path3, path1, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path2);
        tr_sys_path_remove(path1);
    }
    else
    {
        fmt::print(stderr, "WARNING: [{:s}] unable to run symlink tests\n", __FUNCTION__);
    }

    path3.assign(test_dir, "/c"sv);

    createFileWithContents(path1, "test");

    if (createHardlink(path2, path1))
    {
        /* File and hardlink to it are the same */
        EXPECT_TRUE(tr_sys_path_is_same(path1, path2, &error));
        EXPECT_FALSE(error) << error;

        /* Two hardlinks to the same file are the same */
        createHardlink(path3, path2);
        EXPECT_TRUE(tr_sys_path_is_same(path2, path3, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_TRUE(tr_sys_path_is_same(path1, path3, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path2);

        EXPECT_TRUE(tr_sys_path_is_same(path1, path3, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path3);

        /* File and hardlink to another file are not the same */
        createFileWithContents(path3, "test");
        createHardlink(path2, path3);
        EXPECT_FALSE(tr_sys_path_is_same(path1, path2, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_is_same(path2, path1, &error));
        EXPECT_FALSE(error) << error;

        tr_sys_path_remove(path3);
        tr_sys_path_remove(path2);
    }
    else
    {
        fmt::print(stderr, "WARNING: [{:s}] unable to run symlink tests\n", __FUNCTION__);
    }

    if (createSymlink(path2, path1, false) && createHardlink(path3, path1))
    {
        EXPECT_TRUE(tr_sys_path_is_same(path2, path3, &error));
        EXPECT_FALSE(error) << error;
    }
    else
    {
        fmt::print(stderr, "WARNING: [{:s}] unable to run combined symlink and hardlink tests\n", __FUNCTION__);
    }

    tr_sys_path_remove(path3);
    tr_sys_path_remove(path2);
    tr_sys_path_remove(path1);

    // NOLINTEND(readability-suspicious-call-argument)
}

TEST_F(FileTest, pathResolve)
{
    auto const test_dir = createTestDir(currentTestName());

    auto error = tr_error{};
    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto const path2 = tr_pathbuf{ test_dir, "/b"sv };

    createFileWithContents(path1, "test");

    if (createSymlink(path2, path1, false))
    {
        auto resolved = tr_sys_path_resolve(path2, &error);
        EXPECT_FALSE(error) << error;
        EXPECT_TRUE(pathContainsNoSymlinks(resolved.c_str()));

        tr_sys_path_remove(path2);
        tr_sys_path_remove(path1);

        tr_sys_dir_create(path1, 0, 0755);
        EXPECT_TRUE(createSymlink(path2, path1, true)); /* Win32: directory and file symlinks differ :( */
        resolved = tr_sys_path_resolve(path2, &error);
        EXPECT_FALSE(error) << error;
        EXPECT_TRUE(pathContainsNoSymlinks(resolved.c_str()));
    }
    else
    {
        fmt::print(stderr, "WARNING: [{:s}] unable to run symlink tests\n", __FUNCTION__);
    }

    tr_sys_path_remove(path2);
    tr_sys_path_remove(path1);

#ifdef _WIN32

    auto resolved = tr_sys_path_resolve("\\\\127.0.0.1\\NonExistent"sv, &error);
    EXPECT_EQ(""sv, resolved);
    EXPECT_TRUE(error);
    error = {};

    resolved = tr_sys_path_resolve("\\\\127.0.0.1\\ADMIN$\\NonExistent"sv, &error);
    EXPECT_EQ(""sv, resolved);
    EXPECT_TRUE(error);
    error = {};

    for (auto const& input : { "\\\\127.0.0.1\\ADMIN$\\System32"sv,
                               "\\\\127.0.0.1\\ADMIN$\\\\System32"sv,
                               "\\\\127.0.0.1\\\\ADMIN$\\System32"sv,
                               "\\\\127.0.0.1\\\\ADMIN$\\\\System32"sv,
                               "\\\\127.0.0.1\\ADMIN$/System32"sv })
    {
        resolved = tr_sys_path_resolve(input, &error);
        EXPECT_EQ("\\\\127.0.0.1\\ADMIN$\\System32"sv, resolved);
        EXPECT_FALSE(error) << error;
    }

#endif
}

TEST_F(FileTest, pathBasename)
{
    auto const common_xname_tests = std::vector<XnameTestData>{
        XnameTestData{ "/", "/" },
        { "", "." },
#ifdef _WIN32
        { "\\", "/" },
        /* Invalid paths */
        { "\\\\\\", "" },
        { "123:", "" },
        /* Reserved characters */
        { "<", "" },
        { ">", "" },
        { ":", "" },
        { "\"", "" },
        { "|", "" },
        { "?", "" },
        { "*", "" },
        { "a\\<", "" },
        { "a\\>", "" },
        { "a\\:", "" },
        { "a\\\"", "" },
        { "a\\|", "" },
        { "a\\?", "" },
        { "a\\*", "" },
        { "c:\\a\\b<c\\d", "" },
        { "c:\\a\\b>c\\d", "" },
        { "c:\\a\\b:c\\d", "" },
        { "c:\\a\\b\"c\\d", "" },
        { "c:\\a\\b|c\\d", "" },
        { "c:\\a\\b?c\\d", "" },
        { "c:\\a\\b*c\\d", "" },
#else
        { "////", "/" },
#endif
    };

    testPathXname(common_xname_tests.data(), common_xname_tests.size(), tr_sys_path_basename);
    // testPathXname(common_xname_tests.data(), common_xname_tests.size(), tr_sys_path_dirname);

    auto const basename_tests = std::vector<XnameTestData>{
        XnameTestData{ "a", "a" },
        { "aa", "aa" },
        { "/aa", "aa" },
        { "/a/b/c", "c" },
        { "/a/b/c/", "c" },
#ifdef _WIN32
        { "c:\\a\\b\\c", "c" },
        { "c:", "/" },
        { "c:/", "/" },
        { "c:\\", "/" },
        { "c:a/b", "b" },
        { "c:a", "a" },
        { "\\\\a\\b\\c", "c" },
        { "//a/b", "b" },
        { "//1.2.3.4/b", "b" },
        { "\\\\a", "a" },
        { "\\\\1.2.3.4", "1.2.3.4" },
        { "\\", "/" },
        { "\\a", "a" },
#endif
    };

    testPathXname(basename_tests.data(), basename_tests.size(), tr_sys_path_basename);
}

TEST_F(FileTest, pathDirname)
{
#ifdef _WIN32
    static auto constexpr DirnameTests = std::array<std::pair<std::string_view, std::string_view>, 48>{ {
        { "C:\\a/b\\c"sv, "C:\\a/b"sv },
        { "C:\\a/b\\c\\"sv, "C:\\a/b"sv },
        { "C:\\a/b"sv, "C:\\a"sv },
        { "C:/a"sv, "C:/"sv },
        { "C:"sv, "C:"sv },
        { "C:/"sv, "C:/"sv },
        { "c:a/b"sv, "c:a"sv },
        { "c:a"sv, "c:"sv },
        { "\\\\a"sv, "\\"sv },
        { "\\\\1.2.3.4"sv, "\\"sv },
        { "\\\\"sv, "\\"sv },
        { "a/b\\c"sv, "a/b"sv },
        // taken from Node.js unit tests
        // https://github.com/nodejs/node/blob/e46c680bf2b211bbd52cf959ca17ee98c7f657f5/test/parallel/test-path-dirname.js
        { "c:\\"sv, "c:\\"sv },
        { "c:\\foo"sv, "c:\\"sv },
        { "c:\\foo\\"sv, "c:\\"sv },
        { "c:\\foo\\bar"sv, "c:\\foo"sv },
        { "c:\\foo\\bar\\"sv, "c:\\foo"sv },
        { "c:\\foo\\bar\\baz"sv, "c:\\foo\\bar"sv },
        { "c:\\foo bar\\baz"sv, "c:\\foo bar"sv },
        { "\\"sv, "\\"sv },
        { "\\foo"sv, "\\"sv },
        { "\\foo\\"sv, "\\"sv },
        { "\\foo\\bar"sv, "\\foo"sv },
        { "\\foo\\bar\\"sv, "\\foo"sv },
        { "\\foo\\bar\\baz"sv, "\\foo\\bar"sv },
        { "\\foo bar\\baz"sv, "\\foo bar"sv },
        { "c:"sv, "c:"sv },
        { "c:foo"sv, "c:"sv },
        { "c:foo\\"sv, "c:"sv },
        { "c:foo\\bar"sv, "c:foo"sv },
        { "c:foo\\bar\\"sv, "c:foo"sv },
        { "c:foo\\bar\\baz"sv, "c:foo\\bar"sv },
        { "c:foo bar\\baz"sv, "c:foo bar"sv },
        { "file:stream"sv, "."sv },
        { "dir\\file:stream"sv, "dir"sv },
        { "\\\\unc\\share"sv, "\\\\unc\\share"sv },
        { "\\\\unc\\share\\foo"sv, "\\\\unc\\share\\"sv },
        { "\\\\unc\\share\\foo\\"sv, "\\\\unc\\share\\"sv },
        { "\\\\unc\\share\\foo\\bar"sv, "\\\\unc\\share\\foo"sv },
        { "\\\\unc\\share\\foo\\bar\\"sv, "\\\\unc\\share\\foo"sv },
        { "\\\\unc\\share\\foo\\bar\\baz"sv, "\\\\unc\\share\\foo\\bar"sv },
        { "/a/b/"sv, "/a"sv },
        { "/a/b"sv, "/a"sv },
        { "/a"sv, "/"sv },
        { ""sv, "."sv },
        { "/"sv, "/"sv },
        { "////"sv, "/"sv },
        { "foo"sv, "."sv },
    } };
#else
    static auto constexpr DirnameTests = std::array<std::pair<std::string_view, std::string_view>, 15>{ {
        // taken from Node.js unit tests
        // https://github.com/nodejs/node/blob/e46c680bf2b211bbd52cf959ca17ee98c7f657f5/test/parallel/test-path-dirname.js
        { "/a/b/"sv, "/a"sv },
        { "/a/b"sv, "/a"sv },
        { "/a"sv, "/"sv },
        { ""sv, "."sv },
        { "/"sv, "/"sv },
        { "////"sv, "/"sv },
        { "//a"sv, "//"sv },
        { "foo"sv, "."sv },
        // taken from dirname(3) manpage
        { "usr"sv, "."sv },
        { "/usr/lib", "/usr"sv },
        { "/usr/"sv, "/"sv },
        { "/usr/"sv, "/"sv },
        { "/"sv, "/"sv },
        { "."sv, "."sv },
        { ".."sv, "."sv },
    } };
#endif

    for (auto const& [input, expected] : DirnameTests)
    {
        EXPECT_EQ(expected, tr_sys_path_dirname(input))
            << "input[" << input << "] expected [" << expected << "] actual [" << tr_sys_path_dirname(input) << "]\n";

        auto path = tr_pathbuf{ input };
        path.popdir();
        EXPECT_EQ(expected, path) << "input[" << input << "] expected [" << expected << "] actual [" << path << "]\n";
    }

    /* TODO: is_same(dirname(x) + '/' + basename(x), x) */
}

TEST_F(FileTest, pathRename)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto const path2 = tr_pathbuf{ test_dir, "/b"sv };
    auto path3 = tr_pathbuf{ path2, "/c"sv };

    createFileWithContents(path1, "test");

    /* Preconditions */
    EXPECT_TRUE(tr_sys_path_exists(path1));
    EXPECT_FALSE(tr_sys_path_exists(path2));

    /* Forward rename works */
    auto error = tr_error{};
    EXPECT_TRUE(tr_sys_path_rename(path1, path2, &error));
    EXPECT_FALSE(tr_sys_path_exists(path1));
    EXPECT_TRUE(tr_sys_path_exists(path2));
    EXPECT_FALSE(error) << error;

    /* Backward rename works */
    EXPECT_TRUE(tr_sys_path_rename(path2, path1, &error));
    EXPECT_TRUE(tr_sys_path_exists(path1));
    EXPECT_FALSE(tr_sys_path_exists(path2));
    EXPECT_FALSE(error) << error;

    /* Another backward rename [of non-existent file] does not work */
    EXPECT_FALSE(tr_sys_path_rename(path2, path1, &error));
    EXPECT_TRUE(error);
    error = {};

    /* Rename to file which couldn't be created does not work */
    EXPECT_FALSE(tr_sys_path_rename(path1, path3, &error));
    EXPECT_TRUE(error);
    error = {};

    /* Rename of non-existent file does not work */
    EXPECT_FALSE(tr_sys_path_rename(path3, path2, &error));
    EXPECT_TRUE(error);
    error = {};

    createFileWithContents(path2, "test");

    /* Renaming file does overwrite existing file */
    EXPECT_TRUE(tr_sys_path_rename(path2, path1, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_dir_create(path2, 0, 0777);

    /* Renaming file does not overwrite existing directory, and vice versa */
    EXPECT_FALSE(tr_sys_path_rename(path1, path2, &error));
    EXPECT_TRUE(error);
    error = {};
    EXPECT_FALSE(tr_sys_path_rename(path2, path1, &error));
    EXPECT_TRUE(error);
    error = {};

    tr_sys_path_remove(path2);

    path3.assign(test_dir, "/c"sv);

    if (createSymlink(path2, path1, false))
    {
        /* Preconditions */
        EXPECT_TRUE(tr_sys_path_exists(path2));
        EXPECT_FALSE(tr_sys_path_exists(path3));
        EXPECT_TRUE(tr_sys_path_is_same(path1, path2));

        /* Rename of symlink works, files stay the same */
        EXPECT_TRUE(tr_sys_path_rename(path2, path3, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_exists(path2));
        EXPECT_TRUE(tr_sys_path_exists(path3));
        EXPECT_TRUE(tr_sys_path_is_same(path1, path3));

        tr_sys_path_remove(path3);
    }
    else
    {
        fmt::print(stderr, "WARNING: [{:s}] unable to run symlink tests\n", __FUNCTION__);
    }

    if (createHardlink(path2, path1))
    {
        /* Preconditions */
        EXPECT_TRUE(tr_sys_path_exists(path2));
        EXPECT_FALSE(tr_sys_path_exists(path3));
        EXPECT_TRUE(tr_sys_path_is_same(path1, path2));

        /* Rename of hardlink works, files stay the same */
        EXPECT_TRUE(tr_sys_path_rename(path2, path3, &error));
        EXPECT_FALSE(error) << error;
        EXPECT_FALSE(tr_sys_path_exists(path2));
        EXPECT_TRUE(tr_sys_path_exists(path3));
        EXPECT_TRUE(tr_sys_path_is_same(path1, path3));

        tr_sys_path_remove(path3);
    }
    else
    {
        fmt::print(stderr, "WARNING: [{:s}] unable to run hardlink tests\n", __FUNCTION__);
    }

    tr_sys_path_remove(path1);
}

TEST_F(FileTest, pathRemove)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto const path2 = tr_pathbuf{ test_dir, "/b"sv };
    auto const path3 = tr_pathbuf{ path2, "/c"sv };

    /* Can't remove non-existent file/directory */
    EXPECT_FALSE(tr_sys_path_exists(path1));
    auto error = tr_error{};
    EXPECT_FALSE(tr_sys_path_remove(path1, &error));
    EXPECT_TRUE(error);
    EXPECT_FALSE(tr_sys_path_exists(path1));
    error = {};

    /* Removing file works */
    createFileWithContents(path1, "test");
    EXPECT_TRUE(tr_sys_path_exists(path1));
    EXPECT_TRUE(tr_sys_path_remove(path1, &error));
    EXPECT_FALSE(error) << error;
    EXPECT_FALSE(tr_sys_path_exists(path1));

    /* Removing empty directory works */
    tr_sys_dir_create(path1, 0, 0777);
    EXPECT_TRUE(tr_sys_path_exists(path1));
    EXPECT_TRUE(tr_sys_path_remove(path1, &error));
    EXPECT_FALSE(error) << error;
    EXPECT_FALSE(tr_sys_path_exists(path1));

    /* Removing non-empty directory fails */
    tr_sys_dir_create(path2, 0, 0777);
    createFileWithContents(path3, "test");
    EXPECT_TRUE(tr_sys_path_exists(path2));
    EXPECT_TRUE(tr_sys_path_exists(path3));
    EXPECT_FALSE(tr_sys_path_remove(path2, &error));
    EXPECT_TRUE(error);
    EXPECT_TRUE(tr_sys_path_exists(path2));
    EXPECT_TRUE(tr_sys_path_exists(path3));
    error = {};

    tr_sys_path_remove(path3);
    tr_sys_path_remove(path2);
}

TEST_F(FileTest, pathNativeSeparators)
{
    EXPECT_EQ(nullptr, tr_sys_path_native_separators(nullptr));

    static auto constexpr Tests = std::array<std::pair<std::string_view, std::string_view>, 5>{ {
        { "", "" },
        { "a", TR_IF_WIN32("a", "a") },
        { "/", TR_IF_WIN32("\\", "/") },
        { "/a/b/c", TR_IF_WIN32("\\a\\b\\c", "/a/b/c") },
        { "C:\\a/b\\c", TR_IF_WIN32("C:\\a\\b\\c", "C:\\a/b\\c") },
    } };

    for (auto const& [input, expected] : Tests)
    {
        auto buf = tr_pathbuf{ input };
        EXPECT_EQ(expected, tr_sys_path_native_separators(std::data(buf)));
    }
}

TEST_F(FileTest, fileCopy)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a.txt" };
    auto const path2 = tr_pathbuf{ test_dir, "/b.txt" };
    auto constexpr Contents = "hello, world!"sv;

    // no source file
    auto error = tr_error{};
    EXPECT_FALSE(tr_sys_path_copy(path1, path2, &error));
    EXPECT_TRUE(error);
    error = {};

    createFileWithContents(path1, Contents);

    // source file exists but is inaccessible
    (void)chmod(path1, 0);
    EXPECT_FALSE(tr_sys_path_copy(path1, test_dir, &error));
    EXPECT_TRUE(error);
    error = {};
    (void)chmod(path1, 0600);

    // source file exists but target is invalid
    EXPECT_FALSE(tr_sys_path_copy(path1, test_dir, &error));
    EXPECT_TRUE(error);
    error = {};

    // source and target are valid
    createFileWithContents(path1, Contents);
    EXPECT_TRUE(tr_sys_path_copy(path1, path2, &error));
    EXPECT_FALSE(error) << error;
}

TEST_F(FileTest, fileOpen)
{
    auto const test_dir = createTestDir(currentTestName());

    // can't open non-existent file
    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    EXPECT_FALSE(tr_sys_path_exists(path1));
    auto error = tr_error{};
    EXPECT_EQ(TR_BAD_SYS_FILE, tr_sys_file_open(path1, TR_SYS_FILE_READ, 0600, &error));
    EXPECT_TRUE(error);
    EXPECT_FALSE(tr_sys_path_exists(path1));
    error = {};
    EXPECT_EQ(TR_BAD_SYS_FILE, tr_sys_file_open(path1, TR_SYS_FILE_WRITE, 0600, &error));
    EXPECT_TRUE(error);
    EXPECT_FALSE(tr_sys_path_exists(path1));
    error = {};

    // can't open directory
    tr_sys_dir_create(path1, 0, 0777);
#ifdef _WIN32
    // this works on *NIX
    EXPECT_EQ(TR_BAD_SYS_FILE, tr_sys_file_open(path1, TR_SYS_FILE_READ, 0600, &error));
    EXPECT_TRUE(error);
    error = {};
#endif
    EXPECT_EQ(TR_BAD_SYS_FILE, tr_sys_file_open(path1, TR_SYS_FILE_WRITE, 0600, &error));
    EXPECT_TRUE(error);
    error = {};

    tr_sys_path_remove(path1);

    // can create non-existent file
    auto fd = tr_sys_file_open(path1, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0640, &error);
    EXPECT_NE(TR_BAD_SYS_FILE, fd);
    EXPECT_FALSE(error) << error;
    tr_sys_file_close(fd);
    EXPECT_TRUE(tr_sys_path_exists(path1));
    EXPECT_TRUE(validatePermissions(path1, 0640));

    // can open existing file
    EXPECT_TRUE(tr_sys_path_exists(path1));
    fd = tr_sys_file_open(path1, TR_SYS_FILE_READ, 0600, &error);
    EXPECT_NE(TR_BAD_SYS_FILE, fd);
    EXPECT_FALSE(error) << error;
    tr_sys_file_close(fd);
    fd = tr_sys_file_open(path1, TR_SYS_FILE_WRITE, 0600, &error);
    EXPECT_NE(TR_BAD_SYS_FILE, fd);
    EXPECT_FALSE(error) << error;
    tr_sys_file_close(fd);

    tr_sys_path_remove(path1);
    createFileWithContents(path1, "test");

    /* File gets truncated */
    auto info = tr_sys_path_get_info(path1, TR_SYS_PATH_NO_FOLLOW);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_EQ(4U, info->size);
    fd = tr_sys_file_open(path1, TR_SYS_FILE_WRITE | TR_SYS_FILE_TRUNCATE, 0600, &error);
    EXPECT_NE(TR_BAD_SYS_FILE, fd);
    EXPECT_FALSE(error) << error;
    info = tr_sys_path_get_info(path1);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_EQ(0U, info->size);
    tr_sys_file_close(fd);
    info = tr_sys_path_get_info(path1, TR_SYS_PATH_NO_FOLLOW);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_EQ(0U, info->size);

    /* TODO: symlink and hardlink tests */

    tr_sys_path_remove(path1);
}

TEST_F(FileTest, fileTruncate)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path = tr_pathbuf{ test_dir, "/a"sv };
    auto fd = tr_sys_file_open(path, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0600);

    auto error = tr_error{};
    EXPECT_TRUE(tr_sys_file_truncate(fd, 10, &error));
    EXPECT_FALSE(error) << error;
    auto info = tr_sys_path_get_info(path);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_EQ(10U, info->size);

    EXPECT_TRUE(tr_sys_file_truncate(fd, 20, &error));
    EXPECT_FALSE(error) << error;
    info = tr_sys_path_get_info(path);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_EQ(20U, info->size);

    EXPECT_TRUE(tr_sys_file_truncate(fd, 0, &error));
    EXPECT_FALSE(error) << error;
    info = tr_sys_path_get_info(path);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_EQ(0U, info->size);

    EXPECT_TRUE(tr_sys_file_truncate(fd, 50, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_file_close(fd);

    info = tr_sys_path_get_info(path);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_EQ(50U, info->size);

    fd = tr_sys_file_open(path, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0600);

    EXPECT_TRUE(tr_sys_file_truncate(fd, 25, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_file_close(fd);

    info = tr_sys_path_get_info(path);
    EXPECT_TRUE(info.has_value());
    assert(info.has_value());
    EXPECT_EQ(25U, info->size);

    // try to truncate a closed file
    EXPECT_FALSE(tr_sys_file_truncate(fd, 10, &error)); // coverity[USE_AFTER_FREE]
    EXPECT_TRUE(error);
    error = {};

    tr_sys_path_remove(path);
}

TEST_F(FileTest, filePreallocate)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto fd = tr_sys_file_open(path1, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0600);

    auto error = tr_error{};
    auto prealloc_size = size_t{ 50 };
    if (tr_sys_file_preallocate(fd, prealloc_size, 0, &error))
    {
        EXPECT_FALSE(error) << error;
        auto info = tr_sys_path_get_info(path1);
        EXPECT_TRUE(info.has_value());
        assert(info.has_value());
        EXPECT_EQ(prealloc_size, info->size);
    }
    else
    {
        EXPECT_TRUE(error);
        fmt::print(
            stderr,
            "WARNING: [{:s}] unable to preallocate file (full): {:s} ({:d})\n",
            __FUNCTION__,
            error.message(),
            error.code());
        error = {};
    }

    tr_sys_file_close(fd);

    tr_sys_path_remove(path1);

    fd = tr_sys_file_open(path1, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0600);

    prealloc_size = size_t{ 500U } * 1024U * 1024U;
    if (tr_sys_file_preallocate(fd, prealloc_size, TR_SYS_FILE_PREALLOC_SPARSE, &error))
    {
        EXPECT_FALSE(error) << error;
        auto info = tr_sys_path_get_info(path1);
        EXPECT_TRUE(info.has_value());
        assert(info.has_value());
        EXPECT_EQ(prealloc_size, info->size);
    }
    else
    {
        EXPECT_TRUE(error);
        fmt::print(
            stderr,
            "WARNING: [{:s}] unable to preallocate file (sparse): {:s} ({:d})\n",
            __FUNCTION__,
            error.message(),
            error.code());
        error = {};
    }

    tr_sys_file_close(fd);

    tr_sys_path_remove(path1);
}

TEST_F(FileTest, dirCreate)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto const path2 = tr_pathbuf{ path1, "/b"sv };

    // Can create directory which has parent
    auto error = tr_error{};
    EXPECT_TRUE(tr_sys_dir_create(path1, 0, 0700, &error));
    EXPECT_FALSE(error) << error;
    EXPECT_TRUE(tr_sys_path_exists(path1));
    EXPECT_TRUE(validatePermissions(path1, 0700));

    tr_sys_path_remove(path1);
    createFileWithContents(path1, "test");

    // Can't create directory where file already exists
    EXPECT_FALSE(tr_sys_dir_create(path1, 0, 0700, &error));
    EXPECT_TRUE(error);
    error = {};
    EXPECT_FALSE(tr_sys_dir_create(path1, TR_SYS_DIR_CREATE_PARENTS, 0700, &error));
    EXPECT_TRUE(error);
    error = {};

    tr_sys_path_remove(path1);

    // Can't create directory which has no parent
    EXPECT_FALSE(tr_sys_dir_create(path2, 0, 0700, &error));
    EXPECT_TRUE(error);
    EXPECT_FALSE(tr_sys_path_exists(path2));
    error = {};

    // Can create directory with parent directories
    EXPECT_TRUE(tr_sys_dir_create(path2, TR_SYS_DIR_CREATE_PARENTS, 0751, &error));
    EXPECT_FALSE(error) << error;
    EXPECT_TRUE(tr_sys_path_exists(path1));
    EXPECT_TRUE(tr_sys_path_exists(path2));
    EXPECT_TRUE(validatePermissions(path1, 0751));
    EXPECT_TRUE(validatePermissions(path2, 0751));

    // Can create existing directory (no-op)
    EXPECT_TRUE(tr_sys_dir_create(path1, 0, 0700, &error));
    EXPECT_FALSE(error) << error;
    EXPECT_TRUE(tr_sys_dir_create(path1, TR_SYS_DIR_CREATE_PARENTS, 0700, &error));
    EXPECT_FALSE(error) << error;

    tr_sys_path_remove(path2);
    tr_sys_path_remove(path1);
}

TEST_F(FileTest, dirCreateTemp)
{
    auto const test_dir = createTestDir(currentTestName());

    auto error = tr_error{};
    auto path = tr_pathbuf{ test_dir, "/test-XXXXXX" };
    EXPECT_TRUE(tr_sys_dir_create_temp(std::data(path), &error));
    EXPECT_FALSE(error) << error;
    tr_sys_path_remove(path);

    path.assign(test_dir, "/path-does-not-exist/test-XXXXXX");
    EXPECT_FALSE(tr_sys_dir_create_temp(std::data(path), &error));
    EXPECT_TRUE(error);
}

TEST_F(FileTest, dirRead)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const path1 = tr_pathbuf{ test_dir, "/a"sv };
    auto const path2 = tr_pathbuf{ test_dir, "/b"sv };

    auto have1 = bool{};
    auto have2 = bool{};
    testDirReadImpl(test_dir, &have1, &have2);
    EXPECT_FALSE(have1);
    EXPECT_FALSE(have2);

    createFileWithContents(path1, "test");
    testDirReadImpl(test_dir, &have1, &have2);
    EXPECT_TRUE(have1);
    EXPECT_FALSE(have2);

    createFileWithContents(path2, "test");
    testDirReadImpl(test_dir, &have1, &have2);
    EXPECT_TRUE(have1);
    EXPECT_TRUE(have2);

    tr_sys_path_remove(path1);
    testDirReadImpl(test_dir, &have1, &have2);
    EXPECT_FALSE(have1);
    EXPECT_TRUE(have2);
}

TEST_F(FileTest, dirOpen)
{
    auto const test_dir = createTestDir(currentTestName());

    auto const file = tr_pathbuf{ test_dir, "/foo.fxt" };
    auto constexpr Contents = "hello, world!"sv;
    createFileWithContents(file, std::data(Contents), std::size(Contents));

    // path does not exist
    auto err = tr_error{};
    auto odir = tr_sys_dir_open("/no/such/path", &err);
    EXPECT_EQ(TR_BAD_SYS_DIR, odir);
    EXPECT_TRUE(err);
    err = {};

    // path is not a directory
    odir = tr_sys_dir_open(file, &err);
    EXPECT_EQ(TR_BAD_SYS_DIR, odir);
    EXPECT_TRUE(err);
    err = {};

    // path exists and is readable
    odir = tr_sys_dir_open(test_dir, &err);
    EXPECT_NE(TR_BAD_SYS_DIR, odir);
    EXPECT_FALSE(err);
    auto files = std::set<std::string>{};
    for (;;)
    {
        char const* const filename = tr_sys_dir_read_name(odir, &err);
        if (filename == nullptr)
        {
            break;
        }
        files.insert(filename);
    }
    EXPECT_EQ(3U, std::size(files));
    EXPECT_FALSE(err) << err;
    EXPECT_TRUE(tr_sys_dir_close(odir, &err));
    EXPECT_FALSE(err) << err;
}

} // namespace libtransmission::test
