// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstddef> // std::byte
#include <cstdint>
#include <limits> // std::numeric_limits
#include <string_view>

#include <math/wide_integer/uintwide_t.h>

#include "libtransmission/crypto-utils.h" // tr_sha1
#include "libtransmission/peer-mse.h"
#include "libtransmission/tr-arc4.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h" // tr_sha1_digest_t

// workaround bug in GCC < 10.4
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=99859
#if __GNUC__ < 10 || (__GNUC__ == 10 && __GNUC_MINOR__ < 4)
#define PRIME_CONSTEXPR const
#else
#define PRIME_CONSTEXPR constexpr
#endif

using namespace std::literals;

namespace
{
namespace wi
{
// clang-format off
using key_t = math::wide_integer::uintwide_t<
    tr_message_stream_encryption::DH::KeySize * std::numeric_limits<unsigned char>::digits
#ifdef WIDE_INTEGER_HAS_LIMB_TYPE_UINT64
    , uint64_t
#endif
    >;
// clang-format on

using private_key_t = math::wide_integer::uintwide_t<
    tr_message_stream_encryption::DH::PrivateKeySize * std::numeric_limits<unsigned char>::digits>;

template<typename UIntWide>
auto import_bits(std::array<std::byte, UIntWide::my_width2 / std::numeric_limits<uint8_t>::digits> const& bigend_bin)
{
    auto ret = UIntWide{};
    static_assert(sizeof(UIntWide) == sizeof(bigend_bin));

    for (auto const walk : bigend_bin)
    {
        ret <<= 8;
        ret += static_cast<uint8_t>(walk);
    }

    return ret;
}

template<typename UIntWide>
auto export_bits(UIntWide i)
{
    auto ret = std::array<std::byte, UIntWide::my_width2 / std::numeric_limits<uint8_t>::digits>{};

    for (auto walk = std::rbegin(ret), end = std::rend(ret); walk != end; ++walk)
    {
        *walk = std::byte(static_cast<uint8_t>(i & 0xFF));
        i >>= 8;
    }

    return ret;
}

auto constexpr Generator = wi::key_t{ 2U };
// NOLINTBEGIN(readability-identifier-naming)
auto PRIME_CONSTEXPR Prime = wi::key_t{
    "0xFFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A63A36210000000000090563"
};
// NOLINTEND(readability-identifier-naming)

} // namespace wi
} // namespace

namespace tr_message_stream_encryption
{

// --- DH

[[nodiscard]] DH::private_key_bigend_t DH::randomPrivateKey() noexcept
{
    return tr_rand_obj<DH::private_key_bigend_t>();
}

DH::key_bigend_t DH::publicKey() noexcept
{
    if (public_key_ == key_bigend_t{})
    {
        auto const private_key_wi = wi::import_bits<wi::private_key_t>(private_key_);
        auto const public_key_wi = math::wide_integer::powm(wi::Generator, private_key_wi, wi::Prime);
        public_key_ = wi::export_bits(public_key_wi);
    }

    return public_key_;
}

void DH::setPeerPublicKey(key_bigend_t const& peer_public_key)
{
    auto const secret = math::wide_integer::powm(
        wi::import_bits<wi::key_t>(peer_public_key),
        wi::import_bits<wi::private_key_t>(private_key_),
        wi::Prime);
    secret_ = wi::export_bits(secret);
}

// --- Filter

void Filter::decrypt_init(bool is_incoming, DH const& dh, tr_sha1_digest_t const& info_hash)
{
    auto const key = is_incoming ? "keyA"sv : "keyB"sv;
    auto const buf = tr_sha1::digest(key, dh.secret(), info_hash);
    dec_active_ = true;
    dec_key_.init(std::data(buf), std::size(buf));
    dec_key_.discard(1024);
}

void Filter::encrypt_init(bool is_incoming, DH const& dh, tr_sha1_digest_t const& info_hash)
{
    auto const key = is_incoming ? "keyB"sv : "keyA"sv;
    auto const buf = tr_sha1::digest(key, dh.secret(), info_hash);
    enc_active_ = true;
    enc_key_.init(std::data(buf), std::size(buf));
    enc_key_.discard(1024);
}

} // namespace tr_message_stream_encryption
