// IPv4/UDP envelope validation and destination-IP passport matching.

#include "common/r_frame_builder.hpp"
#include "common/r_session.hpp"
#include "common/goose_container.hpp"
#include "common/sv_container.hpp"
#include "bus_processor/process_bus_parser.hpp"

#include <rte_mbuf.h>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
    constexpr uint32_t DST_IP  = R_DEFAULT_DST_IP;      // 239.192.1.1
    // Different group, same RFC 1112 multicast MAC: the low 23 bits coincide.
    constexpr uint32_t ALIAS_IP = 0xEF400101;           // 239.64.1.1
    constexpr size_t   UDP_PAYLOAD = 100;

    using Off = RFrame::Offset;

    // A structurally valid Ethernet/IPv4/UDP frame with a zero-filled payload.
    // The envelope validator ignores payload content, so no session is built.
    std::vector< uint8_t > make_frame(size_t udpPayload = UDP_PAYLOAD,
                                      uint32_t dstIP = DST_IP)
    {
        RFrameConfig cfg;
        cfg.dstIP = dstIP;

        std::vector< uint8_t > frame(Off::OFF_UDP_PAYLOAD + udpPayload, 0);
        RFrame::build_prefix(frame.data(), cfg, udpPayload);
        return frame;
    }

    int validate(const std::vector< uint8_t >& frame, uint64_t olFlags,
                 uint32_t& dstIP)
    {
        return ProcessBusParser::validate_r_envelope(
                   frame.data(), static_cast< unsigned >(frame.size()),
                   olFlags, dstIP);
    }
}

// --- Checksum offload states ------------------------------------------------

TEST(REnvelopeCksum, GoodFlagIsAccepted)
{
    uint32_t dstIP = 0;
    EXPECT_EQ(validate(make_frame(), RTE_MBUF_F_RX_IP_CKSUM_GOOD, dstIP), R_ENV_OK);
    EXPECT_EQ(dstIP, DST_IP);
}

TEST(REnvelopeCksum, BadFlagIsRejected)
{
    uint32_t dstIP = 0;
    // A byte-perfect frame is still rejected: the NIC's verdict wins.
    EXPECT_EQ(validate(make_frame(), RTE_MBUF_F_RX_IP_CKSUM_BAD, dstIP),
              R_ENV_ERR_IP_CKSUM);
}

TEST(REnvelopeCksum, NoneFlagIsAccepted)
{
    uint32_t dstIP = 0;
    EXPECT_EQ(validate(make_frame(), RTE_MBUF_F_RX_IP_CKSUM_NONE, dstIP), R_ENV_OK);
}

TEST(REnvelopeCksum, UnknownFlagVerifiesInSoftware)
{
    uint32_t dstIP = 0;
    EXPECT_EQ(validate(make_frame(), RTE_MBUF_F_RX_IP_CKSUM_UNKNOWN, dstIP), R_ENV_OK);
}

TEST(REnvelopeCksum, UnknownFlagRejectsCorruptedChecksum)
{
    std::vector< uint8_t > frame = make_frame();
    frame[Off::OFF_IP_CSUM] ^= 0xFF;                    // break the header checksum

    uint32_t dstIP = 0;
    EXPECT_EQ(validate(frame, RTE_MBUF_F_RX_IP_CKSUM_UNKNOWN, dstIP),
              R_ENV_ERR_IP_CKSUM);
    // The same corrupt frame is accepted when the NIC reports it as good.
    EXPECT_EQ(validate(frame, RTE_MBUF_F_RX_IP_CKSUM_GOOD, dstIP), R_ENV_OK);
}

// --- Structural validation --------------------------------------------------

class REnvelope : public ::testing::Test
{
protected:
    // Structural cases run with GOOD so the checksum path never masks them.
    static constexpr uint64_t GOOD = RTE_MBUF_F_RX_IP_CKSUM_GOOD;
    uint32_t dstIP = 0;
};

TEST_F(REnvelope, ValidFrameExtractsDestinationIp)
{
    EXPECT_EQ(validate(make_frame(), GOOD, dstIP), R_ENV_OK);
    EXPECT_EQ(dstIP, DST_IP);
}

TEST_F(REnvelope, NonIpv4IsRejected)
{
    std::vector< uint8_t > frame = make_frame();
    frame[12] = 0x88; frame[13] = 0xB8;                 // L2 GOOSE EtherType
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_NOT_IPV4);
}

TEST_F(REnvelope, Ihl6IsRejected)
{
    std::vector< uint8_t > frame = make_frame();
    frame[Off::OFF_IP] = 0x46;                          // IHL 6: options present
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_IHL);
}

TEST_F(REnvelope, TotalLengthTooSmall)
{
    std::vector< uint8_t > frame = make_frame();
    frame[Off::OFF_IP + 2] = 0x00; frame[Off::OFF_IP + 3] = 27;
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_IP_LEN);
}

TEST_F(REnvelope, TotalLengthExceedsReceivedBytes)
{
    std::vector< uint8_t > frame = make_frame();
    const uint16_t tooBig = static_cast< uint16_t >(frame.size());  // 14+len > size
    frame[Off::OFF_IP + 2] = static_cast< uint8_t >(tooBig >> 8);
    frame[Off::OFF_IP + 3] = static_cast< uint8_t >(tooBig);
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_IP_LEN);
}

TEST_F(REnvelope, MoreFragmentsIsRejected)
{
    std::vector< uint8_t > frame = make_frame();
    frame[Off::OFF_IP + 6] = 0x20; frame[Off::OFF_IP + 7] = 0x00;   // MF set
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_FRAGMENT);
}

TEST_F(REnvelope, NonZeroFragmentOffsetIsRejected)
{
    std::vector< uint8_t > frame = make_frame();
    frame[Off::OFF_IP + 6] = 0x00; frame[Off::OFF_IP + 7] = 0x08;   // offset 8
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_FRAGMENT);
}

TEST_F(REnvelope, DontFragmentSetAndClearBothAccepted)
{
    std::vector< uint8_t > frame = make_frame();
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_OK);              // DF set by builder

    frame[Off::OFF_IP + 6] = 0x00; frame[Off::OFF_IP + 7] = 0x00;   // DF clear
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_OK);
}

TEST_F(REnvelope, WrongProtocolIsRejected)
{
    std::vector< uint8_t > frame = make_frame();
    frame[Off::OFF_IP_PROTO] = 6;                       // TCP
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_PROTO);
}

TEST_F(REnvelope, WrongDestinationPortIsRejected)
{
    std::vector< uint8_t > frame = make_frame();
    frame[Off::OFF_UDP_DST_PORT + 1] = 103;             // not 102
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_UDP_PORT);
}

TEST_F(REnvelope, UdpLengthBelowEightIsRejected)
{
    std::vector< uint8_t > frame = make_frame();
    frame[Off::OFF_UDP_LENGTH] = 0x00; frame[Off::OFF_UDP_LENGTH + 1] = 7;
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_UDP_LEN);
}

TEST_F(REnvelope, UdpLengthInconsistentWithTotalLengthIsRejected)
{
    std::vector< uint8_t > frame = make_frame();
    // Still >= 8, but no longer equal to (IP total length - 20).
    frame[Off::OFF_UDP_LENGTH] = 0x00; frame[Off::OFF_UDP_LENGTH + 1] = 9;
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_ERR_UDP_LEN);
}

TEST_F(REnvelope, TrailingEthernetPaddingIsAccepted)
{
    std::vector< uint8_t > frame = make_frame();
    frame.resize(frame.size() + 24, 0);                 // padding past the datagram
    EXPECT_EQ(validate(frame, GOOD, dstIP), R_ENV_OK);
    EXPECT_EQ(dstIP, DST_IP);
}

// --- Destination IP in the passport -----------------------------------------

TEST(REnvelopeDstIp, WrongRGooseDestinationMisses)
{
    GooseSource::ptr src = std::make_shared< GooseSource >();
    src->SetMAC(RFrame::multicast_mac(DST_IP)).SetAppID(0x0001)
        .SetGOID("GOID00000001").SetDataSetRef("DS").SetGOCBRef("GOCB")
        .SetCRev(1).SetNumEntries(4).SetDstIP(DST_IP);

    GooseContainer map;
    map[src->GetPassport()] = src;

    GoosePassport wrong = src->GetPassport();
    wrong.dstIP = ALIAS_IP;
    EXPECT_FALSE(wrong == src->GetPassport());
    EXPECT_TRUE(map.find(wrong) == map.end());
    EXPECT_TRUE(map.find(src->GetPassport()) != map.end());
}

TEST(REnvelopeDstIp, WrongRSvDestinationMisses)
{
    SVStreamSource::ptr src = std::make_shared< SVStreamSource >();
    src->SetMAC(RFrame::multicast_mac(DST_IP)).SetAppID(0x0001)
        .SetSVID("SVID0001").SetCRev(1).SetNumASDU(1).SetDstIP(DST_IP);

    SVContainer map;
    map[src->GetPassport()] = src;

    SVStreamPassport wrong = src->GetPassport();
    wrong.dstIP = ALIAS_IP;
    EXPECT_FALSE(wrong == src->GetPassport());
    EXPECT_TRUE(map.find(wrong) == map.end());
}

TEST(REnvelopeDstIp, AliasedMulticastMacIsDisambiguatedByIp)
{
    // The two groups share a destination MAC, so only the IP tells them apart.
    ASSERT_TRUE(RFrame::multicast_mac(DST_IP) == RFrame::multicast_mac(ALIAS_IP));

    GoosePassport a;
    a.dmac = RFrame::multicast_mac(DST_IP);
    a.appid = 0x0001;
    a.dstIP = DST_IP;

    GoosePassport b = a;
    b.dstIP = ALIAS_IP;
    EXPECT_FALSE(a == b);
}

TEST(REnvelopeDstIp, Layer2PassportsIgnoreDstIp)
{
    // L2 sources never set dstIP: both sides stay zero and still match.
    GoosePassport a, b;
    a.dmac = b.dmac = MAC("01:0C:CD:04:00:01");
    a.appid = b.appid = 0x0001;
    EXPECT_EQ(a.dstIP, 0u);
    EXPECT_TRUE(a == b);
}
