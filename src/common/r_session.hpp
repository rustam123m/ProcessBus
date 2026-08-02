#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace RSess
{
    // Security modes implemented here (a subset of what 90-5 defines).
    enum SecurityMode : uint8_t
    {
        SEC_NONE = 0,       // no signature, no encryption
        SEC_HMAC = 1,       // HMAC-SHA256-128: authenticate, do not encrypt
        SEC_GCM  = 2,       // AES-128-GCM: encrypt payload header + APDU, authenticate
    };

    // --- Protocol constants (version 2) ---
    constexpr uint8_t  CLTP_LI              = 0x01;
    constexpr uint8_t  CLTP_UD              = 0x40;
    constexpr uint8_t  SI_R_GOOSE           = 0xA1;
    constexpr uint8_t  SI_R_SV              = 0xA2;
    constexpr uint8_t  SESSION_HDR_LI       = 24;   // r_session.c:860
    constexpr uint8_t  PI_COMMON_HDR        = 0x80;
    constexpr uint8_t  COMMON_HDR_LI2       = 18;   // r_session.c:866 (never trust on RX)
    constexpr uint16_t SESSION_VERSION      = 2;
    constexpr uint8_t  PAYLOAD_TYPE_GOOSE   = 0x81;
    constexpr uint8_t  PAYLOAD_TYPE_SV      = 0x82;
    constexpr uint8_t  SIGNATURE_TAG        = 0x85;
    constexpr uint16_t UDP_DST_PORT         = 102;

    // Non-zero: the version-2 receiver rejects KeyID 0 outright (r_session.c:661).
    constexpr uint32_t KEY_ID               = 1;

    constexpr uint8_t  IV_LEN_NONE          = 0;
    constexpr uint8_t  IV_LEN_GCM           = 12;   // r_session.c:912
    constexpr size_t   TAG_SIZE             = 16;   // HMAC-128 tag and GCM tag alike
    constexpr size_t   TRAILER_SIZE         = 2 + TAG_SIZE;  // 0x85, length byte, tag

    // --- Fixed offsets, relative to the UDP payload (= the CLTP byte) ---
    enum Offset : size_t
    {
        OFF_CLTP_LI     = 0,
        OFF_CLTP_UD     = 1,
        OFF_SI          = 2,
        OFF_SESSION_LI  = 3,
        OFF_PI          = 4,
        OFF_LI2         = 5,
        OFF_SPDU_LENGTH = 6,
        OFF_SPDU_NUMBER = 10,
        OFF_VERSION     = 14,
        OFF_TIME_CUR_KEY= 16,
        OFF_TIME_NEXT_KEY = 20,
        OFF_KEY_ID      = 22,
        OFF_IV_LENGTH   = 26,
        OFF_IV          = 27,   // only when the IV length byte is non-zero
    };

    // Bytes from the CLTP byte through the IV-length byte inclusive.
    constexpr size_t SESSION_FIXED_SIZE = 27;
    // PayloadLength(4) + Type(1) + Simulation(1) + APPID(2) + APDULength(2)
    constexpr size_t PAYLOAD_HDR_SIZE   = 10;
    // Per-element prefix inside PayloadLength: Type + Simulation + APPID + APDULength
    constexpr size_t PAYLOAD_ELEM_SIZE  = 6;

    // Ethernet + IPv4 + UDP bytes in front of the UDP payload (no VLAN, IHL=5).
    constexpr size_t ETH_IP_UDP_SIZE    = 14 + 20 + 8;

    constexpr uint8_t iv_length(SecurityMode mode)
    {
        return (mode == SEC_GCM) ? IV_LEN_GCM : IV_LEN_NONE;
    }

    // Offset of the payload header (its PayloadLength field) from the CLTP byte.
    constexpr size_t payload_hdr_offset(uint8_t ivLen)
    {
        return SESSION_FIXED_SIZE + ivLen;
    }

    // Offset of the APDU from the CLTP byte: 37 without an IV, 49 with the GCM IV.
    constexpr size_t apdu_offset(uint8_t ivLen)
    {
        return payload_hdr_offset(ivLen) + PAYLOAD_HDR_SIZE;
    }

    constexpr size_t trailer_size(SecurityMode mode)
    {
        return (mode == SEC_NONE) ? 0 : TRAILER_SIZE;
    }

    // Total UDP payload size for one APDU.
    constexpr size_t udp_payload_size(SecurityMode mode, size_t apduLength)
    {
        return apdu_offset(iv_length(mode)) + apduLength + trailer_size(mode);
    }

    // Offset of the payload-header APPID field from the CLTP byte (33 or 45).
    constexpr size_t appid_offset(uint8_t ivLen)
    {
        return payload_hdr_offset(ivLen) + 4 /* PayloadLength */ + 1 /* Type */ + 1 /* Simulation */;
    }

    struct SessionHeader
    {
        uint8_t     si = 0;                 // 0xA1 R-GOOSE / 0xA2 R-SV
        uint32_t    spduLength = 0;
        uint32_t    spduNumber = 0;
        uint16_t    version = 0;
        uint32_t    timeOfCurrentKey = 0;
        int16_t     timeToNextKey = 0;
        uint32_t    keyId = 0;
        uint8_t     ivLength = 0;
        const uint8_t* iv = nullptr;        // points into the frame, null when ivLength == 0
        uint32_t    payloadLength = 0;      // bytes of payload elements, excludes the trailer
    };

    struct PayloadHeader
    {
        uint8_t     payloadType = 0;        // 0x81 GOOSE / 0x82 SV
        uint8_t     simulation = 0;
        uint16_t    appid = 0;
        uint16_t    apduLength = 0;         // raw wire value, see apduSize
        uint32_t    apduSize = 0;           // trustworthy APDU size, derived from payloadLength
    };

    enum ParseError : int
    {
        ERR_TOO_SHORT       = -1,
        ERR_BAD_CLTP        = -2,
        ERR_BAD_SI          = -3,
        ERR_BAD_PI          = -4,
        ERR_BAD_VERSION     = -5,
        ERR_BAD_PAYLOAD_LEN = -6,
        ERR_BAD_PAYLOAD_TYPE= -7,
        ERR_NO_TRAILER      = -8,
        ERR_AUTH_FAILED     = -9,
    };

    struct BuildParams
    {
        uint8_t         si = SI_R_GOOSE;
        uint8_t         payloadType = PAYLOAD_TYPE_GOOSE;
        SecurityMode    mode = SEC_NONE;
        uint32_t        spduNumber = 0;
        uint32_t        keyId = KEY_ID;
        uint16_t        appid = 0;
        uint16_t        apduLength = 0;
        bool            simulation = false;
    };

    /*
     * Write the version-2 envelope at the CLTP byte.
     * buf start of the UDP payload; must hold udp_payload_size() bytes
     * Returns offset of the APDU from buf (37 or 49)
     */
    size_t build(uint8_t* buf, const BuildParams& params);

    // Parse a `--r-mode` argument. Throws std::invalid_argument on anything else.
    SecurityMode parse_security_mode(std::string_view text);
    const char*  to_string(SecurityMode mode);

    class Crypto;

    /*
     * Apply the security trailer to a finished frame.
     * buf    start of the UDP payload, envelope and APDU already written
     * mode   must match the mode passed to build(). No-op for SEC_NONE.
     */
    bool seal(uint8_t* buf, SecurityMode mode, Crypto& crypto);

    /*
     * Parse the session header only, stopping at the payload length.
     * Returns offset of the payload header (its PayloadLength field), or a
     *         negative ParseError. Every read is bound by size.
     */
    int parse_session(const uint8_t* buf, size_t size, SessionHeader& session);

    /*
     * Verify, and for SEC_GCM decrypt in place, a received frame.
     * buf     start of the UDP payload (mutable: GCM decrypts in place)
     * session filled in by parse_session()
     * Returns true when the frame is authentic
     */
    bool unseal(uint8_t* buf, size_t size, SecurityMode mode,
                const SessionHeader& session, Crypto& crypto);

    /*
     * Read the payload header once it is known to be plaintext.
     * Returns offset of the APDU from buf, or a negative ParseError
     *
     * libiec61850 writes `payloadSize + 2` into the APDU-length field
     * (r_session.c:950), so bounds come from PayloadHeader::apduSize.
     */
    int parse_payload(const uint8_t* buf, size_t size, const SessionHeader& session,
                      PayloadHeader& payload);

    /*
     * parse_session() + parse_payload() for unencrypted frames.
     * Returns offset of the APDU from buf, or a negative ParseError
     */
    int parse(const uint8_t* buf, size_t size,
              SessionHeader& session, PayloadHeader& payload);
}
