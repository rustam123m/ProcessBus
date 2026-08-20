#pragma once

#include "r_session.hpp"

#include <cstdint>
#include <memory>

namespace RSess
{
    // Fixed lab key; AES-128-GCM uses the first GCM_KEY_SIZE bytes.
    constexpr size_t HMAC_KEY_SIZE = 32;
    constexpr size_t GCM_KEY_SIZE  = 16;   // AES-128; the backend picks AES-128/256 by key size

    constexpr uint8_t LAB_KEY[HMAC_KEY_SIZE] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };

    class Crypto
    {
    public:
        Crypto();
        ~Crypto();

        Crypto(const Crypto&) = delete;
        Crypto& operator=(const Crypto&) = delete;

        bool IsReady() const { return m_ready; }

        // "mbedtls" or "openssl", fixed at compile time by CRYPTO_BACKEND.
        // Printed at start-up so a captured run records which one produced it.
        static const char* BackendName();

        // 12-byte deterministic IV: KeyID(4) || 0x00000000 || SPDUNumber(4).
        static void DeriveIV(uint32_t keyId, uint32_t spduNumber, uint8_t iv[IV_LEN_GCM]);

        // HMAC-SHA256 truncated to 128 bit (r_session.c:981).
        bool HMAC(const uint8_t* data, size_t size, uint8_t tag[TAG_SIZE]);
        bool VerifyHMAC(const uint8_t* data, size_t size, const uint8_t* tag);

        // AES-128-GCM, encrypts data in place.
        bool Encrypt(const uint8_t* iv, size_t ivSize,
                     const uint8_t* aad, size_t aadSize,
                     uint8_t* data, size_t size, uint8_t tag[TAG_SIZE]);
        bool Decrypt(const uint8_t* iv, size_t ivSize,
                     const uint8_t* aad, size_t aadSize,
                     uint8_t* data, size_t size,
                     const uint8_t* tag, size_t tagSize);

    private:
        /*
         * Backend state lives behind a pimpl so this header pulls in neither
         * mbedtls nor OpenSSL headers. Exactly one of
         * r_session_crypto_{mbedtls,openssl}.cpp defines Impl and the
         * destructor, selected by CRYPTO_BACKEND at configure time.
         */
        struct Impl;
        std::unique_ptr< Impl > m_impl;
        bool                    m_ready = false;
    };
}
