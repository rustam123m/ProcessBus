// Classification and software-RSS key extraction for routable frames.

#include "common/r_frame_builder.hpp"
#include "common/r_session.hpp"
#include "common/r_session_crypto.hpp"
#include "bus_processor/process_bus_parser.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
    constexpr uint16_t APDU_SIZE = 100;

    // A syntactically complete routable frame with a filler APDU.
    std::vector< uint8_t > make_frame(RSess::SecurityMode mode, uint8_t si,
                                      uint8_t payloadType, uint16_t appid,
                                      RSess::Crypto& crypto)
    {
        RFrameConfig cfg;
        cfg.mode = mode;

        const size_t udpSize = RSess::udp_payload_size(mode, APDU_SIZE);
        std::vector< uint8_t > frame(RFrame::OFF_UDP_PAYLOAD + udpSize, 0);

        RFrame::build_prefix(frame.data(), cfg, udpSize);
        // The generator publishes the APPID in the source port; mirror that here.
        frame[RFrame::OFF_UDP_SRC_PORT]     = static_cast< uint8_t >(appid >> 8);
        frame[RFrame::OFF_UDP_SRC_PORT + 1] = static_cast< uint8_t >(appid);

        RSess::BuildParams params;
        params.si          = si;
        params.payloadType = payloadType;
        params.mode        = mode;
        params.spduNumber  = 7;
        params.appid       = appid;
        params.apduLength  = APDU_SIZE;

        uint8_t* udp = frame.data() + RFrame::OFF_UDP_PAYLOAD;
        const size_t apduOff = RSess::build(udp, params);
        udp[apduOff] = (payloadType == RSess::PAYLOAD_TYPE_GOOSE) ? 0x61 : 0x60;

        EXPECT_TRUE(RSess::seal(udp, mode, crypto));
        return frame;
    }

    BUS_PROTO classify(const std::vector< uint8_t >& frame, unsigned& appid)
    {
        return ProcessBusParser::get_proto_type(frame.data(), &appid,
                                                static_cast< unsigned >(frame.size()));
    }
}

class RRouting : public ::testing::TestWithParam< RSess::SecurityMode >
{
};

TEST_P(RRouting, RoutableGooseIsClassified)
{
    RSess::Crypto crypto;
    unsigned appid = 0;

    const std::vector< uint8_t > frame =
        make_frame(GetParam(), RSess::SI_R_GOOSE, RSess::PAYLOAD_TYPE_GOOSE, 0x0042, crypto);

    EXPECT_EQ(classify(frame, appid), BUS_PROTO_R_GOOSE);
    EXPECT_EQ(appid, 0x0042u);
}

TEST_P(RRouting, RoutableSVIsClassified)
{
    RSess::Crypto crypto;
    unsigned appid = 0;

    const std::vector< uint8_t > frame =
        make_frame(GetParam(), RSess::SI_R_SV, RSess::PAYLOAD_TYPE_SV, 0x0007, crypto);

    EXPECT_EQ(classify(frame, appid), BUS_PROTO_R_SV);
    EXPECT_EQ(appid, 0x0007u);
}

TEST_P(RRouting, RssKeyIsModeIndependent)
{
    // Under AES-GCM the payload-header APPID is ciphertext: one stream would smear.
    RSess::Crypto crypto;
    constexpr unsigned WORKER_NUM = 4;      // power of two, as multi_core_rss requires
    constexpr uint16_t APPID = 0x0103;

    const std::vector< uint8_t > frame =
        make_frame(GetParam(), RSess::SI_R_GOOSE, RSess::PAYLOAD_TYPE_GOOSE, APPID, crypto);

    unsigned appid = 0;
    ASSERT_EQ(classify(frame, appid), BUS_PROTO_R_GOOSE);
    EXPECT_EQ(appid & (WORKER_NUM - 1), APPID & (WORKER_NUM - 1));
}

INSTANTIATE_TEST_SUITE_P(AllModes, RRouting,
                         ::testing::Values(RSess::SEC_NONE,
                                           RSess::SEC_HMAC,
                                           RSess::SEC_GCM));

TEST(RRoutingNegative, PlainIPv4IsNotRoutableBus)
{
    RSess::Crypto crypto;
    unsigned appid = 0;

    std::vector< uint8_t > frame =
        make_frame(RSess::SEC_NONE, RSess::SI_R_GOOSE, RSess::PAYLOAD_TYPE_GOOSE, 1, crypto);

    // Wrong destination port
    std::vector< uint8_t > other = frame;
    other[RFrame::OFF_UDP_DST_PORT + 1] = 0x67;      // 103
    EXPECT_EQ(classify(other, appid), NON_BUS_PROTO);

    // Not UDP
    other = frame;
    other[RFrame::OFF_IP_PROTO] = 6;                 // TCP
    EXPECT_EQ(classify(other, appid), NON_BUS_PROTO);

    // IPv4 options present: our offsets assume IHL = 5
    other = frame;
    other[RFrame::OFF_IP] = 0x46;
    EXPECT_EQ(classify(other, appid), NON_BUS_PROTO);

    // Unknown session identifier
    other = frame;
    other[size_t(RFrame::OFF_UDP_PAYLOAD) + RSess::OFF_SI] = 0xA3;   // management SPDU
    EXPECT_EQ(classify(other, appid), NON_BUS_PROTO);
}

TEST(RRoutingNegative, RuntIsNotClassifiedAsRoutable)
{
    /*
     * A 60-byte IPv4/UDP runt is shorter than the fixed session header. It must
     * be rejected on length before any of those bytes is read.
     */
    RSess::Crypto crypto;
    unsigned appid = 0;

    const std::vector< uint8_t > frame =
        make_frame(RSess::SEC_NONE, RSess::SI_R_GOOSE, RSess::PAYLOAD_TYPE_GOOSE, 1, crypto);

    for (unsigned size=42;size<73;++size) {
        EXPECT_EQ(ProcessBusParser::get_proto_type(frame.data(), &appid, size),
                  NON_BUS_PROTO) << "accepted a routable frame of only " << size << " bytes";
    }
    EXPECT_EQ(ProcessBusParser::get_proto_type(frame.data(), &appid, 73),
              BUS_PROTO_R_GOOSE);
}

TEST(RRoutingNegative, TruncatedFrameIsRejectedByTheParser)
{
    // Reads are bound by the IPv4 total length, not the frame length.
    RSess::Crypto crypto;

    std::vector< uint8_t > frame =
        make_frame(RSess::SEC_NONE, RSess::SI_R_GOOSE, RSess::PAYLOAD_TYPE_GOOSE, 1, crypto);

    RSess::SessionHeader session;
    GoosePassport pass;
    GooseState state;

    EXPECT_LT(ProcessBusParser::parse_r_goose_packet(
                  frame.data(), static_cast< int >(frame.size()) - 20,
                  RSess::SEC_NONE, crypto, session, pass, state), 0);
}

TEST(RRoutingNegative, L2FramesAreUnaffected)
{
    /*
     * The routable branch must not disturb the classification the L2 pipeline
     * has always done.
     */
    unsigned appid = 0;

    std::vector< uint8_t > goose(128, 0);
    goose[12] = 0x88; goose[13] = 0xB8;
    goose[14] = 0x12; goose[15] = 0x34;
    EXPECT_EQ(ProcessBusParser::get_proto_type(goose.data(), &appid,
                                               static_cast< unsigned >(goose.size())),
              BUS_PROTO_GOOSE);
    EXPECT_EQ(appid, 0x1234u);

    std::vector< uint8_t > sv(128, 0);
    sv[12] = 0x81; sv[13] = 0x00;           // VLAN
    sv[16] = 0x88; sv[17] = 0xBA;
    sv[18] = 0x56; sv[19] = 0x78;
    EXPECT_EQ(ProcessBusParser::get_proto_type(sv.data(), &appid,
                                               static_cast< unsigned >(sv.size())),
              BUS_PROTO_SV);
    EXPECT_EQ(appid, 0x5678u);
}
