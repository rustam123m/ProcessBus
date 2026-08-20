// Backend-neutral half of RSess::Crypto: byte math and the tag compare.

#include "r_session_crypto.hpp"

namespace RSess
{

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

}
