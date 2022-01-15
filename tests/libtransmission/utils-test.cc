/*
 * This file Copyright (C) 2013-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <string_view>

#ifdef _WIN32
#include <windows.h>
#define setenv(key, value, unused) SetEnvironmentVariableA(key, value)
#define unsetenv(key) SetEnvironmentVariableA(key, nullptr)
#endif

#include "transmission.h"

#include "crypto-utils.h" // tr_rand_int_weak()
#include "platform.h"
#include "ptrarray.h"
#include "utils.h"

#include "test-fixtures.h"

#include <algorithm>
#include <array>
#include <cmath> // sqrt()
#include <cstdlib> // setenv(), unsetenv()
#include <iostream>
#include <sstream>
#include <string>

using ::libtransmission::test::makeString;
using UtilsTest = ::testing::Test;
using namespace std::literals;

TEST_F(UtilsTest, trStripPositionalArgs)
{
    auto const* in = "Hello %1$s foo %2$.*f";
    auto const* expected = "Hello %s foo %.*f";
    auto const* out = tr_strip_positional_args(in);
    EXPECT_STREQ(expected, out);

    in = "Hello %1$'d foo %2$'f";
    expected = "Hello %d foo %f";
    out = tr_strip_positional_args(in);
    EXPECT_STREQ(expected, out);
}

TEST_F(UtilsTest, trStrvJoin)
{
    EXPECT_EQ(""sv, tr_strvJoin(""sv));
    EXPECT_EQ("test"sv, tr_strvJoin("test"sv));
    EXPECT_EQ("foo/bar"sv, tr_strvJoin("foo"sv, "/", std::string{ "bar" }));
    EXPECT_EQ("abcde"sv, tr_strvJoin("a", "b", "c", "d", "e"));
}

TEST_F(UtilsTest, trStrvContains)
{
    EXPECT_FALSE(tr_strvContains("a test is this"sv, "TEST"sv));
    EXPECT_FALSE(tr_strvContains("test"sv, "testt"sv));
    EXPECT_FALSE(tr_strvContains("test"sv, "this is a test"sv));
    EXPECT_TRUE(tr_strvContains(" test "sv, "tes"sv));
    EXPECT_TRUE(tr_strvContains(" test"sv, "test"sv));
    EXPECT_TRUE(tr_strvContains("a test is this"sv, "test"sv));
    EXPECT_TRUE(tr_strvContains("test "sv, "test"sv));
    EXPECT_TRUE(tr_strvContains("test"sv, ""sv));
    EXPECT_TRUE(tr_strvContains("test"sv, "t"sv));
    EXPECT_TRUE(tr_strvContains("test"sv, "te"sv));
    EXPECT_TRUE(tr_strvContains("test"sv, "test"sv));
    EXPECT_TRUE(tr_strvContains("this is a test"sv, "test"sv));
    EXPECT_TRUE(tr_strvContains(""sv, ""sv));
}

TEST_F(UtilsTest, trStrvStartsWith)
{
    EXPECT_FALSE(tr_strvStartsWith(""sv, "this is a string"sv));
    EXPECT_FALSE(tr_strvStartsWith("this is a strin"sv, "this is a string"sv));
    EXPECT_FALSE(tr_strvStartsWith("this is a strin"sv, "this is a string"sv));
    EXPECT_FALSE(tr_strvStartsWith("this is a string"sv, " his is a string"sv));
    EXPECT_FALSE(tr_strvStartsWith("this is a string"sv, "his is a string"sv));
    EXPECT_FALSE(tr_strvStartsWith("this is a string"sv, "string"sv));
    EXPECT_TRUE(tr_strvStartsWith(""sv, ""sv));
    EXPECT_TRUE(tr_strvStartsWith("this is a string"sv, ""sv));
    EXPECT_TRUE(tr_strvStartsWith("this is a string"sv, "this "sv));
    EXPECT_TRUE(tr_strvStartsWith("this is a string"sv, "this is"sv));
    EXPECT_TRUE(tr_strvStartsWith("this is a string"sv, "this"sv));
}

TEST_F(UtilsTest, trStrvEndsWith)
{
    EXPECT_FALSE(tr_strvEndsWith(""sv, "string"sv));
    EXPECT_FALSE(tr_strvEndsWith("this is a string"sv, "alphabet"sv));
    EXPECT_FALSE(tr_strvEndsWith("this is a string"sv, "strin"sv));
    EXPECT_FALSE(tr_strvEndsWith("this is a string"sv, "this is"sv));
    EXPECT_FALSE(tr_strvEndsWith("this is a string"sv, "this"sv));
    EXPECT_FALSE(tr_strvEndsWith("tring"sv, "string"sv));
    EXPECT_TRUE(tr_strvEndsWith(""sv, ""sv));
    EXPECT_TRUE(tr_strvEndsWith("this is a string"sv, " string"sv));
    EXPECT_TRUE(tr_strvEndsWith("this is a string"sv, ""sv));
    EXPECT_TRUE(tr_strvEndsWith("this is a string"sv, "a string"sv));
    EXPECT_TRUE(tr_strvEndsWith("this is a string"sv, "g"sv));
    EXPECT_TRUE(tr_strvEndsWith("this is a string"sv, "string"sv));
}

TEST_F(UtilsTest, trStrvSep)
{
    auto constexpr Delim = ',';

    auto sv = "token1,token2,token3"sv;
    EXPECT_EQ("token1"sv, tr_strvSep(&sv, Delim));
    EXPECT_EQ("token2"sv, tr_strvSep(&sv, Delim));
    EXPECT_EQ("token3"sv, tr_strvSep(&sv, Delim));
    EXPECT_EQ(""sv, tr_strvSep(&sv, Delim));

    sv = " token1,token2"sv;
    EXPECT_EQ(" token1"sv, tr_strvSep(&sv, Delim));
    EXPECT_EQ("token2"sv, tr_strvSep(&sv, Delim));

    sv = "token1;token2"sv;
    EXPECT_EQ("token1;token2"sv, tr_strvSep(&sv, Delim));
    EXPECT_EQ(""sv, tr_strvSep(&sv, Delim));

    sv = ""sv;
    EXPECT_EQ(""sv, tr_strvSep(&sv, Delim));
}

TEST_F(UtilsTest, trStrvStrip)
{
    EXPECT_EQ(""sv, tr_strvStrip("              "sv));
    EXPECT_EQ("test test"sv, tr_strvStrip("    test test     "sv));
    EXPECT_EQ("test"sv, tr_strvStrip("   test     "sv));
    EXPECT_EQ("test"sv, tr_strvStrip("   test "sv));
    EXPECT_EQ("test"sv, tr_strvStrip(" test       "sv));
    EXPECT_EQ("test"sv, tr_strvStrip(" test "sv));
    EXPECT_EQ("test"sv, tr_strvStrip("test"sv));
}

TEST_F(UtilsTest, trStrvDup)
{
    auto constexpr Key = "this is a test"sv;
    char* str = tr_strvDup(Key);
    EXPECT_NE(nullptr, str);
    EXPECT_EQ(Key, str);
    tr_free(str);
}

TEST_F(UtilsTest, trStrvPath)
{
    EXPECT_EQ("foo" TR_PATH_DELIMITER_STR "bar", tr_strvPath("foo", "bar"));
    EXPECT_EQ(TR_PATH_DELIMITER_STR "foo" TR_PATH_DELIMITER_STR "bar", tr_strvPath("", "foo", "bar"));

    EXPECT_EQ("", tr_strvPath(""sv));
    EXPECT_EQ("foo"sv, tr_strvPath("foo"sv));
    EXPECT_EQ(
        "foo" TR_PATH_DELIMITER_STR "bar" TR_PATH_DELIMITER_STR "baz" TR_PATH_DELIMITER_STR "mum"sv,
        tr_strvPath("foo"sv, "bar", std::string{ "baz" }, "mum"sv));
}

TEST_F(UtilsTest, trStrvUtf8Clean)
{
    auto in = "hello world"sv;
    auto out = std::string{};
    tr_strvUtf8Clean(in, out);
    EXPECT_EQ(in, out);

    in = "hello world"sv;
    tr_strvUtf8Clean(in.substr(0, 5), out);
    EXPECT_EQ("hello"sv, out);

    // this version is not utf-8 (but cp866)
    in = "\x92\xE0\xE3\xA4\xAD\xAE \xA1\xEB\xE2\xEC \x81\xAE\xA3\xAE\xAC"sv;
    tr_strvUtf8Clean(in, out);
    EXPECT_TRUE(std::size(out) == 17 || std::size(out) == 33);
    EXPECT_TRUE(tr_utf8_validate(out, nullptr));

    // same string, but utf-8 clean
    in = "Трудно быть Богом"sv;
    tr_strvUtf8Clean(in, out);
    EXPECT_NE(nullptr, out.data());
    EXPECT_TRUE(tr_utf8_validate(out, nullptr));
    EXPECT_EQ(in, out);

    // https://trac.transmissionbt.com/ticket/6064
    // TODO(anyone): It seems like that bug was not fixed so much as we just
    // let strlen() solve the problem for us; however, it's probably better
    // to wait until https://github.com/transmission/transmission/issues/612
    // is resolved before revisiting this.
    in = "\xF4\x00\x81\x82"sv;
    tr_strvUtf8Clean(in, out);
    EXPECT_NE(nullptr, out.data());
    EXPECT_TRUE(out.size() == 1 || out.size() == 2);
    EXPECT_TRUE(tr_utf8_validate(out, nullptr));

    in = "\xF4\x33\x81\x82"sv;
    tr_strvUtf8Clean(in, out);
    EXPECT_NE(nullptr, out.data());
    EXPECT_TRUE(out.size() == 4 || out.size() == 7);
    EXPECT_TRUE(tr_utf8_validate(out, nullptr));
}

TEST_F(UtilsTest, trParseNumberRange)
{
    auto const tostring = [](std::vector<int> const& v)
    {
        std::stringstream ss;
        for (auto const& i : v)
        {
            ss << i << ' ';
        }
        return ss.str();
    };

    auto numbers = tr_parseNumberRange("1-10,13,16-19"sv);
    EXPECT_EQ(std::string("1 2 3 4 5 6 7 8 9 10 13 16 17 18 19 "), tostring(numbers));

    numbers = tr_parseNumberRange("1-5,3-7,2-6"sv);
    EXPECT_EQ(std::string("1 2 3 4 5 6 7 "), tostring(numbers));

    numbers = tr_parseNumberRange("1-Hello"sv);
    auto const empty_string = std::string{};
    EXPECT_EQ(empty_string, tostring(numbers));

    numbers = tr_parseNumberRange("1-"sv);
    EXPECT_EQ(empty_string, tostring(numbers));

    numbers = tr_parseNumberRange("Hello"sv);
    EXPECT_EQ(empty_string, tostring(numbers));
}

namespace
{

int compareInts(void const* va, void const* vb) noexcept
{
    auto const a = *static_cast<int const*>(va);
    auto const b = *static_cast<int const*>(vb);
    return a - b;
}

} // namespace

TEST_F(UtilsTest, lowerbound)
{
    auto const a = std::array<int, 7>{ 1, 2, 3, 3, 3, 5, 8 };
    auto const expected_pos = std::array<int, 10>{ 0, 1, 2, 5, 5, 6, 6, 6, 7, 7 };
    auto const expected_exact = std::array<bool, 10>{ true, true, true, false, true, false, false, true, false, false };

    for (int i = 1; i <= 10; i++)
    {
        bool exact;
        auto const pos = tr_lowerBound(&i, a.data(), a.size(), sizeof(int), compareInts, &exact);
        EXPECT_EQ(expected_pos[i - 1], pos);
        EXPECT_EQ(expected_exact[i - 1], exact);
    }
}

TEST_F(UtilsTest, trStrlower)
{
    EXPECT_EQ(""sv, tr_strlower(""sv));
    EXPECT_EQ("apple"sv, tr_strlower("APPLE"sv));
    EXPECT_EQ("apple"sv, tr_strlower("Apple"sv));
    EXPECT_EQ("apple"sv, tr_strlower("aPPLe"sv));
    EXPECT_EQ("apple"sv, tr_strlower("applE"sv));
    EXPECT_EQ("hello"sv, tr_strlower("HELLO"sv));
    EXPECT_EQ("hello"sv, tr_strlower("hello"sv));
}

TEST_F(UtilsTest, trMemmem)
{
    auto const haystack = std::string{ "abcabcabcabc" };
    auto const needle = std::string{ "cab" };

    EXPECT_EQ(haystack, tr_memmem(haystack.data(), haystack.size(), haystack.data(), haystack.size()));
    EXPECT_EQ(haystack.substr(2), tr_memmem(haystack.data(), haystack.size(), needle.data(), needle.size()));
    EXPECT_EQ(nullptr, tr_memmem(needle.data(), needle.size(), haystack.data(), haystack.size()));
}

TEST_F(UtilsTest, array)
{
    auto array = std::array<size_t, 10>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    auto n = array.size();

    tr_removeElementFromArray(array.data(), 5U, sizeof(size_t), n);
    --n;

    for (size_t i = 0; i < n; ++i)
    {
        EXPECT_EQ(array[i], i < 5 ? i : i + 1);
    }

    tr_removeElementFromArray(array.data(), 0U, sizeof(size_t), n);
    --n;

    for (size_t i = 0; i < n; ++i)
    {
        EXPECT_EQ(array[i], i < 4 ? i + 1 : i + 2);
    }

    tr_removeElementFromArray(array.data(), n - 1, sizeof(size_t), n);
    --n;

    for (size_t i = 0; i < n; ++i)
    {
        EXPECT_EQ(array[i], i < 4 ? i + 1 : i + 2);
    }
}

TEST_F(UtilsTest, truncd)
{
    auto buf = std::array<char, 32>{};

    tr_snprintf(buf.data(), buf.size(), "%.2f%%", 99.999);
    EXPECT_STREQ("100.00%", buf.data());

    tr_snprintf(buf.data(), buf.size(), "%.2f%%", tr_truncd(99.999, 2));
    EXPECT_STREQ("99.99%", buf.data());

    tr_snprintf(buf.data(), buf.size(), "%.4f", tr_truncd(403650.656250, 4));
    EXPECT_STREQ("403650.6562", buf.data());

    tr_snprintf(buf.data(), buf.size(), "%.2f", tr_truncd(2.15, 2));
    EXPECT_STREQ("2.15", buf.data());

    tr_snprintf(buf.data(), buf.size(), "%.2f", tr_truncd(2.05, 2));
    EXPECT_STREQ("2.05", buf.data());

    tr_snprintf(buf.data(), buf.size(), "%.2f", tr_truncd(3.3333, 2));
    EXPECT_STREQ("3.33", buf.data());

    tr_snprintf(buf.data(), buf.size(), "%.0f", tr_truncd(3.3333, 0));
    EXPECT_STREQ("3", buf.data());

    tr_snprintf(buf.data(), buf.size(), "%.0f", tr_truncd(3.9999, 0));
    EXPECT_STREQ("3", buf.data());

#if !(defined(_MSC_VER) || (defined(__MINGW32__) && defined(__MSVCRT__)))
    /* FIXME: MSCVRT behaves differently in case of nan */
    auto const nan = sqrt(-1.0);
    tr_snprintf(buf.data(), buf.size(), "%.2f", tr_truncd(nan, 2));
    EXPECT_TRUE(strstr(buf.data(), "nan") != nullptr || strstr(buf.data(), "NaN") != nullptr);
#endif
}

TEST_F(UtilsTest, trStrdupPrintfFmtS)
{
    auto s = makeString(tr_strdup_printf("%s", "test"));
    EXPECT_EQ("test", s);
}

TEST_F(UtilsTest, trStrdupPrintf)
{
    auto s = makeString(tr_strdup_printf("%d %s %c %u", -1, "0", '1', 2));
    EXPECT_EQ("-1 0 1 2", s);

    auto* s3 = reinterpret_cast<char*>(tr_malloc0(4098));
    memset(s3, '-', 4097);
    s3[2047] = 't';
    s3[2048] = 'e';
    s3[2049] = 's';
    s3[2050] = 't';

    auto* s2 = reinterpret_cast<char*>(tr_malloc0(4096));
    memset(s2, '-', 4095);
    s2[2047] = '%';
    s2[2048] = 's';

    // NOLINTNEXTLINE(clang-diagnostic-format-nonliteral)
    s = makeString(tr_strdup_printf(s2, "test"));
    EXPECT_EQ(s3, s);

    tr_free(s2);

    s = makeString(tr_strdup_printf("%s", s3));
    EXPECT_EQ(s3, s);

    tr_free(s3);
}

TEST_F(UtilsTest, env)
{
    char const* test_key = "TR_TEST_ENV";

    unsetenv(test_key);

    EXPECT_FALSE(tr_env_key_exists(test_key));
    EXPECT_EQ(123, tr_env_get_int(test_key, 123));
    EXPECT_EQ(nullptr, tr_env_get_string(test_key, nullptr));
    auto s = makeString(tr_env_get_string(test_key, "a"));
    EXPECT_EQ("a", s);

    setenv(test_key, "", 1);

    EXPECT_TRUE(tr_env_key_exists(test_key));
    EXPECT_EQ(456, tr_env_get_int(test_key, 456));
    s = makeString(tr_env_get_string(test_key, nullptr));
    EXPECT_EQ("", s);
    s = makeString(tr_env_get_string(test_key, "b"));
    EXPECT_EQ("", s);

    setenv(test_key, "135", 1);

    EXPECT_TRUE(tr_env_key_exists(test_key));
    EXPECT_EQ(135, tr_env_get_int(test_key, 789));
    s = makeString(tr_env_get_string(test_key, nullptr));
    EXPECT_EQ("135", s);
    s = makeString(tr_env_get_string(test_key, "c"));
    EXPECT_EQ("135", s);
}

TEST_F(UtilsTest, mimeTypes)
{
    EXPECT_EQ("audio/x-flac"sv, tr_get_mime_type_for_filename("music.flac"sv));
    EXPECT_EQ("audio/x-flac"sv, tr_get_mime_type_for_filename("music.FLAC"sv));
    EXPECT_EQ("video/x-msvideo"sv, tr_get_mime_type_for_filename(".avi"sv));
    EXPECT_EQ("video/x-msvideo"sv, tr_get_mime_type_for_filename("/path/to/FILENAME.AVI"sv));
    EXPECT_EQ("application/octet-stream"sv, tr_get_mime_type_for_filename("music.ajoijfeisfe"sv));
}

TEST_F(UtilsTest, saveFile)
{
    // save a file to GoogleTest's temp dir
    auto filename = tr_strvJoin(::testing::TempDir(), "filename.txt");
    auto contents = "these are the contents"sv;
    tr_error* error = nullptr;
    EXPECT_TRUE(tr_saveFile(filename, contents, &error));
    EXPECT_EQ(nullptr, error);

    // now read the file back in and confirm the contents are the same
    auto buf = std::vector<char>{};
    EXPECT_TRUE(tr_loadFile(buf, filename, &error));
    EXPECT_EQ(nullptr, error);
    auto sv = std::string_view{ std::data(buf), std::size(buf) };
    EXPECT_EQ(contents, sv);

    // remove the tempfile
    EXPECT_TRUE(tr_sys_path_remove(filename.c_str(), &error));
    EXPECT_EQ(nullptr, error);

    // try saving a file to a path that doesn't exist
    filename = "/this/path/does/not/exist/foo.txt";
    EXPECT_FALSE(tr_saveFile(filename, contents, &error));
    ASSERT_NE(nullptr, error);
    EXPECT_NE(0, error->code);
    tr_error_clear(&error);
}
