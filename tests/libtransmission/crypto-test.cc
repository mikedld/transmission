// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cassert>
#include <cstddef> // std::byte, size_t
#include <cstdint> // uint8_t
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>

#include <libtransmission/peer-mse.h>
#include <libtransmission/crypto-utils.h>
#include <libtransmission/tr-macros.h>
#include <libtransmission/utils.h>

#include "gtest/gtest.h"

using namespace std::literals;

namespace
{

auto constexpr SomeHash = tr_sha1_digest_t{
    std::byte{ 0 },  std::byte{ 1 },  std::byte{ 2 },  std::byte{ 3 },  std::byte{ 4 },  std::byte{ 5 },  std::byte{ 6 },
    std::byte{ 7 },  std::byte{ 8 },  std::byte{ 9 },  std::byte{ 10 }, std::byte{ 11 }, std::byte{ 12 }, std::byte{ 13 },
    std::byte{ 14 }, std::byte{ 15 }, std::byte{ 16 }, std::byte{ 17 }, std::byte{ 18 }, std::byte{ 19 },
};

template<size_t N>
std::string toString(std::array<std::byte, N> const& array)
{
    auto ostr = std::ostringstream{};
    ostr << '[';
    for (auto const b : array)
    {
        ostr << static_cast<unsigned>(b) << ' ';
    }
    ostr << ']';
    return ostr.str();
}

} // namespace

TEST(Crypto, DH)
{
    auto a = tr_message_stream_encryption::DH{ tr_message_stream_encryption::DH::randomPrivateKey() };
    auto b = tr_message_stream_encryption::DH{ tr_message_stream_encryption::DH::randomPrivateKey() };

    a.setPeerPublicKey(b.publicKey());
    b.setPeerPublicKey(a.publicKey());
    EXPECT_EQ(toString(a.secret()), toString(b.secret()));
    EXPECT_EQ(a.secret(), b.secret());
    EXPECT_EQ(96U, std::size(a.secret()));

    auto c = tr_message_stream_encryption::DH{ tr_message_stream_encryption::DH::randomPrivateKey() };
    c.setPeerPublicKey(b.publicKey());
    EXPECT_NE(a.secret(), c.secret());
    EXPECT_NE(toString(a.secret()), toString(c.secret()));
}

TEST(Crypto, encryptDecrypt)
{
    auto a_dh = tr_message_stream_encryption::DH{ tr_message_stream_encryption::DH::randomPrivateKey() };
    auto b_dh = tr_message_stream_encryption::DH{ tr_message_stream_encryption::DH::randomPrivateKey() };

    a_dh.setPeerPublicKey(b_dh.publicKey());
    b_dh.setPeerPublicKey(a_dh.publicKey());

    auto constexpr Input1 = "test1"sv;
    auto encrypted1 = std::array<char, 128>{};
    auto decrypted1 = std::array<char, 128>{};

    auto a = tr_message_stream_encryption::Filter{};
    a.encrypt_init(false, a_dh, SomeHash);
    a.encrypt(std::data(Input1), std::size(Input1), std::data(encrypted1));
    auto b = tr_message_stream_encryption::Filter{};
    b.decrypt_init(true, b_dh, SomeHash);
    b.decrypt(std::data(encrypted1), std::size(Input1), std::data(decrypted1));
    EXPECT_EQ(Input1, std::data(decrypted1)) << "Input1 " << Input1 << " decrypted1 " << std::data(decrypted1);

    auto constexpr Input2 = "@#)C$@)#(*%bvkdjfhwbc039bc4603756VB3)"sv;
    auto encrypted2 = std::array<char, 128>{};
    auto decrypted2 = std::array<char, 128>{};

    b.encrypt_init(true, b_dh, SomeHash);
    b.encrypt(std::data(Input2), std::size(Input2), std::data(encrypted2));
    a.decrypt_init(false, a_dh, SomeHash);
    a.decrypt(std::data(encrypted2), std::size(Input2), std::data(decrypted2));
    EXPECT_EQ(Input2, std::data(decrypted2)) << "Input2 " << Input2 << " decrypted2 " << std::data(decrypted2);
}

TEST(Crypto, sha1)
{
    auto hash1 = tr_sha1::digest("test"sv);
    EXPECT_EQ(
        0,
        memcmp(
            std::data(hash1),
            "\xa9\x4a\x8f\xe5\xcc\xb1\x9b\xa6\x1c\x4c\x08\x73\xd3\x91\xe9\x87\x98\x2f\xbb\xd3",
            std::size(hash1)));

    auto hash2 = tr_sha1::digest("test"sv);
    EXPECT_EQ(hash1, hash2);

    hash1 = tr_sha1::digest("1"sv, "22"sv, "333"sv);
    hash2 = tr_sha1::digest("1"sv, "22"sv, "333"sv);
    EXPECT_EQ(hash1, hash2);
    EXPECT_EQ(
        0,
        memcmp(
            std::data(hash1),
            "\x1f\x74\x64\x8e\x50\xa6\xa6\x70\x8e\xc5\x4a\xb3\x27\xa1\x63\xd5\x53\x6b\x7c\xed",
            std::size(hash1)));

    auto const hash3 = tr_sha1::digest("test"sv);
    EXPECT_EQ("a94a8fe5ccb19ba61c4c0873d391e987982fbbd3"sv, tr_sha1_to_string(hash3));

    auto const hash4 = tr_sha1::digest("te"sv, "st"sv);
    EXPECT_EQ("a94a8fe5ccb19ba61c4c0873d391e987982fbbd3"sv, tr_sha1_to_string(hash4));

    auto const hash5 = tr_sha1::digest("t"sv, "e"sv, std::string{ "s" }, std::array<char, 1>{ { 't' } });
    EXPECT_EQ("a94a8fe5ccb19ba61c4c0873d391e987982fbbd3"sv, tr_sha1_to_string(hash5));
}

TEST(Crypto, ssha1)
{
    struct LocalTest
    {
        std::string_view plain_text;
        std::string_view ssha1;
    };

    static auto constexpr Tests = std::array<LocalTest, 2>{ {
        { "test"sv, "{15ad0621b259a84d24dcd4e75b09004e98a3627bAMbyRHJy"sv },
        { "QNY)(*#$B)!_X$B !_B#($^!)*&$%CV!#)&$C!@$(P*)"sv, "{10e2d7acbb104d970514a147cd16d51dfa40fb3c0OSwJtOL"sv },
    } };

    static auto constexpr HashCount = size_t{ 4U } * 1024U;

    for (auto const& [plain_text, ssha1] : Tests)
    {
        auto hashes = std::unordered_set<std::string>{};
        hashes.reserve(HashCount);

        EXPECT_TRUE(tr_ssha1_matches(ssha1, plain_text));

        for (size_t j = 0; j < HashCount; ++j)
        {
            auto const hash = tr_ssha1(plain_text);
            EXPECT_TRUE(tr_ssha1_matches(hash, plain_text));
            hashes.insert(hash);
        }

        // confirm all hashes are different
        EXPECT_EQ(HashCount, hashes.size());

        /* exchange two first chars */
        auto phrase = std::string{ plain_text };
        std::swap(phrase[0], phrase[1]);

        for (auto const& hash : hashes)
        {
            /* changed phrase doesn't match the hashes */
            EXPECT_FALSE(tr_ssha1_matches(hash, phrase));
        }
    }

    /* should work with different salt lengths as well */
    EXPECT_TRUE(tr_ssha1_matches("{a94a8fe5ccb19ba61c4c0873d391e987982fbbd3", "test"));
    EXPECT_TRUE(tr_ssha1_matches("{d209a21d3bc4f8fc4f8faf347e69f3def597eb170pySy4ai1ZPMjeU1", "test"));
}

TEST(Crypto, sha1FromString)
{
    // bad lengths
    EXPECT_FALSE(tr_sha1_from_string(""));
    EXPECT_FALSE(tr_sha1_from_string("a94a8fe5ccb19ba61c4c0873d391e987982fbbd"sv));
    EXPECT_FALSE(tr_sha1_from_string("a94a8fe5ccb19ba61c4c0873d391e987982fbbd33"sv));
    // nonhex
    EXPECT_FALSE(tr_sha1_from_string("a94a8fe5ccb19ba61c4cz873d391e987982fbbd3"sv));
    EXPECT_FALSE(tr_sha1_from_string("a94a8fe5ccb19  61c4c0873d391e987982fbbd3"sv));

    // lowercase hex
    auto const baseline = "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3"sv;
    auto const lc = tr_sha1_from_string(baseline);
    EXPECT_TRUE(lc.has_value());
    assert(lc.has_value());
    EXPECT_EQ(baseline, tr_sha1_to_string(*lc));

    // uppercase hex should yield the same result
    auto const uc = tr_sha1_from_string(tr_strupper(baseline));
    EXPECT_TRUE(uc.has_value());
    assert(uc.has_value());
    EXPECT_EQ(*lc, *uc);
}

TEST(Crypto, sha256FromString)
{
    // bad lengths
    EXPECT_FALSE(tr_sha256_from_string(""));
    EXPECT_FALSE(tr_sha256_from_string("a94a8fe5ccb19ba61c4c0873d391e987982fbbd"sv));
    EXPECT_FALSE(tr_sha256_from_string("a94a8fe5ccb19ba61c4c0873d391e987982fbbd33"sv));
    EXPECT_FALSE(tr_sha256_from_string("05d58dfd14ed21d33add137eb7a2c5d4ef5aaa4a945e654363d32b7c4bf5c92"sv));
    EXPECT_FALSE(tr_sha256_from_string("05d58dfd14ed21d33add137eb7a2c5d4ef5aaa4a945e654363d32b7c4bf5c9299"sv));
    // nonhex
    EXPECT_FALSE(tr_sha256_from_string("a94a8fe5ccb19ba61c4cz873d391e987982fbbd3aaaaaaaaaaaaaaaaaaaaaaa"sv));
    EXPECT_FALSE(tr_sha256_from_string("05  8dfd14ed21d33add137eb7a2c5d4ef5aaa4a945e654363d32b7c4bf5c92"sv));

    // lowercase hex
    auto const baseline = "05d58dfd14ed21d33add137eb7a2c5d4ef5aaa4a945e654363d32b7c4bf5c929"sv;
    auto const lc = tr_sha256_from_string(baseline);
    EXPECT_TRUE(lc.has_value());
    assert(lc.has_value());
    EXPECT_EQ(baseline, tr_sha256_to_string(*lc));

    // uppercase hex should yield the same result
    auto const uc = tr_sha256_from_string(tr_strupper(baseline));
    EXPECT_TRUE(uc.has_value());
    assert(uc.has_value());
    EXPECT_EQ(*lc, *uc);
}

TEST(Crypto, random)
{
    /* test that tr_rand_int() stays in-bounds */
    for (int i = 0; i < 100000; ++i)
    {
        auto const val = tr_rand_int(100U);
        EXPECT_LE(0U, val);
        EXPECT_LT(val, 100U);
    }
}

using CryptoRandBufferTest = ::testing::TestWithParam<size_t>;

TEST_P(CryptoRandBufferTest, randBuf)
{
    static auto constexpr Iterations = 1000U;

    auto const width = GetParam();
    auto const empty = std::vector<uint8_t>(width, 0);

    auto buf = empty;

    for (size_t i = 0; i < Iterations; ++i)
    {
        auto tmp = buf;
        tr_rand_buffer(std::data(tmp), std::size(tmp));
        EXPECT_NE(tmp, empty);
        EXPECT_NE(tmp, buf);
        buf = tmp;
    }

    for (size_t i = 0; i < Iterations; ++i)
    {
        auto tmp = buf;
        EXPECT_TRUE(tr_rand_buffer_crypto(std::data(tmp), std::size(tmp)));
        EXPECT_NE(tmp, empty);
        EXPECT_NE(tmp, buf);
        buf = tmp;
    }

    for (size_t i = 0; i < Iterations; ++i)
    {
        auto tmp = buf;
        tr_rand_buffer_std(std::data(tmp), std::size(tmp));
        EXPECT_NE(tmp, empty);
        EXPECT_NE(tmp, buf);
        buf = tmp;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Crypto,
    CryptoRandBufferTest,
    ::testing::Values(32, 100, 1024, 3000),
    ::testing::PrintToStringParamName{});

TEST(Crypto, base64)
{
    auto raw = std::string_view{ "YOYO!"sv };
    auto encoded = tr_base64_encode(raw);
    EXPECT_EQ("WU9ZTyE="sv, encoded);
    EXPECT_EQ(raw, tr_base64_decode(encoded));

    EXPECT_EQ(""sv, tr_base64_encode(""sv));
    EXPECT_EQ(""sv, tr_base64_decode(""sv));

    static auto constexpr MaxBufSize = size_t{ 1024 };
    for (size_t i = 1; i <= MaxBufSize; ++i)
    {
        auto buf = std::string{};
        for (size_t j = 0; j < i; ++j)
        {
            buf += static_cast<char>(tr_rand_int(256U));
        }
        EXPECT_EQ(buf, tr_base64_decode(tr_base64_encode(buf)));

        buf = std::string{};
        for (size_t j = 0; j < i; ++j)
        {
            buf += static_cast<char>(1U + tr_rand_int(255U));
        }
        EXPECT_EQ(buf, tr_base64_decode(tr_base64_encode(buf)));
    }
}
