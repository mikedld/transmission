// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <memory>
#include <type_traits>

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonRandom.h>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/crypto-utils.h"
#include "libtransmission/log.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h"

#define TR_CRYPTO_X509_FALLBACK
#include "crypto-utils-fallback.cc" // NOLINT(bugprone-suspicious-include)

// ---

namespace
{

char const* ccrypto_error_to_str(CCCryptorStatus error_code)
{
    switch (error_code)
    {
    case kCCSuccess:
        return "Operation completed normally";

    case kCCParamError:
        return "Illegal parameter value";

    case kCCBufferTooSmall:
        return "Insufficient buffer provided for specified operation";

    case kCCMemoryFailure:
        return "Memory allocation failure";

    case kCCAlignmentError:
        return "Input size was not aligned properly";

    case kCCDecodeError:
        return "Input data did not decode or decrypt properly";

    case kCCUnimplemented:
        return "Function not implemented for the current algorithm";

    case kCCOverflow:
        return "Buffer overflow";

    case kCCRNGFailure:
        return "Random number generator failure";
    }

    return "Unknown error";
}

void log_ccrypto_error(CCCryptorStatus error_code, char const* file, long line)
{
    if (tr_logLevelIsActive(TR_LOG_ERROR))
    {
        tr_logAddMessage(
            file,
            line,
            TR_LOG_ERROR,
            fmt::format(
                fmt::runtime(_("{crypto_library} error: {error} ({error_code})")),
                fmt::arg("crypto_library", "CCrypto"),
                fmt::arg("error", ccrypto_error_to_str(error_code)),
                fmt::arg("error_code", error_code)));
    }
}

bool check_ccrypto_result(CCCryptorStatus result, char const* file, long line)
{
    bool const ret = result == kCCSuccess;

    if (!ret)
    {
        log_ccrypto_error(result, file, line);
    }

    return ret;
}

#define check_result(result) check_ccrypto_result((result), __FILE__, __LINE__)

} // namespace

// --- sha1

tr_sha1::tr_sha1()
{
    clear();
}

tr_sha1::~tr_sha1()
{
}

void tr_sha1::clear()
{
    CC_SHA1_Init(&handle_);
}

void tr_sha1::add(void const* data, size_t data_length)
{
    if (data_length == 0U)
    {
        return;
    }

    CC_SHA1_Update(&handle_, data, data_length);
}

tr_sha1_digest_t tr_sha1::finish()
{
    auto digest = tr_sha1_digest_t{};
    CC_SHA1_Final(reinterpret_cast<unsigned char*>(std::data(digest)), &handle_);
    clear();
    return digest;
}

// --- sha256

tr_sha256::tr_sha256()
{
    clear();
}

tr_sha256::~tr_sha256()
{
}

void tr_sha256::clear()
{
    CC_SHA256_Init(&handle_);
}

void tr_sha256::add(void const* data, size_t data_length)
{
    if (data_length == 0U)
    {
        return;
    }

    CC_SHA256_Update(&handle_, data, data_length);
}

tr_sha256_digest_t tr_sha256::finish()
{
    auto digest = tr_sha256_digest_t{};
    CC_SHA256_Final(reinterpret_cast<unsigned char*>(std::data(digest)), &handle_);
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

    return check_result(CCRandomGenerateBytes(buffer, length));
}
