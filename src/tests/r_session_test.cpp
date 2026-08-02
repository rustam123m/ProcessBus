/*
 * Byte-layout lock and round-trip tests for the R-GOOSE/R-SV session layer.
 *
 * Offsets are absolute on purpose: a typo in a constant would otherwise cancel out.
 */

#include <gtest/gtest.h>

#include "common/r_session.hpp"
#include "common/r_session_crypto.hpp"

#include <cstring>
#include <vector>

namespace
{
    constexpr uint16_t TEST_APPID = 0x1234;
    constexpr uint32_t TEST_SPDU  = 0x00ABCDEF;

    // A recognisable stand-in for a GOOSE APDU (starts with the 0x61 tag).
    std::vector< uint8_t > make_apdu(size_t size)
    {
        std::vector< uint8_t > apdu(size);
        apdu[0] = 0x61;
        for (size_t i=1;i<size;++i) {
            apdu[i] = static_cast< uint8_t >(0xA0 + (i & 0x0F));
        }
        return apdu;
    }

    uint16_t be16(const uint8_t* p) { return static_cast< uint16_t >((p[0] << 8) | p[1]); }
    uint32_t be32(const uint8_t* p) {
        return (static_cast< uint32_t >(p[0]) << 24) | (static_cast< uint32_t >(p[1]) << 16)
             | (static_cast< uint32_t >(p[2]) << 8)  |  static_cast< uint32_t >(p[3]);
    }

    // Build a complete UDP payload: envelope + APDU + security trailer.
    std::vector< uint8_t > make_frame(RSess::SecurityMode mode, uint8_t si,
                                      uint8_t payloadType,
                                      const std::vector< uint8_t >& apdu,
                                      RSess::Crypto& crypto,
                                      uint32_t spduNumber = TEST_SPDU,
                                      uint16_t appid = TEST_APPID)
    {
        std::vector< uint8_t > buf(
            RSess::udp_payload_size(mode, apdu.size()), 0);

        RSess::BuildParams params;
        params.si          = si;
        params.payloadType = payloadType;
        params.mode        = mode;
        params.spduNumber  = spduNumber;
        params.appid       = appid;
        params.apduLength  = static_cast< uint16_t >(apdu.size());

        const size_t apduOff = RSess::build(buf.data(), params);
        std::memcpy(buf.data() + apduOff, apdu.data(), apdu.size());

        EXPECT_TRUE(RSess::seal(buf.data(), mode, crypto));
        return buf;
    }
}

// --- Layout lock -------------------------------------------------------------

class RSessionLayout : public ::testing::TestWithParam< RSess::SecurityMode >
{
};

TEST_P(RSessionLayout, FieldPositionsAreFixed)
{
    const RSess::SecurityMode mode = GetParam();
    const uint8_t expectedIvLen = (mode == RSess::SEC_GCM) ? 12 : 0;
    const size_t  expectedApdu  = (mode == RSess::SEC_GCM) ? 49 : 37;
    const size_t  expectedAppid = (mode == RSess::SEC_GCM) ? 45 : 33;

    const std::vector< uint8_t > apdu = make_apdu(64);

    std::vector< uint8_t > buf(RSess::udp_payload_size(mode, apdu.size()), 0);

    RSess::BuildParams params;
    params.si          = RSess::SI_R_GOOSE;
    params.payloadType = RSess::PAYLOAD_TYPE_GOOSE;
    params.mode        = mode;
    params.spduNumber  = TEST_SPDU;
    params.appid       = TEST_APPID;
    params.apduLength  = static_cast< uint16_t >(apdu.size());

    const size_t apduOff = RSess::build(buf.data(), params);

    ASSERT_EQ(apduOff, expectedApdu);
    ASSERT_EQ(RSess::apdu_offset(expectedIvLen), expectedApdu);
    ASSERT_EQ(RSess::appid_offset(expectedIvLen), expectedAppid);

    // CLTP
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[1], 0x40);
    // Session header
    EXPECT_EQ(buf[2], 0xA1);        // SI = R-GOOSE
    EXPECT_EQ(buf[3], 24);          // session header length
    EXPECT_EQ(buf[4], 0x80);        // common session header tag
    EXPECT_EQ(buf[5], 18);          // common session header length
    EXPECT_EQ(be32(&buf[6]), 20u + 6u + apdu.size());   // SPDU length
    EXPECT_EQ(be32(&buf[10]), TEST_SPDU);               // SPDU number
    EXPECT_EQ(be16(&buf[14]), 2u);                      // protocol version
    EXPECT_EQ(be32(&buf[16]), 0u);                      // TimeOfCurrentKey
    EXPECT_EQ(be16(&buf[20]), 0u);                      // TimeToNextKey
    EXPECT_EQ(be32(&buf[22]), 1u);                      // KeyID (non-zero!)
    EXPECT_EQ(buf[26], expectedIvLen);                  // IV length

    // Payload header
    const size_t ph = 27u + expectedIvLen;
    EXPECT_EQ(be32(&buf[ph]), 6u + apdu.size());        // PayloadLength
    EXPECT_EQ(buf[ph + 4], 0x81);                       // payload type = GOOSE
    EXPECT_EQ(buf[ph + 5], 0x00);                       // simulation
    EXPECT_EQ(be16(&buf[ph + 6]), TEST_APPID);          // APPID
    EXPECT_EQ(be16(&buf[ph + 8]), apdu.size());         // APDU length (true value)
    EXPECT_EQ(ph + 6, expectedAppid);

    // Total size, including the 18-byte security trailer where present
    const size_t expectedTotal = expectedApdu + apdu.size()
                               + ((mode == RSess::SEC_NONE) ? 0 : 18);
    EXPECT_EQ(buf.size(), expectedTotal);
}

INSTANTIATE_TEST_SUITE_P(AllModes, RSessionLayout,
                         ::testing::Values(RSess::SEC_NONE,
                                           RSess::SEC_HMAC,
                                           RSess::SEC_GCM));

TEST(RSessionLayout, EthRelativeOffsetsMatchTheDocumentedOnes)
{
    // Eth(14) + IPv4(20) + UDP(8) = 42, assuming no VLAN and IHL = 5.
    ASSERT_EQ(RSess::ETH_IP_UDP_SIZE, 42u);

    EXPECT_EQ(RSess::ETH_IP_UDP_SIZE + RSess::appid_offset(0), 75u);
    EXPECT_EQ(RSess::ETH_IP_UDP_SIZE + RSess::apdu_offset(0), 79u);
    // AES-GCM shifts everything by the IV-length byte's 12 bytes.
    EXPECT_EQ(RSess::ETH_IP_UDP_SIZE + RSess::appid_offset(12), 87u);
    EXPECT_EQ(RSess::ETH_IP_UDP_SIZE + RSess::apdu_offset(12), 91u);
}

// --- Round trip --------------------------------------------------------------

class RSessionRoundTrip : public ::testing::TestWithParam< RSess::SecurityMode >
{
};

TEST_P(RSessionRoundTrip, BuildSealUnsealParse)
{
    const RSess::SecurityMode mode = GetParam();
    RSess::Crypto crypto;
    ASSERT_TRUE(crypto.IsReady());

    const std::vector< uint8_t > apdu = make_apdu(96);
    std::vector< uint8_t > buf = make_frame(mode, RSess::SI_R_GOOSE,
                                            RSess::PAYLOAD_TYPE_GOOSE,
                                            apdu, crypto);

    RSess::SessionHeader session;
    const int phOff = RSess::parse_session(buf.data(), buf.size(), session);
    ASSERT_GE(phOff, 0);
    EXPECT_EQ(session.si, RSess::SI_R_GOOSE);
    EXPECT_EQ(session.version, 2);
    EXPECT_EQ(session.spduNumber, TEST_SPDU);
    EXPECT_EQ(session.keyId, 1u);
    EXPECT_EQ(session.payloadLength, 6u + apdu.size());

    ASSERT_TRUE(RSess::unseal(buf.data(), buf.size(), mode, session, crypto));

    RSess::PayloadHeader payload;
    const int apduOff = RSess::parse_payload(buf.data(), buf.size(), session, payload);
    ASSERT_GE(apduOff, 0);
    EXPECT_EQ(payload.payloadType, RSess::PAYLOAD_TYPE_GOOSE);
    EXPECT_EQ(payload.appid, TEST_APPID);
    EXPECT_EQ(payload.apduSize, apdu.size());

    EXPECT_EQ(0, std::memcmp(buf.data() + apduOff, apdu.data(), apdu.size()))
        << "recovered APDU differs from the original";
}

INSTANTIATE_TEST_SUITE_P(AllModes, RSessionRoundTrip,
                         ::testing::Values(RSess::SEC_NONE,
                                           RSess::SEC_HMAC,
                                           RSess::SEC_GCM));

TEST(RSessionRoundTrip, GcmEncryptsThePayloadHeaderAndApdu)
{
    RSess::Crypto crypto;
    ASSERT_TRUE(crypto.IsReady());

    const std::vector< uint8_t > apdu = make_apdu(96);
    const std::vector< uint8_t > buf = make_frame(RSess::SEC_GCM,
                                                  RSess::SI_R_GOOSE,
                                                  RSess::PAYLOAD_TYPE_GOOSE,
                                                  apdu, crypto);

    // The APDU must not be readable on the wire...
    EXPECT_NE(0, std::memcmp(buf.data() + 49, apdu.data(), apdu.size()));
    // ...and neither must the APPID, which is why software RSS cannot key on it
    // for GCM frames (see ProcessBusParser::get_proto_type).
    const uint16_t wireAppid = be16(&buf[45]);
    EXPECT_NE(wireAppid, TEST_APPID);

    // The session header stays in clear: it is the AAD.
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(be32(&buf[10]), TEST_SPDU);
}

TEST(RSessionRoundTrip, GcmIvIsDerivedFromKeyIdAndSpduNumber)
{
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(32);
    const std::vector< uint8_t > buf = make_frame(RSess::SEC_GCM,
                                                  RSess::SI_R_GOOSE,
                                                  RSess::PAYLOAD_TYPE_GOOSE,
                                                  apdu, crypto);

    uint8_t expected[RSess::IV_LEN_GCM];
    RSess::Crypto::DeriveIV(RSess::KEY_ID, TEST_SPDU, expected);

    EXPECT_EQ(0, std::memcmp(buf.data() + 27, expected, sizeof(expected)));
    // KeyID(4) || 0(4) || SPDU number(4)
    EXPECT_EQ(be32(expected), RSess::KEY_ID);
    EXPECT_EQ(be32(expected + 4), 0u);
    EXPECT_EQ(be32(expected + 8), TEST_SPDU);
}

TEST(RSessionRoundTrip, SvUsesItsOwnSiAndPayloadType)
{
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(80);
    const std::vector< uint8_t > buf = make_frame(RSess::SEC_NONE,
                                                  RSess::SI_R_SV,
                                                  RSess::PAYLOAD_TYPE_SV,
                                                  apdu, crypto);
    EXPECT_EQ(buf[2], 0xA2);
    EXPECT_EQ(buf[31], 0x82);
}

// --- Negative tests ----------------------------------------------------------

TEST(RSessionNegative, TamperedApduFailsHmac)
{
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(64);
    std::vector< uint8_t > buf = make_frame(RSess::SEC_HMAC,
                                            RSess::SI_R_GOOSE,
                                            RSess::PAYLOAD_TYPE_GOOSE,
                                            apdu, crypto);

    buf[45] ^= 0x01;   // one bit inside the APDU

    RSess::SessionHeader session;
    ASSERT_GE(RSess::parse_session(buf.data(), buf.size(), session), 0);
    EXPECT_FALSE(RSess::unseal(buf.data(), buf.size(),
                                  RSess::SEC_HMAC, session, crypto));
}

TEST(RSessionNegative, TamperedCiphertextFailsGcm)
{
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(64);
    std::vector< uint8_t > buf = make_frame(RSess::SEC_GCM,
                                            RSess::SI_R_GOOSE,
                                            RSess::PAYLOAD_TYPE_GOOSE,
                                            apdu, crypto);

    buf[60] ^= 0x01;

    RSess::SessionHeader session;
    ASSERT_GE(RSess::parse_session(buf.data(), buf.size(), session), 0);
    EXPECT_FALSE(RSess::unseal(buf.data(), buf.size(),
                                  RSess::SEC_GCM, session, crypto));
}

TEST(RSessionNegative, TamperedSessionHeaderFailsGcmBecauseItIsAad)
{
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(64);
    std::vector< uint8_t > buf = make_frame(RSess::SEC_GCM,
                                            RSess::SI_R_GOOSE,
                                            RSess::PAYLOAD_TYPE_GOOSE,
                                            apdu, crypto);

    buf[16] ^= 0xFF;   // TimeOfCurrentKey: authenticated, not encrypted

    RSess::SessionHeader session;
    ASSERT_GE(RSess::parse_session(buf.data(), buf.size(), session), 0);
    EXPECT_FALSE(RSess::unseal(buf.data(), buf.size(),
                                  RSess::SEC_GCM, session, crypto));
}

TEST(RSessionNegative, MissingTrailerIsRejected)
{
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(64);
    std::vector< uint8_t > buf = make_frame(RSess::SEC_HMAC,
                                            RSess::SI_R_GOOSE,
                                            RSess::PAYLOAD_TYPE_GOOSE,
                                            apdu, crypto);

    RSess::SessionHeader session;
    ASSERT_GE(RSess::parse_session(buf.data(), buf.size(), session), 0);

    // Truncate the trailer away
    EXPECT_FALSE(RSess::unseal(buf.data(), buf.size() - 18,
                                  RSess::SEC_HMAC, session, crypto));

    // Present but with the wrong tag byte
    buf[buf.size() - 18] = 0x84;
    EXPECT_FALSE(RSess::unseal(buf.data(), buf.size(),
                                  RSess::SEC_HMAC, session, crypto));
}

TEST(RSessionNegative, MalformedHeadersAreRejected)
{
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(64);
    const std::vector< uint8_t > good = make_frame(RSess::SEC_NONE,
                                                   RSess::SI_R_GOOSE,
                                                   RSess::PAYLOAD_TYPE_GOOSE,
                                                   apdu, crypto);

    RSess::SessionHeader session;
    RSess::PayloadHeader payload;

    EXPECT_EQ(RSess::parse(good.data(), 10, session, payload),
              RSess::ERR_TOO_SHORT);

    auto broken = good;
    broken[1] = 0x41;
    EXPECT_EQ(RSess::parse(broken.data(), broken.size(), session, payload),
              RSess::ERR_BAD_CLTP);

    broken = good;
    broken[2] = 0xA5;
    EXPECT_EQ(RSess::parse(broken.data(), broken.size(), session, payload),
              RSess::ERR_BAD_SI);

    broken = good;
    broken[4] = 0x81;
    EXPECT_EQ(RSess::parse(broken.data(), broken.size(), session, payload),
              RSess::ERR_BAD_PI);

    broken = good;
    broken[15] = 0x01;   // protocol version 1
    EXPECT_EQ(RSess::parse(broken.data(), broken.size(), session, payload),
              RSess::ERR_BAD_VERSION);

    // A PayloadLength that runs past the end of the frame must not be trusted
    broken = good;
    broken[27] = 0xFF; broken[28] = 0xFF;
    EXPECT_EQ(RSess::parse(broken.data(), broken.size(), session, payload),
              RSess::ERR_BAD_PAYLOAD_LEN);

    broken = good;
    broken[31] = 0x83;   // unknown payload type
    EXPECT_EQ(RSess::parse(broken.data(), broken.size(), session, payload),
              RSess::ERR_BAD_PAYLOAD_TYPE);
}

TEST(RSessionNegative, WrongModeOnReceiveIsRejected)
{
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(64);
    std::vector< uint8_t > buf = make_frame(RSess::SEC_HMAC,
                                            RSess::SI_R_GOOSE,
                                            RSess::PAYLOAD_TYPE_GOOSE,
                                            apdu, crypto);

    RSess::SessionHeader session;
    ASSERT_GE(RSess::parse_session(buf.data(), buf.size(), session), 0);

    // An HMAC frame carries no IV, so a receiver configured for GCM rejects it
    EXPECT_FALSE(RSess::unseal(buf.data(), buf.size(),
                                  RSess::SEC_GCM, session, crypto));
}

// --- Security overhead accounting -------------------------------------------

TEST(RSecurityOverhead, PerModeByteCost)
{
    /*
     * Frame sizes differ between modes, so equal packet rates are NOT equal bit
     * rates. The deltas below are what converts one into the other.
     */
    constexpr size_t APDU = 100;

    const size_t none = RSess::udp_payload_size(RSess::SEC_NONE, APDU);
    const size_t hmac = RSess::udp_payload_size(RSess::SEC_HMAC, APDU);
    const size_t gcm  = RSess::udp_payload_size(RSess::SEC_GCM,  APDU);

    EXPECT_EQ(none, 37 + APDU);         // envelope only
    EXPECT_EQ(hmac, none + 18);         // 0x85 + length byte + 16-byte tag
    EXPECT_EQ(gcm,  none + 12 + 18);    // ... plus the 12-byte IV in the header
}

TEST(RSecurityOverhead, NoSecurityModeVerifiesNothing)
{
    // SEC_NONE must do no cryptographic work: corrupt anywhere and it still parses.
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(64);

    std::vector< uint8_t > buf = make_frame(RSess::SEC_NONE, RSess::SI_R_GOOSE,
                                            RSess::PAYLOAD_TYPE_GOOSE, apdu, crypto);
    // No trailer is emitted, so the frame ends exactly at the APDU.
    ASSERT_EQ(buf.size(), 37u + apdu.size());

    RSess::SessionHeader session;
    ASSERT_GE(RSess::parse_session(buf.data(), buf.size(), session), 0);
    EXPECT_EQ(session.ivLength, 0) << "SEC_NONE must not carry an IV";
    EXPECT_TRUE(RSess::unseal(buf.data(), buf.size(),
                              RSess::SEC_NONE, session, crypto));

    buf[40] ^= 0xFF;   // inside the APDU
    EXPECT_TRUE(RSess::unseal(buf.data(), buf.size(),
                              RSess::SEC_NONE, session, crypto))
        << "SEC_NONE authenticated something — the baseline is not crypto-free";
}

TEST(RSecurityOverhead, OnlyGcmHidesThePayload)
{
    /*
     * none and hmac leave the APDU in clear — the difference between them is the
     * signature and nothing else. Only gcm additionally encrypts.
     */
    RSess::Crypto crypto;
    const std::vector< uint8_t > apdu = make_apdu(64);

    for (auto mode : { RSess::SEC_NONE, RSess::SEC_HMAC }) {
        const std::vector< uint8_t > buf =
            make_frame(mode, RSess::SI_R_GOOSE, RSess::PAYLOAD_TYPE_GOOSE, apdu, crypto);
        EXPECT_EQ(0, std::memcmp(buf.data() + 37, apdu.data(), apdu.size()))
            << "APDU should be in clear for mode " << RSess::to_string(mode);
    }

    const std::vector< uint8_t > gcm =
        make_frame(RSess::SEC_GCM, RSess::SI_R_GOOSE,
                   RSess::PAYLOAD_TYPE_GOOSE, apdu, crypto);
    EXPECT_NE(0, std::memcmp(gcm.data() + 49, apdu.data(), apdu.size()));
}
