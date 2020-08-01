/*
 * This file Copyright (C) 2013-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <algorithm>
#include <cmath> // sqrt()
#include <cstdlib> // setenv(), unsetenv()

#ifdef _WIN32
#include <windows.h>
#define setenv(key, value, unused) SetEnvironmentVariableA(key, value)
#define unsetenv(key) SetEnvironmentVariableA(key, nullptr)
#endif

#include "transmission.h"
#include "ConvertUTF.h" // tr_utf8_validate()
#include "platform.h"
#include "crypto-utils.h" // tr_rand_int_weak()
#include "utils.h"
#include "web.h" // tr_http_unescape()

#include "test-fixtures.h"

using ::libtransmission::test::make_string;

using UtilsTest = ::testing::Test;

TEST_F(UtilsTest, tr_strip_positional_args)
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

TEST_F(UtilsTest, tr_strstrip)
{
    auto* in = tr_strdup("   test    ");
    auto* out = tr_strstrip(in);
    EXPECT_EQ(in, out);
    EXPECT_STREQ("test", out);
    tr_free(in);

    in = tr_strdup(" test test ");
    out = tr_strstrip(in);
    EXPECT_EQ(in, out);
    EXPECT_STREQ("test test", out);
    tr_free(in);

    /* strstrip */
    in = tr_strdup("test");
    out = tr_strstrip(in);
    EXPECT_EQ(in, out);
    EXPECT_STREQ("test", out);
    tr_free(in);
}

TEST_F(UtilsTest, tr_strjoin)
{
    char const* in1[] = { "one", "two" };
    auto out = make_string(tr_strjoin(in1, 2, ", "));
    EXPECT_EQ("one, two", out);

    char const* in2[] = { "hello" };
    out = make_string(tr_strjoin(in2, 1, "###"));
    EXPECT_EQ("hello", out);

    char const* in3[] = { "a", "b", "ccc", "d", "eeeee" };
    out = make_string(tr_strjoin(in3, 5, " "));
    EXPECT_EQ("a b ccc d eeeee", out);

    char const* in4[] = { "7", "ate", "9" };
    out = make_string(tr_strjoin(in4, 3, ""));
    EXPECT_EQ("7ate9", out);

    char const** in5;
    out = make_string(tr_strjoin(in5, 0, "a"));
    EXPECT_EQ("", out);
}

TEST_F(UtilsTest, tr_buildPath)
{
    auto out = make_string(tr_buildPath("foo", "bar", nullptr));
    EXPECT_EQ("foo" TR_PATH_DELIMITER_STR "bar", out);

    out = make_string(tr_buildPath("", "foo", "bar", nullptr));
    EXPECT_EQ(TR_PATH_DELIMITER_STR "foo" TR_PATH_DELIMITER_STR "bar", out);
}

TEST_F(UtilsTest, tr_utf8clean)
{
    auto const* in = "hello world";
    auto out = make_string(tr_utf8clean(in, TR_BAD_SIZE));
    EXPECT_EQ(in, out);

    in = "hello world";
    out = make_string(tr_utf8clean(in, 5));
    EXPECT_EQ("hello", out);

    // this version is not utf-8 (but cp866)
    in = "\x92\xE0\xE3\xA4\xAD\xAE \xA1\xEB\xE2\xEC \x81\xAE\xA3\xAE\xAC";
    out = make_string(tr_utf8clean(in, 17));
    EXPECT_TRUE(std::size(out)== 17 || std::size(out)== 33);
    EXPECT_TRUE(tr_utf8_validate(std::data(out), std::size(out), nullptr));

    // same string, but utf-8 clean
    in = "Трудно быть Богом";
    out = make_string(tr_utf8clean(in, TR_BAD_SIZE));
    EXPECT_NE(nullptr, std::data(out));
    EXPECT_TRUE(tr_utf8_validate(std::data(out), std::size(out), nullptr));
    EXPECT_EQ(in, out);

    in = "\xF4\x00\x81\x82";
    out = make_string(tr_utf8clean(in, 4));
    EXPECT_NE(nullptr, std::data(out));
    EXPECT_TRUE(std::size(out) == 1 || std::size(out) == 2);
    EXPECT_TRUE(tr_utf8_validate(std::data(out), std::size(out), nullptr));

    in = "\xF4\x33\x81\x82";
    out = make_string(tr_utf8clean(in, 4));
    EXPECT_NE(nullptr, std::data(out));
    EXPECT_TRUE(std::size(out) == 4 || std::size(out) == 7);
    EXPECT_TRUE(tr_utf8_validate(std::data(out), std::size(out), nullptr));
}

TEST_F(UtilsTest, numbers)
{
    auto count = int {};
    auto* numbers = tr_parseNumberRange("1-10,13,16-19", TR_BAD_SIZE, &count);
    EXPECT_EQ(15, count);
    EXPECT_EQ(1, numbers[0]);
    EXPECT_EQ(6, numbers[5]);
    EXPECT_EQ(10, numbers[9]);
    EXPECT_EQ(13, numbers[10]);
    EXPECT_EQ(16, numbers[11]);
    EXPECT_EQ(19, numbers[14]);
    tr_free(numbers);

    numbers = tr_parseNumberRange("1-5,3-7,2-6", TR_BAD_SIZE, &count);
    EXPECT_EQ(7, count);
    EXPECT_NE(nullptr, numbers);
    for (int i = 0; i < count; ++i)
    {
        EXPECT_EQ(i + 1, numbers[i]);
    }
    tr_free(numbers);

    numbers = tr_parseNumberRange("1-Hello", TR_BAD_SIZE, &count);
    EXPECT_EQ(0, count);
    EXPECT_EQ(nullptr, numbers);

    numbers = tr_parseNumberRange("1-", TR_BAD_SIZE, &count);
    EXPECT_EQ(0, count);
    EXPECT_EQ(nullptr, numbers);

    numbers = tr_parseNumberRange("Hello", TR_BAD_SIZE, &count);
    EXPECT_EQ(0, count);
    EXPECT_EQ(nullptr, numbers);
}

namespace
{

int compareInts(void const* va, void const* vb)
{
    int const a = *(int const*)va;
    int const b = *(int const*)vb;
    return a - b;
}

}

TEST_F(UtilsTest, lowerbound)
{
    auto constexpr A = std::array<int, 7> { 1, 2, 3, 3, 3, 5, 8 };
    int const expected_pos[] = { 0, 1, 2, 5, 5, 6, 6, 6, 7, 7 };
    bool const expected_exact[] = { true, true, true, false, true, false, false, true, false, false };

    for (int i = 1; i <= 10; i++)
    {
        bool exact;
        auto const pos = tr_lowerBound(&i, std::data(A), std::size(A), sizeof(int), compareInts, &exact);
        EXPECT_EQ(expected_pos[i - 1], pos);
        EXPECT_EQ(expected_exact[i - 1], exact);
    }
}

TEST_F(UtilsTest, tr_quickfindFirstK)
{
    auto constexpr run_test = [](size_t const k, size_t const n, int* buf, int range) {
        // populate buf with random ints
        std::generate(buf, buf+n, [range](){return tr_rand_int_weak(range);});

        // find the best k
        tr_quickfindFirstK(buf, n, sizeof(int), compareInts, k);

        // confirm that the smallest K ints are in the first slots K slots in buf
        auto const highest_low = std::max_element(buf, buf+k);
        auto const lowest_high = std::min_element(buf+k, buf+n);
        EXPECT_LE(highest_low, lowest_high);
    };

    auto constexpr K = size_t { 10 };
    auto constexpr NumTrials = size_t { 1000 };
    auto buf = std::array<int,100>{};
    for (auto i = 0; i != NumTrials; ++i)
    {
        run_test(K, std::size(buf), std::data(buf), 100);
    }
}

TEST_F(UtilsTest, tr_memmem)
{
    auto constexpr Haystack = std::string_view { "abcabcabcabc" };
    auto constexpr Needle = std::string_view { "cab" };

    EXPECT_EQ(Haystack, tr_memmem(std::data(Haystack), std::size(Haystack), std::data(Haystack), std::size(Haystack)));
    EXPECT_EQ(Haystack.substr(2), tr_memmem(std::data(Haystack), std::size(Haystack), std::data(Needle), std::size(Needle)));
    EXPECT_EQ(nullptr, tr_memmem(std::data(Needle), std::size(Needle), std::data(Haystack), std::size(Haystack)));
}

TEST_F(UtilsTest, tr_binary_hex)
{
    auto constexpr HexIn = std::string_view { "fb5ef5507427b17e04b69cef31fa3379b456735a" };

    uint8_t binary[20];
    tr_hex_to_binary(std::begin(HexIn), binary, std::size(HexIn)/2);

    char hex_out[41];
    tr_binary_to_hex(binary, hex_out, 20);
    EXPECT_EQ(HexIn, std::string_view(hex_out));
}

TEST_F(UtilsTest, array)
{
    size_t array[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    size_t n = TR_N_ELEMENTS(array);

    tr_removeElementFromArray(array, 5U, sizeof(size_t), n);
    --n;

    for (size_t i = 0; i < n; ++i)
    {
        EXPECT_EQ(array[i], i < 5 ? i : i + 1);
    }

    tr_removeElementFromArray(array, 0U, sizeof(size_t), n);
    --n;

    for (size_t i = 0; i < n; ++i)
    {
        EXPECT_EQ(array[i], i < 4 ? i + 1 : i + 2);
    }

    tr_removeElementFromArray(array, n - 1, sizeof(size_t), n);
    --n;

    for (size_t i = 0; i < n; ++i)
    {
        EXPECT_EQ(array[i], i < 4 ? i + 1 : i + 2);
    }
}

TEST_F(UtilsTest, url)
{
    auto const* url = "http://1";
    int port;
    char* scheme;
    char* host;
    char* path;
    EXPECT_TRUE(tr_urlParse(url, TR_BAD_SIZE, &scheme, &host, &port, &path));
    EXPECT_STREQ("http", scheme);
    EXPECT_STREQ("1", host);
    EXPECT_STREQ("/", path);
    EXPECT_EQ(80, port);
    tr_free(scheme);
    tr_free(path);
    tr_free(host);

    url = "http://www.some-tracker.org/some/path";
    EXPECT_TRUE(tr_urlParse(url, TR_BAD_SIZE, &scheme, &host, &port, &path));
    EXPECT_STREQ("http", scheme);
    EXPECT_STREQ("www.some-tracker.org", host);
    EXPECT_STREQ("/some/path", path);
    EXPECT_EQ(80, port);
    tr_free(scheme);
    tr_free(path);
    tr_free(host);

    url = "http://www.some-tracker.org:8080/some/path";
    EXPECT_TRUE(tr_urlParse(url, TR_BAD_SIZE, &scheme, &host, &port, &path));
    EXPECT_STREQ("http", scheme);
    EXPECT_STREQ("www.some-tracker.org", host);
    EXPECT_STREQ("/some/path", path);
    EXPECT_EQ(8080, port);
    tr_free(scheme);
    tr_free(path);
    tr_free(host);
}

TEST_F(UtilsTest, tr_http_unescape)
{
    auto constexpr Url = std::string_view { "http%3A%2F%2Fwww.example.com%2F~user%2F%3Ftest%3D1%26test1%3D2" };
    auto str = make_string(tr_http_unescape(std::data(Url), std::size(Url)));
    EXPECT_EQ("http://www.example.com/~user/?test=1&test1=2", str);
}

TEST_F(UtilsTest, truncd)
{
    char buf[32];

    tr_snprintf(buf, sizeof(buf), "%.2f%%", 99.999);
    EXPECT_STREQ("100.00%", buf);

    tr_snprintf(buf, sizeof(buf), "%.2f%%", tr_truncd(99.999, 2));
    EXPECT_STREQ("99.99%", buf);

    tr_snprintf(buf, sizeof(buf), "%.4f", tr_truncd(403650.656250, 4));
    EXPECT_STREQ("403650.6562", buf);

    tr_snprintf(buf, sizeof(buf), "%.2f", tr_truncd(2.15, 2));
    EXPECT_STREQ("2.15", buf);

    tr_snprintf(buf, sizeof(buf), "%.2f", tr_truncd(2.05, 2));
    EXPECT_STREQ("2.05", buf);

    tr_snprintf(buf, sizeof(buf), "%.2f", tr_truncd(3.3333, 2));
    EXPECT_STREQ("3.33", buf);

    tr_snprintf(buf, sizeof(buf), "%.0f", tr_truncd(3.3333, 0));
    EXPECT_STREQ("3", buf);

    tr_snprintf(buf, sizeof(buf), "%.0f", tr_truncd(3.9999, 0));
    EXPECT_STREQ("3", buf);

#if !(defined(_MSC_VER) || (defined(__MINGW32__) && defined(__MSVCRT__)))
    /* FIXME: MSCVRT behaves differently in case of nan */
    auto const nan = sqrt(-1);
    tr_snprintf(buf, sizeof(buf), "%.2f", tr_truncd(nan, 2));
    EXPECT_TRUE(strstr(buf, "nan") != nullptr || strstr(buf, "NaN") != nullptr);
#endif
}

TEST_F(UtilsTest, tr_strdup_vprintf)
{
    auto constexpr test_strdup_printf_valist = [](char const* fmt, ...){
        va_list args;
        va_start(args, fmt);
        auto* ret = tr_strdup_vprintf(fmt, args);
        va_end(args);
        return ret;
    };

    auto s = make_string(test_strdup_printf_valist("\n-%s-%s-%s-\n", "\r", "\t", "\b"));
    EXPECT_EQ("\n-\r-\t-\b-\n", s);
}

TEST_F(UtilsTest, tr_strdup_printf_fmt_s)
{
    auto s = make_string(tr_strdup_printf("%s", "test"));
    EXPECT_EQ("test", s);
}

TEST_F(UtilsTest, tr_strdup_printf)
{
    auto s = make_string(tr_strdup_printf("%d %s %c %u", -1, "0", '1', 2));
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

    s = make_string(tr_strdup_printf(s2, "test"));
    EXPECT_EQ(s3, s);

    tr_free(s2);

    s = make_string(tr_strdup_printf("%s", s3));
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
    auto s = make_string(tr_env_get_string(test_key, "a"));
    EXPECT_EQ("a", s);

    setenv(test_key, "", 1);

    EXPECT_TRUE(tr_env_key_exists(test_key));
    EXPECT_EQ(456, tr_env_get_int(test_key, 456));
    s = make_string(tr_env_get_string(test_key, nullptr));
    EXPECT_EQ("", s);
    s = make_string(tr_env_get_string(test_key, "b"));
    EXPECT_EQ("", s);

    setenv(test_key, "135", 1);

    EXPECT_TRUE(tr_env_key_exists(test_key));
    EXPECT_EQ(135, tr_env_get_int(test_key, 789));
    s = make_string(tr_env_get_string(test_key, nullptr));
    EXPECT_EQ("135", s);
    s = make_string(tr_env_get_string(test_key, "c"));
    EXPECT_EQ("135", s);
}
