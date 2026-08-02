// R-GOOSE cross-verification against libiec61850, both directions x 3 modes.

#include "r_interop_fixture.hpp"

#include "bus_generator/goose_traffic_gen.hpp"
#include "bus_generator/r_goose_traffic_gen.hpp"
#include "bus_processor/process_bus_parser.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
    constexpr unsigned GOOSE_NUM = 4;
    constexpr unsigned GOOSE_FREQ = 10;
    constexpr unsigned GOOSE_ENTRIES = 16;

    uint16_t be16(const uint8_t* p) { return static_cast< uint16_t >((p[0] << 8) | p[1]); }

    // The L2 APDU for the same inputs, after one amend of the given IED index.
    std::vector< uint8_t > reference_l2_apdu(unsigned iedIdx)
    {
        GooseTrafficGen gen(GOOSE_NUM, GOOSE_FREQ, GOOSE_ENTRIES);

        std::vector< uint8_t > frame(gen.GetSkeletonBuffer(),
                                     gen.GetSkeletonBuffer() + gen.GetSkeletonSize());
        GoosePacketDesc desc;
        desc.idx = iedIdx;
        gen.AmendPacket(frame.data(), desc);

        const size_t appidOff = gen.GetOffsets()[GOOSE_APPID_OFFSET];
        const size_t apduOff  = appidOff + 8;
        const size_t apduLen  = be16(frame.data() + appidOff + 2) - 8u;

        return std::vector< uint8_t >(frame.begin() + apduOff,
                                      frame.begin() + apduOff + apduLen);
    }

    // One amended R-GOOSE frame, as the generator would hand it to the NIC.
    template< RSess::SecurityMode MODE >
    std::vector< uint8_t > make_r_frame(RGooseTrafficGen& gen, unsigned iedIdx,
                                        unsigned repeats = 1)
    {
        std::vector< uint8_t > frame(gen.GetSkeletonBuffer(),
                                     gen.GetSkeletonBuffer() + gen.GetSkeletonSize());
        GoosePacketDesc desc;
        desc.idx = iedIdx;
        for (unsigned i=0;i<repeats;++i) {
            gen.AmendPacket< MODE >(frame.data(), desc);
        }
        return frame;
    }

    std::vector< uint8_t > udp_payload_of(const std::vector< uint8_t >& frame)
    {
        return std::vector< uint8_t >(frame.begin() + RFrame::OFF_UDP_PAYLOAD, frame.end());
    }
}

// --- Direction A: our TX -> libiec61850 RX ----------------------------------

template< RSess::SecurityMode MODE >
static void expect_library_accepts_our_frame()
{
    RFrameConfig cfg;
    cfg.mode = MODE;

    RGooseTrafficGen gen(GOOSE_NUM, GOOSE_FREQ, GOOSE_ENTRIES, cfg);
    LibRSession lib(MODE);

    const std::vector< uint8_t > frame = make_r_frame< MODE >(gen, 0);
    const std::vector< uint8_t > udp = udp_payload_of(frame);

    const LibRSession::Decoded got = lib.Receive(udp.data(), udp.size());

    ASSERT_TRUE(got.received) << "libiec61850 rejected our R-GOOSE frame in mode "
                              << RSess::to_string(MODE);
    EXPECT_EQ(got.appId, 1) << "APPID mismatch (IED index 0 publishes APPID 1)";

    const std::vector< uint8_t > expected = reference_l2_apdu(0);
    ASSERT_EQ(got.apdu.size(), expected.size());
    EXPECT_EQ(0, std::memcmp(got.apdu.data(), expected.data(), expected.size()))
        << "the APDU libiec61850 recovered differs from the L2 APDU for the same inputs";
}

TEST(RGooseInteropTx, LibraryAcceptsOurFrameNone)
{
    expect_library_accepts_our_frame< RSess::SEC_NONE >();
}

TEST(RGooseInteropTx, LibraryAcceptsOurFrameHmac)
{
    expect_library_accepts_our_frame< RSess::SEC_HMAC >();
}

TEST(RGooseInteropTx, LibraryAcceptsOurFrameGcm)
{
    expect_library_accepts_our_frame< RSess::SEC_GCM >();
}

TEST(RGooseInteropTx, RecycledMbufStaysDecodable)
{
    // Amending the same buffer repeatedly reproduces the mbuf reuse path.
    RFrameConfig cfg;
    cfg.mode = RSess::SEC_GCM;

    RGooseTrafficGen gen(GOOSE_NUM, GOOSE_FREQ, GOOSE_ENTRIES, cfg);
    LibRSession lib(RSess::SEC_GCM);

    const std::vector< uint8_t > frame = make_r_frame< RSess::SEC_GCM >(gen, 0, 3);
    const std::vector< uint8_t > udp = udp_payload_of(frame);

    const LibRSession::Decoded got = lib.Receive(udp.data(), udp.size());
    ASSERT_TRUE(got.received) << "third use of a recycled buffer failed to decode";
    EXPECT_EQ(got.appId, 1);
}

TEST(RGooseInteropTx, TamperedFrameIsRejectedByTheLibrary)
{
    RFrameConfig cfg;
    cfg.mode = RSess::SEC_HMAC;

    RGooseTrafficGen gen(GOOSE_NUM, GOOSE_FREQ, GOOSE_ENTRIES, cfg);
    LibRSession lib(RSess::SEC_HMAC);

    std::vector< uint8_t > frame = make_r_frame< RSess::SEC_HMAC >(gen, 0);
    // Flip a bit inside the APDU, leaving the signature untouched.
    frame[RFrame::OFF_UDP_PAYLOAD + RSess::apdu_offset(0) + 4] ^= 0x01;

    const std::vector< uint8_t > udp = udp_payload_of(frame);
    EXPECT_FALSE(lib.Receive(udp.data(), udp.size()).received);
}

// --- Direction B: libiec61850 TX -> our parser ------------------------------

template< RSess::SecurityMode MODE >
static void expect_we_accept_library_frame()
{
    RFrameConfig cfg;
    cfg.mode = MODE;

    LibRSession lib(MODE);

    const std::vector< uint8_t > apdu = reference_l2_apdu(0);
    const std::vector< uint8_t > udp =
        lib.Send(RSESSION_SPDU_ID_GOOSE, 0x0001, apdu.data(), apdu.size());
    ASSERT_FALSE(udp.empty()) << "libiec61850 produced no frame in mode "
                              << RSess::to_string(MODE);

    std::vector< uint8_t > frame = LibRSession::WrapInFrame(udp, cfg);

    unsigned rssKey = 0;
    EXPECT_EQ(ProcessBusParser::get_proto_type(frame.data(), &rssKey,
                                               static_cast< unsigned >(frame.size())),
              BUS_PROTO_R_GOOSE);

    RSess::Crypto crypto;
    RSess::SessionHeader session;
    GoosePassport pass;
    GooseState state;

    const int rc = ProcessBusParser::parse_r_goose_packet(
                       frame.data(), static_cast< int >(frame.size()),
                       MODE, crypto, session, pass, state);
    ASSERT_EQ(rc, 0) << "our parser rejected a libiec61850 frame in mode "
                     << RSess::to_string(MODE);

    EXPECT_EQ(pass.appid, 0x0001);
    EXPECT_EQ(pass.goid, "GOID00000001");
    EXPECT_EQ(pass.gocbref, "IED00000001LDName/LLN0$GO$GOCB");
    EXPECT_EQ(pass.dataset, "IED00000001LDName/LLN0$DataSet");
    EXPECT_EQ(pass.crev, 1u);
    EXPECT_EQ(state.stNum, 1u);

    // The library starts its session sequence at zero and post-increments.
    EXPECT_EQ(session.spduNumber, 0u);
    EXPECT_EQ(session.keyId, RSess::KEY_ID);
    EXPECT_EQ(session.version, 2u);
}

TEST(RGooseInteropRx, WeAcceptLibraryFrameNone)
{
    expect_we_accept_library_frame< RSess::SEC_NONE >();
}

TEST(RGooseInteropRx, WeAcceptLibraryFrameHmac)
{
    expect_we_accept_library_frame< RSess::SEC_HMAC >();
}

TEST(RGooseInteropRx, WeAcceptLibraryFrameGcm)
{
    expect_we_accept_library_frame< RSess::SEC_GCM >();
}

TEST(RGooseInteropRx, LibraryRandomIvIsHonoured)
{
    // The IV travels in the header: two frames must differ and both decrypt.
    RFrameConfig cfg;
    cfg.mode = RSess::SEC_GCM;

    LibRSession lib(RSess::SEC_GCM);
    const std::vector< uint8_t > apdu = reference_l2_apdu(0);

    const std::vector< uint8_t > first =
        lib.Send(RSESSION_SPDU_ID_GOOSE, 0x0001, apdu.data(), apdu.size());
    const std::vector< uint8_t > second =
        lib.Send(RSESSION_SPDU_ID_GOOSE, 0x0001, apdu.data(), apdu.size());
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());

    EXPECT_NE(0, std::memcmp(first.data() + RSess::OFF_IV,
                             second.data() + RSess::OFF_IV, RSess::IV_LEN_GCM))
        << "expected libiec61850 to use a random IV per frame";

    RSess::Crypto crypto;
    for (const auto& udp : { first, second }) {
        std::vector< uint8_t > frame = LibRSession::WrapInFrame(udp, cfg);

        RSess::SessionHeader session;
        GoosePassport pass;
        GooseState state;
        EXPECT_EQ(0, ProcessBusParser::parse_r_goose_packet(
                         frame.data(), static_cast< int >(frame.size()),
                         RSess::SEC_GCM, crypto, session, pass, state));
    }
}

TEST(RGooseInteropRx, ForgedTagIsRejected)
{
    // A corrupted tag must be rejected, not passed upward.
    RFrameConfig cfg;
    cfg.mode = RSess::SEC_HMAC;

    LibRSession lib(RSess::SEC_HMAC);
    const std::vector< uint8_t > apdu = reference_l2_apdu(0);
    std::vector< uint8_t > udp =
        lib.Send(RSESSION_SPDU_ID_GOOSE, 0x0001, apdu.data(), apdu.size());
    ASSERT_FALSE(udp.empty());

    udp[udp.size() - 1] ^= 0xFF;   // last byte of the tag

    std::vector< uint8_t > frame = LibRSession::WrapInFrame(udp, cfg);

    RSess::Crypto crypto;
    RSess::SessionHeader session;
    GoosePassport pass;
    GooseState state;
    EXPECT_EQ(R_PARSE_ERR_AUTH,
              ProcessBusParser::parse_r_goose_packet(
                  frame.data(), static_cast< int >(frame.size()),
                  RSess::SEC_HMAC, crypto, session, pass, state));
}

TEST(RGooseFrameSize, FitsStandardMtu)
{
    // Frames must fit a 1500 B MTU: nothing enables jumbo frames.
    constexpr size_t MAX_ETHERNET_FRAME = 1514;   // 1500 payload + 14 header

    for (auto mode : { RSess::SEC_NONE, RSess::SEC_HMAC, RSess::SEC_GCM }) {
        RFrameConfig cfg;
        cfg.mode = mode;

        RGooseTrafficGen gen(GOOSE_NUM, GOOSE_FREQ, GOOSE_ENTRIES, cfg);
        std::cout << "  R-GOOSE  " << RSess::to_string(mode)
                  << "  frame = " << gen.GetSkeletonSize() << " bytes\n";

        EXPECT_LE(gen.GetSkeletonSize(), MAX_ETHERNET_FRAME);
        EXPECT_GE(gen.GetSkeletonSize(), 64u) << "below the Ethernet minimum";
    }

    GooseTrafficGen l2(GOOSE_NUM, GOOSE_FREQ, GOOSE_ENTRIES);
    std::cout << "  L2 GOOSE (baseline)  frame = " << l2.GetSkeletonSize() << " bytes\n";
}
