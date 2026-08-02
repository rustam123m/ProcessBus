#include "r_session.hpp"
#include "r_session_crypto.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
    // Plain byte writes keep r_session free of any DPDK dependency.
    inline void put_u16(uint8_t* p, uint16_t v)
    {
        p[0] = static_cast< uint8_t >(v >> 8);
        p[1] = static_cast< uint8_t >(v);
    }

    inline void put_u32(uint8_t* p, uint32_t v)
    {
        p[0] = static_cast< uint8_t >(v >> 24);
        p[1] = static_cast< uint8_t >(v >> 16);
        p[2] = static_cast< uint8_t >(v >> 8);
        p[3] = static_cast< uint8_t >(v);
    }

    inline uint16_t get_u16(const uint8_t* p)
    {
        return static_cast< uint16_t >((static_cast< uint16_t >(p[0]) << 8) | p[1]);
    }

    inline uint32_t get_u32(const uint8_t* p)
    {
        return (static_cast< uint32_t >(p[0]) << 24)
             | (static_cast< uint32_t >(p[1]) << 16)
             | (static_cast< uint32_t >(p[2]) << 8)
             |  static_cast< uint32_t >(p[3]);
    }
}

namespace RSess
{

SecurityMode parse_security_mode(std::string_view text)
{
    if (text == "none") {
        return SEC_NONE;
    }
    if (text == "hmac") {
        return SEC_HMAC;
    }
    if (text == "gcm") {
        return SEC_GCM;
    }
    throw std::invalid_argument("Unknown R security mode '" + std::string(text)
                                + "', expected none|hmac|gcm");
}

const char* to_string(SecurityMode mode)
{
    switch (mode) {
    case SEC_NONE: return "none";
    case SEC_HMAC: return "hmac-sha256-128";
    case SEC_GCM:  return "aes-128-gcm";
    }
    return "?";
}

size_t build(uint8_t* buf, const BuildParams& params)
{
    const uint8_t  ivLen = iv_length(params.mode);
    const uint32_t payloadLength = static_cast< uint32_t >(PAYLOAD_ELEM_SIZE) + params.apduLength;

    buf[OFF_CLTP_LI]    = CLTP_LI;
    buf[OFF_CLTP_UD]    = CLTP_UD;
    buf[OFF_SI]         = params.si;
    buf[OFF_SESSION_LI] = SESSION_HDR_LI;
    buf[OFF_PI]         = PI_COMMON_HDR;
    buf[OFF_LI2]        = COMMON_HDR_LI2;

    // Mirrors r_session.c:869 verbatim, IV bytes excluded. Informational only.
    put_u32(buf + OFF_SPDU_LENGTH, 20 + payloadLength);
    put_u32(buf + OFF_SPDU_NUMBER, params.spduNumber);
    put_u16(buf + OFF_VERSION, SESSION_VERSION);
    put_u32(buf + OFF_TIME_CUR_KEY, 0);
    put_u16(buf + OFF_TIME_NEXT_KEY, 0);
    put_u32(buf + OFF_KEY_ID, params.keyId);
    buf[OFF_IV_LENGTH]  = ivLen;
    if (ivLen > 0) {
        // Real contents are written per packet by seal(), from the SPDU number.
        std::memset(buf + OFF_IV, 0, ivLen);
    }

    uint8_t* phdr = buf + payload_hdr_offset(ivLen);
    put_u32(phdr, payloadLength);
    phdr[4] = params.payloadType;
    phdr[5] = params.simulation ? 1 : 0;
    put_u16(phdr + 6, params.appid);
    // True APDU length; libiec61850 writes payloadSize + 2 (r_session.c:950).
    put_u16(phdr + 8, params.apduLength);

    return apdu_offset(ivLen);
}

bool seal(uint8_t* buf, SecurityMode mode, Crypto& crypto)
{
    if (mode == SEC_NONE) {
        return true;
    }

    const uint8_t  ivLen = buf[OFF_IV_LENGTH];
    const size_t   phOff = payload_hdr_offset(ivLen);
    const uint32_t payloadLength = get_u32(buf + phOff);
    // Payload elements start after the PayloadLength field (r_session.c:933).
    const size_t   payloadStart = phOff + 4;
    const size_t   payloadEnd = payloadStart + payloadLength;

    buf[payloadEnd]     = SIGNATURE_TAG;
    // Encoder hardcodes 16 (r_session.c:969,978); we only emit 128-bit tags.
    buf[payloadEnd + 1] = static_cast< uint8_t >(TAG_SIZE);
    uint8_t* tag = buf + payloadEnd + 2;

    if (mode == SEC_HMAC) {
        // Authenticated range is the whole UDP payload (r_session.c:961).
        return crypto.HMAC(buf, payloadEnd, tag);
    }

    // SEC_GCM
    uint8_t* iv = buf + OFF_IV;
    Crypto::DeriveIV(get_u32(buf + OFF_KEY_ID), get_u32(buf + OFF_SPDU_NUMBER), iv);

    /*
     * AAD covers everything up to the payload elements; the ciphertext covers
     * the payload header and the APDU (r_session.c:1000,1001).
     */
    return crypto.Encrypt(iv, ivLen,
                          buf, payloadStart,
                          buf + payloadStart, payloadLength,
                          tag);
}

int parse_session(const uint8_t* buf, size_t size, SessionHeader& session)
{
    if (size < SESSION_FIXED_SIZE + 4) {
        return ERR_TOO_SHORT;
    }
    if (buf[OFF_CLTP_LI] != CLTP_LI || buf[OFF_CLTP_UD] != CLTP_UD) {
        return ERR_BAD_CLTP;
    }

    session.si = buf[OFF_SI];
    if (session.si != SI_R_GOOSE && session.si != SI_R_SV) {
        return ERR_BAD_SI;
    }
    if (buf[OFF_PI] != PI_COMMON_HDR) {
        return ERR_BAD_PI;
    }

    session.version = get_u16(buf + OFF_VERSION);
    if (session.version != SESSION_VERSION) {
        return ERR_BAD_VERSION;
    }

    session.spduLength       = get_u32(buf + OFF_SPDU_LENGTH);
    session.spduNumber       = get_u32(buf + OFF_SPDU_NUMBER);
    session.timeOfCurrentKey = get_u32(buf + OFF_TIME_CUR_KEY);
    session.timeToNextKey    = static_cast< int16_t >(get_u16(buf + OFF_TIME_NEXT_KEY));
    session.keyId            = get_u32(buf + OFF_KEY_ID);
    session.ivLength         = buf[OFF_IV_LENGTH];
    session.iv               = (session.ivLength > 0) ? (buf + OFF_IV) : nullptr;

    const size_t phOff = payload_hdr_offset(session.ivLength);
    if (phOff + PAYLOAD_HDR_SIZE > size) {
        return ERR_TOO_SHORT;
    }

    session.payloadLength = get_u32(buf + phOff);
    if (session.payloadLength < PAYLOAD_ELEM_SIZE
        || phOff + 4 + session.payloadLength > size) {
        return ERR_BAD_PAYLOAD_LEN;
    }

    return static_cast< int >(phOff);
}

bool unseal(uint8_t* buf, size_t size, SecurityMode mode,
            const SessionHeader& session, Crypto& crypto)
{
    if (mode == SEC_NONE) {
        return true;
    }

    const size_t phOff = payload_hdr_offset(session.ivLength);
    const size_t payloadStart = phOff + 4;
    const size_t payloadEnd = payloadStart + session.payloadLength;

    if (payloadEnd + TRAILER_SIZE > size) {
        return false;
    }
    if (buf[payloadEnd] != SIGNATURE_TAG) {
        return false;
    }
    const uint8_t* tag = buf + payloadEnd + 2;

    if (mode == SEC_HMAC) {
        return crypto.VerifyHMAC(buf, payloadEnd, tag);
    }

    // SEC_GCM — the IV travels in the header, so a random-IV sender interoperates.
    if (session.ivLength != IV_LEN_GCM || session.iv == nullptr) {
        return false;
    }
    const size_t tagSize = buf[payloadEnd + 1];
    if (tagSize != TAG_SIZE) {
        return false;
    }

    return crypto.Decrypt(session.iv, session.ivLength,
                          buf, payloadStart,
                          buf + payloadStart, session.payloadLength,
                          tag, tagSize);
}

int parse_payload(const uint8_t* buf, size_t size, const SessionHeader& session,
                  PayloadHeader& payload)
{
    const size_t phOff = payload_hdr_offset(session.ivLength);
    if (phOff + PAYLOAD_HDR_SIZE > size) {
        return ERR_TOO_SHORT;
    }

    const uint8_t* phdr = buf + phOff;
    payload.payloadType = phdr[4];
    if (payload.payloadType != PAYLOAD_TYPE_GOOSE
        && payload.payloadType != PAYLOAD_TYPE_SV) {
        return ERR_BAD_PAYLOAD_TYPE;
    }
    payload.simulation = phdr[5];
    payload.appid      = get_u16(phdr + 6);
    payload.apduLength = get_u16(phdr + 8);
    payload.apduSize   = session.payloadLength - static_cast< uint32_t >(PAYLOAD_ELEM_SIZE);

    return static_cast< int >(phOff + PAYLOAD_HDR_SIZE);
}

int parse(const uint8_t* buf, size_t size,
          SessionHeader& session, PayloadHeader& payload)
{
    const int phOff = parse_session(buf, size, session);
    if (phOff < 0) {
        return phOff;
    }
    return parse_payload(buf, size, session, payload);
}

}
