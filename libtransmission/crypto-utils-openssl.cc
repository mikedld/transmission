// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#ifdef __APPLE__
/* OpenSSL "deprecated" as of OS X 10.7, but we still use it */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <array>
#include <cstddef> // size_t

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <fmt/core.h>

#include "libtransmission/crypto-utils.h"
#include "libtransmission/log.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h" // tr_sha1_digest_t, tr_sha25...
#include "libtransmission/utils.h"

#if !defined(WITH_OPENSSL)
#error OPENSSL module
#endif

namespace
{
void log_openssl_error(char const* file, int line)
{
    if (!tr_logLevelIsActive(TR_LOG_ERROR))
    {
        return;
    }

    auto const error_code = ERR_get_error();

    if (static bool strings_loaded = false; !strings_loaded)
    {
#if OPENSSL_VERSION_NUMBER < 0x10100000 || (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x20700000)
        ERR_load_crypto_strings();
#else
        OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
#endif

        strings_loaded = true;
    }

    auto buf = std::array<char, 512>{};
    ERR_error_string_n(error_code, std::data(buf), std::size(buf));
    tr_logAddMessage(
        file,
        line,
        TR_LOG_ERROR,
        fmt::format(
            fmt::runtime(_("{crypto_library} error: {error} ({error_code})")),
            fmt::arg("crypto_library", "OpenSSL"),
            fmt::arg("error", std::data(buf)),
            fmt::arg("error_code", error_code)));
}

#define log_error() log_openssl_error(__FILE__, __LINE__)

bool check_openssl_result(int result, int expected_result, bool expected_equal, char const* file, int line)
{
    bool const ret = (result == expected_result) == expected_equal;

    if (!ret)
    {
        log_openssl_error(file, line);
    }

    return ret;
}

#define check_result(result) check_openssl_result((result), 1, true, __FILE__, __LINE__)

void digest_add_bytes(EVP_MD_CTX* ctx, void const* data, size_t data_length)
{
    if (data_length != 0U)
    {
        EVP_DigestUpdate(ctx, data, data_length);
    }
}

template<typename DigestType>
DigestType digest_finish(EVP_MD_CTX* ctx)
{
    unsigned int hash_length = 0;
    auto digest = DigestType{};
    auto* const digest_as_uchar = reinterpret_cast<unsigned char*>(std::data(digest));
    [[maybe_unused]] bool const ok = check_result(EVP_DigestFinal_ex(ctx, digest_as_uchar, &hash_length));
    TR_ASSERT(!ok || hash_length == std::size(digest));
    return digest;
}
} // namespace

// --- sha1

tr_sha1::tr_sha1()
    : handle_{ EVP_MD_CTX_create() }
{
    clear();
}

tr_sha1::~tr_sha1()
{
    EVP_MD_CTX_destroy(handle_);
}

void tr_sha1::clear()
{
    EVP_DigestInit_ex(handle_, EVP_sha1(), nullptr);
}

void tr_sha1::add(void const* data, size_t data_length)
{
    digest_add_bytes(handle_, data, data_length);
}

tr_sha1_digest_t tr_sha1::finish()
{
    auto digest = digest_finish<tr_sha1_digest_t>(handle_);
    clear();
    return digest;
}

// --- sha256

tr_sha256::tr_sha256()
    : handle_{ EVP_MD_CTX_create() }
{
    clear();
}

tr_sha256::~tr_sha256()
{
    EVP_MD_CTX_destroy(handle_);
}

void tr_sha256::clear()
{
    EVP_DigestInit_ex(handle_, EVP_sha256(), nullptr);
}

void tr_sha256::add(void const* data, size_t data_length)
{
    digest_add_bytes(handle_, data, data_length);
}

tr_sha256_digest_t tr_sha256::finish()
{
    auto digest = digest_finish<tr_sha256_digest_t>(handle_);
    clear();
    return digest;
}

// --- x509

tr_x509_store_t tr_ssl_get_x509_store(tr_ssl_ctx_t handle)
{
    if (handle == nullptr)
    {
        return nullptr;
    }

    return SSL_CTX_get_cert_store(static_cast<SSL_CTX const*>(handle));
}

bool tr_x509_store_add(tr_x509_store_t handle, tr_x509_cert_t cert)
{
    TR_ASSERT(handle != nullptr);
    TR_ASSERT(cert != nullptr);

    return check_result(X509_STORE_add_cert(static_cast<X509_STORE*>(handle), static_cast<X509*>(cert)));
}

tr_x509_cert_t tr_x509_cert_new(void const* der, size_t der_length)
{
    TR_ASSERT(der != nullptr);

    X509* const ret = d2i_X509(nullptr, reinterpret_cast<unsigned char const**>(&der), der_length);

    if (ret == nullptr)
    {
        log_error();
    }

    return ret;
}

void tr_x509_cert_free(tr_x509_cert_t handle)
{
    if (handle == nullptr)
    {
        return;
    }

    X509_free(static_cast<X509*>(handle));
}

// --- rand

bool tr_rand_buffer_crypto(void* buffer, size_t length)
{
    if (length == 0)
    {
        return true;
    }

    TR_ASSERT(buffer != nullptr);

    return check_result(RAND_bytes(static_cast<unsigned char*>(buffer), (int)length));
}
