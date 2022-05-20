// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#ifndef TR_CRYPTO_UTILS_H
#define TR_CRYPTO_UTILS_H

#include <cstdint> // uint8_t
#include <cstddef> // size_t
#include <optional>
#include <string>
#include <string_view>

#include "transmission.h" // tr_sha1_digest_t

/**
*** @addtogroup utils Utilities
*** @{
**/

/** @brief Opaque SHA1 context type. */
using tr_sha1_ctx_t = void*;
/** @brief Opaque DH context type. */
using tr_dh_ctx_t = void*;
/** @brief Opaque DH secret key type. */
using tr_dh_secret_t = void*;
/** @brief Opaque SSL context type. */
using tr_ssl_ctx_t = void*;
/** @brief Opaque X509 certificate store type. */
using tr_x509_store_t = void*;
/** @brief Opaque X509 certificate type. */
using tr_x509_cert_t = void*;

/**
 * @brief Allocate and initialize new SHA1 hasher context.
 */
tr_sha1_ctx_t tr_sha1_init(void);

/**
 * @brief Update SHA1 hash.
 */
bool tr_sha1_update(tr_sha1_ctx_t handle, void const* data, size_t data_length);

/**
 * @brief Finalize and export SHA1 hash, free hasher context.
 */
std::optional<tr_sha1_digest_t> tr_sha1_final(tr_sha1_ctx_t handle);

/**
 * @brief Generate a SHA1 hash from one or more chunks of memory.
 */
template<typename... T>
std::optional<tr_sha1_digest_t> tr_sha1(T... args)
{
    auto ctx = tr_sha1_init();
    if (ctx == nullptr)
    {
        return std::nullopt;
    }

    if ((tr_sha1_update(ctx, std::data(args), std::size(args)) && ...))
    {
        return tr_sha1_final(ctx);
    }

    // one of the update() calls failed so we will return nullopt,
    // but we need to call final() first to ensure ctx is released
    tr_sha1_final(ctx);
    return std::nullopt;
}

/**
 * @brief Allocate and initialize new Diffie-Hellman (DH) key exchange context.
 */
tr_dh_ctx_t tr_dh_new(
    uint8_t const* prime_num,
    size_t prime_num_length,
    uint8_t const* generator_num,
    size_t generator_num_length);

/**
 * @brief Free DH key exchange context.
 */
void tr_dh_free(tr_dh_ctx_t handle);

/**
 * @brief Generate private and public DH keys, export public key.
 */
bool tr_dh_make_key(tr_dh_ctx_t handle, size_t private_key_length, uint8_t* public_key, size_t* public_key_length);

/**
 * @brief Perform DH key exchange, generate secret key.
 */
tr_dh_secret_t tr_dh_agree(tr_dh_ctx_t handle, uint8_t const* other_public_key, size_t other_public_key_length);

/**
 * @brief Calculate SHA1 hash of DH secret key, prepending and/or appending
 *        given data to the key during calculation.
 */
std::optional<tr_sha1_digest_t> tr_dh_secret_derive(
    tr_dh_secret_t handle,
    void const* prepend_data,
    size_t prepend_data_size,
    void const* append_data,
    size_t append_data_size);

/**
 * @brief Free DH secret key returned by @ref tr_dh_agree.
 */
void tr_dh_secret_free(tr_dh_secret_t handle);

/**
 * @brief Align DH key (big-endian number) to required length (internal, do not use).
 */
void tr_dh_align_key(uint8_t* key_buffer, size_t key_size, size_t buffer_size);

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
tr_x509_cert_t tr_x509_cert_new(void const* der_data, size_t der_data_size);

/**
 * @brief Free X509 certificate returned by @ref tr_x509_cert_new.
 */
void tr_x509_cert_free(tr_x509_cert_t handle);

/**
 * @brief Returns a random number in the range of [0...upper_bound).
 */
int tr_rand_int(int upper_bound);

/**
 * @brief Returns a pseudorandom number in the range of [0...upper_bound).
 *
 * This is faster, BUT WEAKER, than tr_rand_int() and never be used in sensitive cases.
 * @see tr_rand_int()
 */
int tr_rand_int_weak(int upper_bound);

/**
 * @brief Fill a buffer with random bytes.
 */
bool tr_rand_buffer(void* buffer, size_t length);

/**
 * @brief Generate a SSHA password from its plaintext source.
 */
std::string tr_ssha1(std::string_view plain_text);

/**
 * @brief Return true if this is salted text, false otherwise
 */
bool tr_ssha1_test(std::string_view text);

/**
 * @brief Validate a test password against the a ssha1 password.
 */
bool tr_ssha1_matches(std::string_view ssha1, std::string_view plain_text);

/**
 * @brief Translate null-terminated string into base64.
 * @return a new std::string with the encoded contents
 */
std::string tr_base64_encode(std::string_view input);

/**
 * @brief Translate a character range from base64 into raw form.
 * @return a new std::string with the decoded contents.
 */
std::string tr_base64_decode(std::string_view input);

/**
 * @brief Generate an ascii hex string for a sha1 digest.
 */
std::string tr_sha1_to_string(tr_sha1_digest_t const&);

/**
 * @brief Generate a sha1 digest from a hex string.
 */
std::optional<tr_sha1_digest_t> tr_sha1_from_string(std::string_view hex);

/** @} */

#endif /* TR_CRYPTO_UTILS_H */
