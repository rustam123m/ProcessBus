#include "r_session_crypto.hpp"

#include <cstring>

namespace RSess
{

Crypto::Crypto()
{
    mbedtls_md_init(&m_md);
    mbedtls_gcm_init(&m_gcm);

    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr) {
        return;
    }
    // The 1 requests HMAC support; without it hmac_starts() fails.
    if (mbedtls_md_setup(&m_md, info, 1) != 0) {
        return;
    }
    if (mbedtls_md_hmac_starts(&m_md, LAB_KEY, HMAC_KEY_SIZE) != 0) {
        return;
    }
    if (mbedtls_gcm_setkey(&m_gcm, MBEDTLS_CIPHER_ID_AES, LAB_KEY, GCM_KEY_SIZE * 8) != 0) {
        return;
    }

    m_ready = true;
}

Crypto::~Crypto()
{
    mbedtls_gcm_free(&m_gcm);
    mbedtls_md_free(&m_md);
}

void Crypto::DeriveIV(uint32_t keyId, uint32_t spduNumber, uint8_t iv[IV_LEN_GCM])
{
    iv[0]  = static_cast< uint8_t >(keyId >> 24);
    iv[1]  = static_cast< uint8_t >(keyId >> 16);
    iv[2]  = static_cast< uint8_t >(keyId >> 8);
    iv[3]  = static_cast< uint8_t >(keyId);
    iv[4]  = 0;
    iv[5]  = 0;
    iv[6]  = 0;
    iv[7]  = 0;
    iv[8]  = static_cast< uint8_t >(spduNumber >> 24);
    iv[9]  = static_cast< uint8_t >(spduNumber >> 16);
    iv[10] = static_cast< uint8_t >(spduNumber >> 8);
    iv[11] = static_cast< uint8_t >(spduNumber);
}

bool Crypto::HMAC(const uint8_t* data, size_t size, uint8_t tag[TAG_SIZE])
{
    uint8_t digest[32];

    // hmac_reset() re-primes the inner pad from the key set in the constructor,
    // so the key schedule is not redone per packet.
    if (mbedtls_md_hmac_reset(&m_md) != 0) {
        return false;
    }
    if (mbedtls_md_hmac_update(&m_md, data, size) != 0) {
        return false;
    }
    if (mbedtls_md_hmac_finish(&m_md, digest) != 0) {
        return false;
    }

    std::memcpy(tag, digest, TAG_SIZE);
    return true;
}

bool Crypto::VerifyHMAC(const uint8_t* data, size_t size, const uint8_t* tag)
{
    uint8_t expected[TAG_SIZE];
    if (!HMAC(data, size, expected)) {
        return false;
    }

    // Constant-time compare: no early exit on the first differing byte.
    uint8_t diff = 0;
    for (size_t i=0;i<TAG_SIZE;++i) {
        diff |= static_cast< uint8_t >(expected[i] ^ tag[i]);
    }
    return diff == 0;
}

bool Crypto::Encrypt(const uint8_t* iv, size_t ivSize,
                     const uint8_t* aad, size_t aadSize,
                     uint8_t* data, size_t size, uint8_t tag[TAG_SIZE])
{
    // In-place: input and output point at the same buffer, as libiec61850 does
    // (r_session.c:1009 -> r_session_crypto_mbedtls.c:97).
    return mbedtls_gcm_crypt_and_tag(&m_gcm, MBEDTLS_GCM_ENCRYPT, size,
                                     iv, ivSize, aad, aadSize,
                                     data, data, TAG_SIZE, tag) == 0;
}

bool Crypto::Decrypt(const uint8_t* iv, size_t ivSize,
                     const uint8_t* aad, size_t aadSize,
                     uint8_t* data, size_t size,
                     const uint8_t* tag, size_t tagSize)
{
    return mbedtls_gcm_auth_decrypt(&m_gcm, size, iv, ivSize, aad, aadSize,
                                    tag, tagSize, data, data) == 0;
}

}
