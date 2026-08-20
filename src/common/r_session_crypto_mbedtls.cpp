// mbedtls backend for RSess::Crypto. Selected by -DCRYPTO_BACKEND=mbedtls.

#include "r_session_crypto.hpp"

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

#include <cstring>

namespace RSess
{

const char* Crypto::BackendName() { return "mbedtls"; }

struct Crypto::Impl
{
    mbedtls_md_context_t    md  = {};
    mbedtls_gcm_context     gcm = {};
};

Crypto::Crypto()
    : m_impl(std::make_unique< Impl >())
{
    mbedtls_md_init(&m_impl->md);
    mbedtls_gcm_init(&m_impl->gcm);

    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr) {
        return;
    }
    // The 1 requests HMAC support; without it hmac_starts() fails.
    if (mbedtls_md_setup(&m_impl->md, info, 1) != 0) {
        return;
    }
    if (mbedtls_md_hmac_starts(&m_impl->md, LAB_KEY, HMAC_KEY_SIZE) != 0) {
        return;
    }
    if (mbedtls_gcm_setkey(&m_impl->gcm, MBEDTLS_CIPHER_ID_AES, LAB_KEY, GCM_KEY_SIZE * 8) != 0) {
        return;
    }

    m_ready = true;
}

Crypto::~Crypto()
{
    mbedtls_gcm_free(&m_impl->gcm);
    mbedtls_md_free(&m_impl->md);
}

bool Crypto::HMAC(const uint8_t* data, size_t size, uint8_t tag[TAG_SIZE])
{
    uint8_t digest[32];

    // hmac_reset() re-primes the inner pad from the key set in the constructor,
    // so the key schedule is not redone per packet.
    if (mbedtls_md_hmac_reset(&m_impl->md) != 0) {
        return false;
    }
    if (mbedtls_md_hmac_update(&m_impl->md, data, size) != 0) {
        return false;
    }
    if (mbedtls_md_hmac_finish(&m_impl->md, digest) != 0) {
        return false;
    }

    std::memcpy(tag, digest, TAG_SIZE);
    return true;
}

bool Crypto::Encrypt(const uint8_t* iv, size_t ivSize,
                     const uint8_t* aad, size_t aadSize,
                     uint8_t* data, size_t size, uint8_t tag[TAG_SIZE])
{
    // In-place: input and output point at the same buffer, as libiec61850 does
    // (r_session.c:1009 -> r_session_crypto_mbedtls.c:97).
    return mbedtls_gcm_crypt_and_tag(&m_impl->gcm, MBEDTLS_GCM_ENCRYPT, size,
                                     iv, ivSize, aad, aadSize,
                                     data, data, TAG_SIZE, tag) == 0;
}

bool Crypto::Decrypt(const uint8_t* iv, size_t ivSize,
                     const uint8_t* aad, size_t aadSize,
                     uint8_t* data, size_t size,
                     const uint8_t* tag, size_t tagSize)
{
    return mbedtls_gcm_auth_decrypt(&m_impl->gcm, size, iv, ivSize, aad, aadSize,
                                    tag, tagSize, data, data) == 0;
}

}
