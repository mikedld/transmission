// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#ifndef TR_CRYPTO_UTILS_H
#define TR_CRYPTO_UTILS_H

#include <array>
#include <cstddef> // size_t
#include <cstdint>
#include <limits>
#include <optional>
#include <random> // for std::uniform_int_distribution<T>
#include <string>
#include <string_view>

#include "libtransmission/tr-macros.h" // tr_sha1_digest_t, tr_sha256_d...
#include "libtransmission/tr-strbuf.h"

#if defined(WITH_CCRYPTO)
#include <CommonCrypto/CommonDigest.h>
using tr_sha1_context_t = CC_SHA1_CTX;
using tr_sha256_context_t = CC_SHA256_CTX;
#elif defined(WITH_MBEDTLS)
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
using tr_sha1_context_t = mbedtls_sha1_context;
using tr_sha256_context_t = mbedtls_sha256_context;
#elif defined(WITH_OPENSSL)
#include <openssl/evp.h>
using tr_sha1_context_t = EVP_MD_CTX*;
using tr_sha256_context_t = EVP_MD_CTX*;
#elif defined(WITH_WOLFSSL)
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>
using tr_sha1_context_t = wc_Sha;
using tr_sha256_context_t = wc_Sha256;
#else
#error no crypto library specified
#endif

/**
 * @addtogroup utils Utilities
 * @{
 */

class tr_sha1
{
public:
    tr_sha1();
    tr_sha1(tr_sha1&&) = delete;
    tr_sha1(tr_sha1 const&) = delete;
    tr_sha1& operator=(tr_sha1&&) = delete;
    tr_sha1& operator=(tr_sha1 const&) = delete;
    ~tr_sha1();

    void add(void const* data, size_t data_length);
    [[nodiscard]] tr_sha1_digest_t finish();
    void clear();

    template<typename... T>
    [[nodiscard]] static auto digest(T const&... args)
    {
        auto context = tr_sha1{};
        (context.add(std::data(args), std::size(args)), ...);
        return context.finish();
    }

private:
    tr_sha1_context_t handle_;
};

class tr_sha256
{
public:
    tr_sha256();
    tr_sha256(tr_sha256&&) = delete;
    tr_sha256(tr_sha256 const&) = delete;
    tr_sha256& operator=(tr_sha256&&) = delete;
    tr_sha256& operator=(tr_sha256 const&) = delete;
    ~tr_sha256();

    void add(void const* data, size_t data_length);
    [[nodiscard]] tr_sha256_digest_t finish();
    void clear();

    template<typename... T>
    [[nodiscard]] static auto digest(T const&... args)
    {
        auto context = tr_sha256{};
        (context.add(std::data(args), std::size(args)), ...);
        return context.finish();
    }

private:
    tr_sha256_context_t handle_;
};

/** @brief Opaque SSL context type. */
using tr_ssl_ctx_t = void*;
/** @brief Opaque X509 certificate store type. */
using tr_x509_store_t = void*;
/** @brief Opaque X509 certificate type. */
using tr_x509_cert_t = void*;

/**
 * @brief Get X509 certificate store from SSL context.
 */
tr_x509_store_t tr_ssl_get_x509_store(tr_ssl_ctx_t handle);

/**
 * @brief Add certificate to X509 certificate store.
 */
bool tr_x509_store_add(tr_x509_store_t handle, tr_x509_cert_t cert);

/**
 * @brief Allocate and initialize new X509 certificate from DER-encoded buffer.
 */
tr_x509_cert_t tr_x509_cert_new(void const* der, size_t der_length);

/**
 * @brief Free X509 certificate returned by @ref tr_x509_cert_new.
 */
void tr_x509_cert_free(tr_x509_cert_t handle);

/**
 * @brief Fill a buffer with random bytes.
 */
void tr_rand_buffer(void* buffer, size_t length);

// Client code should use `tr_rand_buffer()`.
// These helpers are only exposed here to permit open-box tests.
bool tr_rand_buffer_crypto(void* buffer, size_t length);
void tr_rand_buffer_std(void* buffer, size_t length);

template<typename T>
T tr_rand_obj()
{
    auto t = T{};
    tr_rand_buffer(&t, sizeof(T));
    return t;
}

/**
 * @brief Generate a SSHA password from its plaintext source.
 */
[[nodiscard]] std::string tr_ssha1(std::string_view plaintext);

/**
 * @brief Return true if this is salted text, false otherwise
 */
[[nodiscard]] bool tr_ssha1_test(std::string_view text);

/**
 * @brief Validate a test password against the a ssha1 password.
 */
[[nodiscard]] bool tr_ssha1_matches(std::string_view ssha1, std::string_view plaintext);

/**
 * @brief Translate null-terminated string into base64.
 * @return a new std::string with the encoded contents
 */
[[nodiscard]] std::string tr_base64_encode(std::string_view input);

/**
 * @brief Translate a character range from base64 into raw form.
 * @return a new std::string with the decoded contents.
 */
[[nodiscard]] std::string tr_base64_decode(std::string_view input);

using tr_sha1_string = tr_strbuf<char, sizeof(tr_sha1_digest_t) * 2U + 1U>;

/**
 * @brief Generate an ascii hex string for a sha1 digest.
 */
[[nodiscard]] tr_sha1_string tr_sha1_to_string(tr_sha1_digest_t const&);

/**
 * @brief Generate a sha1 digest from a hex string.
 */
[[nodiscard]] std::optional<tr_sha1_digest_t> tr_sha1_from_string(std::string_view hex);

using tr_sha256_string = tr_strbuf<char, sizeof(tr_sha256_digest_t) * 2U + 1U>;

/**
 * @brief Generate an ascii hex string for a sha256 digest.
 */
[[nodiscard]] tr_sha256_string tr_sha256_to_string(tr_sha256_digest_t const&);

/**
 * @brief Generate a sha256 digest from a hex string.
 */
[[nodiscard]] std::optional<tr_sha256_digest_t> tr_sha256_from_string(std::string_view hex);

/**
 * @brief Calculate CRC32-C checksum for a buffer.
 */
[[nodiscard]] uint32_t tr_crc32c(uint8_t const* data, size_t count);

// Convenience utility to efficiently get many random small values.
// Use this instead of making a lot of calls to tr_rand_int().
template<typename T = uint8_t, size_t N = 1024U>
class tr_salt_shaker
{
public:
    [[nodiscard]] auto operator()() noexcept
    {
        if (pos == std::size(buf))
        {
            pos = 0U;
        }

        if (pos == 0U)
        {
            tr_rand_buffer(std::data(buf), std::size(buf) * sizeof(T));
        }

        return buf[pos++];
    }

private:
    size_t pos = 0;
    std::array<T, N> buf;
};

// UniformRandomBitGenerator impl that uses `tr_rand_buffer()`.
// See https://en.cppreference.com/w/cpp/named_req/UniformRandomBitGenerator
template<typename T, size_t N = 1024U>
class tr_urbg
{
public:
    using result_type = T;
    static_assert(!std::numeric_limits<T>::is_signed);

    [[nodiscard]] static constexpr T min() noexcept
    {
        return std::numeric_limits<T>::min();
    }

    [[nodiscard]] static constexpr T max() noexcept
    {
        return std::numeric_limits<T>::max();
    }

    [[nodiscard]] T operator()() noexcept
    {
        return buf_();
    }

private:
    tr_salt_shaker<T, N> buf_;
};

/**
 * @brief Returns a random number in the range of [0...upper_bound).
 */
template<class T>
[[nodiscard]] T tr_rand_int(T upper_bound)
{
    static_assert(!std::is_signed<T>());
    using dist_type = std::uniform_int_distribution<T>;

    thread_local auto rng = tr_urbg<T>{};
    thread_local auto dist = dist_type{};
    return dist(rng, typename dist_type::param_type(0, upper_bound - 1));
}

/** @} */

#endif /* TR_CRYPTO_UTILS_H */
