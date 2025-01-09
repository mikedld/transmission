// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <mutex>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/crypto-utils.h"
#include "libtransmission/log.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h"

#define TR_CRYPTO_X509_FALLBACK
#include "libtransmission/crypto-utils-fallback.cc" // NOLINT(bugprone-suspicious-include)

#if !defined(WITH_MBEDTLS)
#error mbedtls module
#endif

namespace
{
void log_mbedtls_error(int error_code, char const* file, int line)
{
    if (tr_logLevelIsActive(TR_LOG_ERROR))
    {
        char error_message[256];
        mbedtls_strerror(error_code, error_message, sizeof(error_message));

        tr_logAddMessage(
            file,
            line,
            TR_LOG_ERROR,
            fmt::format(
                fmt::runtime(_("{crypto_library} error: {error} ({error_code})")),
                fmt::arg("crypto_library", "MbedTLS"),
                fmt::arg("error", error_message),
                fmt::arg("error_code", error_code)));
    }
}

#define log_error(error_code) log_mbedtls_error((error_code), __FILE__, __LINE__)

bool check_mbedtls_result(int result, int expected_result, char const* file, int line)
{
    bool const ret = result == expected_result;

    if (!ret)
    {
        log_mbedtls_error(result, file, line);
    }

    return ret;
}

#define check_result(result) check_mbedtls_result((result), 0, __FILE__, __LINE__)
#define check_result_eq(result, x_result) check_mbedtls_result((result), (x_result), __FILE__, __LINE__)

// ---

int my_rand(void* /*context*/, unsigned char* buffer, size_t buffer_size)
{
    // since we're initializing tr_rand_buffer()'s rng, we can't use tr_rand_buffer() here
    tr_rand_buffer_std(buffer, buffer_size);
    return 0;
}

mbedtls_ctr_drbg_context* get_rng()
{
    static mbedtls_ctr_drbg_context rng;
    static bool rng_initialized = false;

    if (!rng_initialized)
    {
#if MBEDTLS_VERSION_NUMBER >= 0x02000000
        mbedtls_ctr_drbg_init(&rng);

        if (!check_result(mbedtls_ctr_drbg_seed(&rng, my_rand, nullptr, nullptr, 0)))
#else

        if (!check_result(mbedtls_ctr_drbg_init(&rng, my_rand, nullptr, nullptr, 0)))
#endif
        {
            return nullptr;
        }

        rng_initialized = true;
    }

    return &rng;
}

std::recursive_mutex rng_mutex_;

} // namespace

// --- sha1

tr_sha1::tr_sha1()
{
    clear();
}

tr_sha1::~tr_sha1() = default;

void tr_sha1::clear()
{
    mbedtls_sha1_init(&handle_);

#if MBEDTLS_VERSION_NUMBER < 0x03000000 && MBEDTLS_VERSION_NUMBER >= 0x02070000
    mbedtls_sha1_starts_ret(&handle_);
#else
    mbedtls_sha1_starts(&handle_);
#endif
}

void tr_sha1::add(void const* data, size_t data_length)
{
    if (data_length == 0U)
    {
        return;
    }

#if MBEDTLS_VERSION_NUMBER < 0x03000000 && MBEDTLS_VERSION_NUMBER >= 0x02070000
    mbedtls_sha1_update_ret(&handle_, static_cast<unsigned char const*>(data), data_length);
#else
    mbedtls_sha1_update(&handle_, static_cast<unsigned char const*>(data), data_length);
#endif
}

tr_sha1_digest_t tr_sha1::finish()
{
    auto digest = tr_sha1_digest_t{};
    auto* const digest_as_uchar = reinterpret_cast<unsigned char*>(std::data(digest));
#if MBEDTLS_VERSION_NUMBER < 0x03000000 && MBEDTLS_VERSION_NUMBER >= 0x02070000
    mbedtls_sha1_finish_ret(&handle_, digest_as_uchar);
#else
    mbedtls_sha1_finish(&handle_, digest_as_uchar);
#endif
    clear();
    return digest;
}

// --- sha256

tr_sha256::tr_sha256()
{
    clear();
}

tr_sha256::~tr_sha256() = default;

void tr_sha256::clear()
{
    mbedtls_sha256_init(&handle_);

#if MBEDTLS_VERSION_NUMBER < 0x03000000 && MBEDTLS_VERSION_NUMBER >= 0x02070000
    mbedtls_sha256_starts_ret(&handle_, 0);
#else
    mbedtls_sha256_starts(&handle_, 0);
#endif
}

void tr_sha256::add(void const* data, size_t data_length)
{
    if (data_length == 0U)
    {
        return;
    }

#if MBEDTLS_VERSION_NUMBER < 0x03000000 && MBEDTLS_VERSION_NUMBER >= 0x02070000
    mbedtls_sha256_update_ret(&handle_, static_cast<unsigned char const*>(data), data_length);
#else
    mbedtls_sha256_update(&handle_, static_cast<unsigned char const*>(data), data_length);
#endif
}

tr_sha256_digest_t tr_sha256::finish()
{
    auto digest = tr_sha256_digest_t{};
    auto* const digest_as_uchar = reinterpret_cast<unsigned char*>(std::data(digest));
#if MBEDTLS_VERSION_NUMBER < 0x03000000 && MBEDTLS_VERSION_NUMBER >= 0x02070000
    mbedtls_sha256_finish_ret(&handle_, digest_as_uchar);
#else
    mbedtls_sha256_finish(&handle_, digest_as_uchar);
#endif
    clear();
    return digest;
}

// ---

bool tr_rand_buffer_crypto(void* buffer, size_t length)
{
    if (length == 0)
    {
        return true;
    }

    TR_ASSERT(buffer != nullptr);

    auto constexpr ChunkSize = size_t{ MBEDTLS_CTR_DRBG_MAX_REQUEST };
    static_assert(ChunkSize > 0U);

    auto const lock = std::scoped_lock{ rng_mutex_ };

    for (auto offset = size_t{ 0 }; offset < length; offset += ChunkSize)
    {
        if (!check_result(mbedtls_ctr_drbg_random(
                get_rng(),
                static_cast<unsigned char*>(buffer) + offset,
                std::min(ChunkSize, length - offset))))
        {
            return false;
        }
    }

    return true;
}
