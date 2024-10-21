// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#define LIBTRANSMISSION_VARIANT_MODULE

#include <cstdint> // int64_t
#include <locale>
#include <optional>
#include <string>
#include <string_view>

#include <libtransmission/quark.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>

#include "gtest/gtest.h"

using namespace std::literals;

class JSONTest : public ::testing::TestWithParam<char const*>
{
protected:
    void SetUp() override
    {
        ::testing::TestWithParam<char const*>::SetUp();

        auto const* locale_str = GetParam();
        old_locale_ = tr_locale_set_global(locale_str);
        if (!old_locale_)
        {
            GTEST_SKIP();
        }
    }

    void TearDown() override
    {
        if (old_locale_)
        {
            tr_locale_set_global(*old_locale_);
        }

        ::testing::TestWithParam<char const*>::TearDown();
    }

private:
    std::optional<std::locale> old_locale_;
};

TEST_P(JSONTest, testElements)
{
    static auto constexpr In = std::string_view{
        "{ \"string\": \"hello world\","
        "  \"escaped\": \"bell \\b formfeed \\f linefeed \\n carriage return \\r tab \\t\","
        "  \"int\": 5, "
        "  \"float\": 6.5, "
        "  \"true\": true, "
        "  \"false\": false, "
        "  \"null\": null }"
    };

    // Same as In, just formatted differently
    static auto constexpr Out = std::string_view{
        // clang-format off
        "{"
            "\"escaped\":\"bell \\b formfeed \\f linefeed \\n carriage return \\r tab \\t\","
            "\"false\":false,"
            "\"float\":6.5,"
            "\"int\":5,"
            "\"null\":null,"
            "\"string\":\"hello world\","
            "\"true\":true"
        "}"
        // clang-format on
    };

    auto serde = tr_variant_serde::json().inplace().compact();
    auto const var = serde.parse(In).value_or(tr_variant{});
    auto const* const map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);

    auto sv = map->value_if<std::string_view>(tr_quark_new("string"sv));
    ASSERT_TRUE(sv);
    EXPECT_EQ("hello world"sv, *sv);

    sv = map->value_if<std::string_view>(tr_quark_new("escaped"sv));
    ASSERT_TRUE(sv);
    EXPECT_EQ("bell \b formfeed \f linefeed \n carriage return \r tab \t"sv, *sv);

    auto i = map->value_if<int64_t>(tr_quark_new("int"sv));
    ASSERT_TRUE(i);
    EXPECT_EQ(5, *i);

    auto d = map->value_if<double>(tr_quark_new("float"sv));
    ASSERT_TRUE(d);
    EXPECT_EQ(65, int(*d * 10));

    auto b = map->value_if<bool>(tr_quark_new("true"sv));
    ASSERT_TRUE(b);
    EXPECT_TRUE(*b);

    b = map->value_if<bool>(tr_quark_new("false"sv));
    ASSERT_TRUE(b);
    EXPECT_FALSE(*b);

    auto n = map->value_if<std::nullptr_t>(tr_quark_new("null"sv));
    EXPECT_TRUE(n);

    EXPECT_EQ(serde.to_string(var), Out);
}

TEST_P(JSONTest, testUtf8)
{
    auto in = "{ \"key\": \"Letöltések\" }"sv;
    tr_quark const key = tr_quark_new("key"sv);

    auto serde = tr_variant_serde::json().inplace().compact();
    auto var = serde.parse(in).value_or(tr_variant{});
    auto* map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    auto sv = map->value_if<std::string_view>(key);
    ASSERT_TRUE(sv);
    EXPECT_EQ("Letöltések"sv, *sv);
    var.clear();

    in = R"({ "key": "\u005C" })"sv;
    var = serde.parse(in).value_or(tr_variant{});
    map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    sv = map->value_if<std::string_view>(key);
    ASSERT_TRUE(sv);
    EXPECT_EQ("\\"sv, *sv);
    var.clear();

    /**
     * 1. Feed it JSON-escaped nonascii to the JSON decoder.
     * 2. Confirm that the result is UTF-8.
     * 3. Feed the same UTF-8 back into the JSON encoder.
     * 4. Confirm that the result is UTF-8.
     * 5. Dogfood that result back into the parser.
     * 6. Confirm that the result is UTF-8.
     */
    in = R"({ "key": "Let\u00f6lt\u00e9sek" })"sv;
    var = serde.parse(in).value_or(tr_variant{});
    map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    sv = map->value_if<std::string_view>(key);
    ASSERT_TRUE(sv);
    EXPECT_EQ("Letöltések"sv, *sv);
    auto json = serde.to_string(var);
    var.clear();

    EXPECT_FALSE(std::empty(json));
    EXPECT_EQ(R"({"key":"Letöltések"})"sv, json);
    var = serde.parse(json).value_or(tr_variant{});
    map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    sv = map->value_if<std::string_view>(key);
    ASSERT_TRUE(sv);
    EXPECT_EQ("Letöltések"sv, *sv);

    // Test string known to be prone to locale issues
    // https://github.com/transmission/transmission/issues/5967
    var.clear();
    var = tr_variant::make_map(1U);
    map = var.get_if<tr_variant::Map>();
    map->try_emplace(key, "Дыскаграфія"sv);
    json = serde.to_string(var);
    EXPECT_EQ(R"({"key":"Дыскаграфія"})"sv, json);
    var = serde.parse(json).value_or(tr_variant{});
    map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    sv = map->value_if<std::string_view>(key);
    ASSERT_TRUE(sv);
    EXPECT_EQ("Дыскаграфія"sv, *sv);

    // Thinking emoji 🤔
    var.clear();
    var = tr_variant::make_map(1U);
    map = var.get_if<tr_variant::Map>();
    map->try_emplace(key, "\xf0\x9f\xa4\x94"sv);
    json = serde.to_string(var);
    EXPECT_EQ("{\"key\":\"\xf0\x9f\xa4\x94\"}"sv, json);
    var = serde.parse(json).value_or(tr_variant{});
    map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);
    sv = map->value_if<std::string_view>(key);
    ASSERT_TRUE(sv);
    EXPECT_EQ("\xf0\x9f\xa4\x94"sv, *sv);
}

TEST_P(JSONTest, test1)
{
    static auto constexpr Input =
        "{\n"
        "    \"headers\": {\n"
        "        \"type\": \"request\",\n"
        "        \"tag\": 666\n"
        "    },\n"
        "    \"body\": {\n"
        "        \"name\": \"torrent-info\",\n"
        "        \"arguments\": {\n"
        "            \"ids\": [ 7, 10 ]\n"
        "        }\n"
        "    }\n"
        "}\n"sv;

    auto serde = tr_variant_serde::json();
    auto var = serde.inplace().parse(Input).value_or(tr_variant{});
    auto* map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);

    auto* headers = map->find_if<tr_variant::Map>(tr_quark_new("headers"sv));
    ASSERT_NE(headers, nullptr);
    auto sv = headers->value_if<std::string_view>(tr_quark_new("type"sv));
    ASSERT_TRUE(sv);
    EXPECT_EQ("request"sv, *sv);
    auto i = headers->value_if<int64_t>(TR_KEY_tag);
    ASSERT_TRUE(i);
    EXPECT_EQ(666, *i);
    auto* body = map->find_if<tr_variant::Map>(tr_quark_new("body"sv));
    ASSERT_NE(body, nullptr);
    sv = body->value_if<std::string_view>(TR_KEY_name);
    ASSERT_TRUE(sv);
    EXPECT_EQ("torrent-info"sv, *sv);
    auto* args = body->find_if<tr_variant::Map>(tr_quark_new("arguments"sv));
    ASSERT_NE(args, nullptr);
    auto* ids = args->find_if<tr_variant::Vector>(TR_KEY_ids);
    ASSERT_NE(ids, nullptr);
    EXPECT_EQ(2U, std::size(*ids));
    i = (*ids)[0].value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(7, *i);
    i = (*ids)[1].value_if<int64_t>();
    ASSERT_TRUE(i);
    EXPECT_EQ(10, *i);
}

TEST_P(JSONTest, test2)
{
    static auto constexpr Input = " "sv;
    auto var = tr_variant_serde::json().inplace().parse(Input);
    EXPECT_FALSE(var.has_value());
}

TEST_P(JSONTest, test3)
{
    static auto constexpr Input =
        "{ \"error\": 2,"
        "  \"errorString\": \"torrent not registered with this tracker 6UHsVW'*C\","
        "  \"eta\": 262792,"
        "  \"id\": 25,"
        "  \"leftUntilDone\": 2275655680 }"sv;

    auto var = tr_variant_serde::json().inplace().parse(Input).value_or(tr_variant{});
    auto* map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);

    auto sv = map->value_if<std::string_view>(TR_KEY_errorString);
    ASSERT_TRUE(sv);
    EXPECT_EQ("torrent not registered with this tracker 6UHsVW'*C"sv, *sv);
}

TEST_P(JSONTest, unescape)
{
    static auto constexpr Input = R"({ "string-1": "\/usr\/lib" })"sv;

    auto var = tr_variant_serde::json().inplace().parse(Input).value_or(tr_variant{});
    auto* map = var.get_if<tr_variant::Map>();
    ASSERT_NE(map, nullptr);

    auto sv = map->value_if<std::string_view>(tr_quark_new("string-1"sv));
    ASSERT_TRUE(sv);
    EXPECT_EQ("/usr/lib"sv, *sv);
}

INSTANTIATE_TEST_SUITE_P( //
    JSON,
    JSONTest,
    ::testing::Values( //
        "C",
        "da_DK.UTF-8",
        "fr_FR.UTF-8",
        "ru_RU.UTF-8"));
