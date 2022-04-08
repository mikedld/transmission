// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring> // memmove(), memset()
#include <iterator>
#include <random>
#include <string>
#include <string_view>

#include <arc4.h>

extern "C"
{
#include <b64/cdecode.h>
#include <b64/cencode.h>
}

#include <fmt/format.h>

#include "transmission.h"
#include "crypto-utils.h"
#include "tr-assert.h"
#include "utils.h"

using namespace std::literals;

/***
****
***/

void tr_dh_align_key(uint8_t* key_buffer, size_t key_size, size_t buffer_size)
{
    TR_ASSERT(key_size <= buffer_size);

    /* DH can generate key sizes that are smaller than the size of
       key buffer with exponentially decreasing probability, in which case
       the msb's of key buffer need to be zeroed appropriately. */
    if (key_size < buffer_size)
    {
        size_t const offset = buffer_size - key_size;
        memmove(key_buffer + offset, key_buffer, key_size);
        memset(key_buffer, 0, offset);
    }
}

/***
****
***/

int tr_rand_int(int upper_bound)
{
    TR_ASSERT(upper_bound > 0);

    if (unsigned int noise = 0; tr_rand_buffer(&noise, sizeof(noise)))
    {
        return noise % upper_bound;
    }

    /* fall back to a weaker implementation... */
    return tr_rand_int_weak(upper_bound);
}

int tr_rand_int_weak(int upper_bound)
{
    TR_ASSERT(upper_bound > 0);

    thread_local auto random_engine = std::mt19937{ std::random_device{}() };
    using distribution_type = std::uniform_int_distribution<>;
    thread_local distribution_type distribution;

    // Upper bound is inclusive in std::uniform_int_distribution.
    return distribution(random_engine, distribution_type::param_type{ 0, upper_bound - 1 });
}

/***
****
***/

namespace
{

auto constexpr DigestStringSize = TR_SHA1_DIGEST_STRLEN;
auto constexpr SaltedPrefix = "{"sv;

std::string tr_salt(std::string_view plaintext, std::string_view salt)
{
    static_assert(DigestStringSize == 40);

    // build a sha1 digest of the original content and the salt
    auto const digest = tr_sha1(plaintext, salt);

    // convert it to a string. string holds three parts:
    // DigestPrefix, stringified digest of plaintext + salt, and the salt.
    return fmt::format(FMT_STRING("{:s}{:s}{:s}"), SaltedPrefix, tr_sha1_to_string(*digest), salt);
}

} // namespace

std::string tr_ssha1(std::string_view plaintext)
{
    // build an array of random Salter chars
    auto constexpr Salter = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ./"sv;
    static_assert(std::size(Salter) == 64);
    auto constexpr SaltSize = size_t{ 8 };
    auto salt = std::array<char, SaltSize>{};
    tr_rand_buffer(std::data(salt), std::size(salt));
    std::transform(
        std::begin(salt),
        std::end(salt),
        std::begin(salt),
        [&Salter](auto ch) { return Salter[ch % std::size(Salter)]; });

    return tr_salt(plaintext, std::string_view{ std::data(salt), std::size(salt) });
}

bool tr_ssha1_test(std::string_view text)
{
    return tr_strvStartsWith(text, SaltedPrefix) && std::size(text) >= std::size(SaltedPrefix) + DigestStringSize;
}

bool tr_ssha1_matches(std::string_view ssha1, std::string_view plaintext)
{
    if (!tr_ssha1_test(ssha1))
    {
        return false;
    }

    auto const salt = ssha1.substr(std::size(SaltedPrefix) + DigestStringSize);
    return tr_salt(plaintext, salt) == ssha1;
}

/***
****
***/

static size_t base64_alloc_size(std::string_view input)
{
    size_t ret_length = 4 * ((std::size(input) + 2) / 3);
#ifdef USE_SYSTEM_B64
    // Additional space is needed for newlines if we're using unpatched libb64
    ret_length += ret_length / 72 + 1;
#endif
    return ret_length * 8;
}

std::string tr_base64_encode(std::string_view input)
{
    auto buf = std::vector<char>(base64_alloc_size(input));
    auto state = base64_encodestate{};
    base64_init_encodestate(&state);
    size_t len = base64_encode_block(std::data(input), std::size(input), std::data(buf), &state);
    len += base64_encode_blockend(std::data(buf) + len, &state);
    auto str = std::string{};
    std::copy_if(
        std::data(buf),
        std::data(buf) + len,
        std::back_inserter(str),
        [](auto ch) { return !tr_strvContains("\r\n"sv, ch); });
    return str;
}

std::string tr_base64_decode(std::string_view input)
{
    auto buf = std::vector<char>(std::size(input) + 8);
    auto state = base64_decodestate{};
    base64_init_decodestate(&state);
    size_t const len = base64_decode_block(std::data(input), std::size(input), std::data(buf), &state);
    return std::string{ std::data(buf), len };
}

/***
****
***/

static void tr_binary_to_hex(void const* vinput, void* voutput, size_t byte_length)
{
    static char constexpr Hex[] = "0123456789abcdef";

    auto const* input = static_cast<uint8_t const*>(vinput);
    auto* output = static_cast<char*>(voutput);

    /* go from back to front to allow for in-place conversion */
    input += byte_length;
    output += byte_length * 2;

    *output = '\0';

    while (byte_length-- > 0)
    {
        unsigned int const val = *(--input);
        *(--output) = Hex[val & 0xf];
        *(--output) = Hex[val >> 4];
    }
}

std::string tr_sha1_to_string(tr_sha1_digest_t const& digest)
{
    auto str = std::string(std::size(digest) * 2, '?');
    tr_binary_to_hex(std::data(digest), std::data(str), std::size(digest));
    return str;
}

static void tr_hex_to_binary(char const* input, void* voutput, size_t byte_length)
{
    static char constexpr Hex[] = "0123456789abcdef";

    auto* output = static_cast<uint8_t*>(voutput);

    for (size_t i = 0; i < byte_length; ++i)
    {
        int const hi = strchr(Hex, tolower(*input++)) - Hex;
        int const lo = strchr(Hex, tolower(*input++)) - Hex;
        *output++ = (uint8_t)((hi << 4) | lo);
    }
}

std::optional<tr_sha1_digest_t> tr_sha1_from_string(std::string_view hex)
{
    if (std::size(hex) != TR_SHA1_DIGEST_STRLEN)
    {
        return {};
    }

    if (!std::all_of(std::begin(hex), std::end(hex), [](unsigned char ch) { return isxdigit(ch); }))
    {
        return {};
    }

    auto digest = tr_sha1_digest_t{};
    tr_hex_to_binary(std::data(hex), std::data(digest), std::size(digest));
    return digest;
}
