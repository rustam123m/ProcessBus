// OpenSSL backend for RSess::Crypto. Selected by -DCRYPTO_BACKEND=openssl.

#include "r_session_crypto.hpp"

#include <openssl/evp.h>

#include <cstring>

namespace RSess
{

const char* Crypto::BackendName() { return "openssl"; }

namespace
{
    constexpr size_t SHA256_BLOCK = 64;
    constexpr size_t DIGEST_LEN   = 32;

    /*
     * HMAC pads per RFC 2104: the key is shorter than the block, so it is
     * zero-padded rather than hashed.
     */
    void build_pads(uint8_t ipad[SHA256_BLOCK], uint8_t opad[SHA256_BLOCK])
    {
        std::memset(ipad, 0x36, SHA256_BLOCK);
        std::memset(opad, 0x5C, SHA256_BLOCK);
        for (size_t i=0;i<HMAC_KEY_SIZE;++i) {
            ipad[i] = static_cast< uint8_t >(ipad[i] ^ LAB_KEY[i]);
            opad[i] = static_cast< uint8_t >(opad[i] ^ LAB_KEY[i]);
        }
    }
}

/*
 * The HMAC pair holds the inner and outer SHA-256 midstates with the pads
 * already absorbed; each packet clones one instead of re-deriving the key
 * schedule. This is the analogue of mbedtls hmac_reset().
 *
 * GCM needs one context per direction: an EVP_CIPHER_CTX is bound to encrypt
 * or decrypt when the key is installed, and only the IV is rebound per packet.
 */
struct Crypto::Impl
{
    EVP_MD_CTX*     inner = nullptr;
    EVP_MD_CTX*     outer = nullptr;
    EVP_MD_CTX*     cur   = nullptr;
    EVP_CIPHER_CTX* enc   = nullptr;
    EVP_CIPHER_CTX* dec   = nullptr;
};

Crypto::Crypto()
    : m_impl(std::make_unique< Impl >())
{
    uint8_t ipad[SHA256_BLOCK], opad[SHA256_BLOCK];
    build_pads(ipad, opad);

    m_impl->inner = EVP_MD_CTX_new();
    m_impl->outer = EVP_MD_CTX_new();
    m_impl->cur   = EVP_MD_CTX_new();
    m_impl->enc   = EVP_CIPHER_CTX_new();
    m_impl->dec   = EVP_CIPHER_CTX_new();

    if (!m_impl->inner || !m_impl->outer || !m_impl->cur || !m_impl->enc || !m_impl->dec) {
        return;
    }

    if (EVP_DigestInit_ex(m_impl->inner, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(m_impl->inner, ipad, SHA256_BLOCK) != 1 ||
        EVP_DigestInit_ex(m_impl->outer, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(m_impl->outer, opad, SHA256_BLOCK) != 1) {
        return;
    }

    if (EVP_EncryptInit_ex(m_impl->enc, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(m_impl->enc, EVP_CTRL_GCM_SET_IVLEN, IV_LEN_GCM, nullptr) != 1 ||
        EVP_EncryptInit_ex(m_impl->enc, nullptr, nullptr, LAB_KEY, nullptr) != 1) {
        return;
    }
    if (EVP_DecryptInit_ex(m_impl->dec, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(m_impl->dec, EVP_CTRL_GCM_SET_IVLEN, IV_LEN_GCM, nullptr) != 1 ||
        EVP_DecryptInit_ex(m_impl->dec, nullptr, nullptr, LAB_KEY, nullptr) != 1) {
        return;
    }

    m_ready = true;
}

Crypto::~Crypto()
{
    EVP_CIPHER_CTX_free(m_impl->dec);
    EVP_CIPHER_CTX_free(m_impl->enc);
    EVP_MD_CTX_free(m_impl->cur);
    EVP_MD_CTX_free(m_impl->outer);
    EVP_MD_CTX_free(m_impl->inner);
}

bool Crypto::HMAC(const uint8_t* data, size_t size, uint8_t tag[TAG_SIZE])
{
    uint8_t  digest[DIGEST_LEN];
    unsigned len = 0;

    if (EVP_MD_CTX_copy_ex(m_impl->cur, m_impl->inner) != 1 ||
        EVP_DigestUpdate(m_impl->cur, data, size) != 1 ||
        EVP_DigestFinal_ex(m_impl->cur, digest, &len) != 1) {
        return false;
    }
    if (EVP_MD_CTX_copy_ex(m_impl->cur, m_impl->outer) != 1 ||
        EVP_DigestUpdate(m_impl->cur, digest, DIGEST_LEN) != 1 ||
        EVP_DigestFinal_ex(m_impl->cur, digest, &len) != 1) {
        return false;
    }

    std::memcpy(tag, digest, TAG_SIZE);
    return true;
}

bool Crypto::Encrypt(const uint8_t* iv, size_t ivSize,
                     const uint8_t* aad, size_t aadSize,
                     uint8_t* data, size_t size, uint8_t tag[TAG_SIZE])
{
    int len = 0;

    if (ivSize != IV_LEN_GCM) {
        return false;
    }
    // Only the IV is rebound; the key schedule stays from the constructor.
    if (EVP_EncryptInit_ex(m_impl->enc, nullptr, nullptr, nullptr, iv) != 1) {
        return false;
    }
    if (aadSize > 0 &&
        EVP_EncryptUpdate(m_impl->enc, nullptr, &len, aad, static_cast< int >(aadSize)) != 1) {
        return false;
    }
    // In-place, as the mbedtls backend and libiec61850 both do.
    if (EVP_EncryptUpdate(m_impl->enc, data, &len, data, static_cast< int >(size)) != 1) {
        return false;
    }
    if (EVP_EncryptFinal_ex(m_impl->enc, data + len, &len) != 1) {
        return false;
    }
    return EVP_CIPHER_CTX_ctrl(m_impl->enc, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag) == 1;
}

bool Crypto::Decrypt(const uint8_t* iv, size_t ivSize,
                     const uint8_t* aad, size_t aadSize,
                     uint8_t* data, size_t size,
                     const uint8_t* tag, size_t tagSize)
{
    int len = 0;

    if (ivSize != IV_LEN_GCM || tagSize != TAG_SIZE) {
        return false;
    }
    if (EVP_DecryptInit_ex(m_impl->dec, nullptr, nullptr, nullptr, iv) != 1) {
        return false;
    }
    if (aadSize > 0 &&
        EVP_DecryptUpdate(m_impl->dec, nullptr, &len, aad, static_cast< int >(aadSize)) != 1) {
        return false;
    }
    if (EVP_DecryptUpdate(m_impl->dec, data, &len, data, static_cast< int >(size)) != 1) {
        return false;
    }
    /*
     * The tag must be installed before the final call, whose return value is
     * the authentication verdict. A negative result here means the frame is
     * forged, so the caller must discard it.
     */
    if (EVP_CIPHER_CTX_ctrl(m_impl->dec, EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                            const_cast< uint8_t* >(tag)) != 1) {
        return false;
    }
    return EVP_DecryptFinal_ex(m_impl->dec, data + len, &len) == 1;
}

}
