// R-SV cross-verification against libiec61850, both directions x 3 modes.

#include "r_interop_fixture.hpp"

#include "bus_generator/sv_traffic_gen.hpp"
#include "bus_generator/r_sv_traffic_gen.hpp"
#include "bus_processor/process_bus_parser.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
    constexpr unsigned SV_NUM = 4;

    uint16_t be16(const uint8_t* p) { return static_cast< uint16_t >((p[0] << 8) | p[1]); }

    // The L2 SV APDU for the same inputs, after one amend of the given stream.
    std::vector< uint8_t > reference_l2_apdu(SV_TYPE type, unsigned iedIdx)
    {
        SVTrafficGen gen(SV_NUM, type);

        std::vector< uint8_t > frame(gen.GetSkeletonBuffer(),
                                     gen.GetSkeletonBuffer() + gen.GetSkeletonSize());
        SVPacketDesc desc;
        desc.idx = iedIdx;
        if (type == SV80) {
            gen.AmendPacketSV80(frame.data(), desc);
        } else {
            gen.AmendPacketSV256(frame.data(), desc);
        }

        const size_t appidOff = gen.GetAppidOffset();
        const size_t apduOff  = appidOff + 8;
        const size_t apduLen  = be16(frame.data() + appidOff + 2) - 8u;

        return std::vector< uint8_t >(frame.begin() + apduOff,
                                      frame.begin() + apduOff + apduLen);
    }

    template< RSess::SecurityMode MODE >
    std::vector< uint8_t > make_r_frame(RSVTrafficGen& gen, SV_TYPE type, unsigned iedIdx)
    {
        std::vector< uint8_t > frame(gen.GetSkeletonBuffer(),
                                     gen.GetSkeletonBuffer() + gen.GetSkeletonSize());
        SVPacketDesc desc;
        desc.idx = iedIdx;
        if (type == SV80) {
            gen.AmendPacketSV80< MODE >(frame.data(), desc);
        } else {
            gen.AmendPacketSV256< MODE >(frame.data(), desc);
        }
        return frame;
    }

    std::vector< uint8_t > udp_payload_of(const std::vector< uint8_t >& frame)
    {
        return std::vector< uint8_t >(frame.begin() + RFrame::OFF_UDP_PAYLOAD, frame.end());
    }

    /*
     * The APDU our generator sealed into frame, recovered as the receive path does.
     *
     * libiec61850 never initialises the SV sample area, so the reference APDU is
     * compared on length only.
     */
    std::vector< uint8_t > sealed_apdu(const std::vector< uint8_t >& frame,
                                       RSess::SecurityMode mode)
    {
        std::vector< uint8_t > udp = udp_payload_of(frame);

        RSess::Crypto crypto;
        RSess::SessionHeader session;
        if (RSess::parse_session(udp.data(), udp.size(), session) < 0) {
            return {};
        }
        if (!RSess::unseal(udp.data(), udp.size(), mode, session, crypto)) {
            return {};
        }

        RSess::PayloadHeader phdr;
        const int apduOff = RSess::parse_payload(udp.data(), udp.size(), session, phdr);
        if (apduOff < 0) {
            return {};
        }
        return std::vector< uint8_t >(udp.begin() + apduOff,
                                      udp.begin() + apduOff + phdr.apduSize);
    }
}

// --- Direction A: our TX -> libiec61850 RX ----------------------------------

template< RSess::SecurityMode MODE >
static void expect_library_accepts_our_frame(SV_TYPE type)
{
    RFrameConfig cfg;
    cfg.mode = MODE;

    RSVTrafficGen gen(SV_NUM, type, cfg);
    LibRSession lib(MODE);

    const std::vector< uint8_t > frame = make_r_frame< MODE >(gen, type, 0);
    const std::vector< uint8_t > udp = udp_payload_of(frame);

    const LibRSession::Decoded got = lib.Receive(udp.data(), udp.size());

    ASSERT_TRUE(got.received) << "libiec61850 rejected our R-SV frame in mode "
                              << RSess::to_string(MODE);
    EXPECT_EQ(got.appId, 1);

    // Structural identity with the L2 APDU: same inputs, same encoding, same size.
    EXPECT_EQ(got.apdu.size(), reference_l2_apdu(type, 0).size());

    const std::vector< uint8_t > sealed = sealed_apdu(frame, MODE);
    ASSERT_FALSE(sealed.empty());
    ASSERT_EQ(got.apdu.size(), sealed.size());
    EXPECT_EQ(0, std::memcmp(got.apdu.data(), sealed.data(), sealed.size()))
        << "libiec61850 recovered different bytes than we sealed into the frame";
}

TEST(RSVInteropTx, LibraryAcceptsOurSV80None)
{
    expect_library_accepts_our_frame< RSess::SEC_NONE >(SV80);
}

TEST(RSVInteropTx, LibraryAcceptsOurSV80Hmac)
{
    expect_library_accepts_our_frame< RSess::SEC_HMAC >(SV80);
}

TEST(RSVInteropTx, LibraryAcceptsOurSV80Gcm)
{
    expect_library_accepts_our_frame< RSess::SEC_GCM >(SV80);
}

TEST(RSVInteropTx, LibraryAcceptsOurSV256None)
{
    expect_library_accepts_our_frame< RSess::SEC_NONE >(SV256);
}

TEST(RSVInteropTx, LibraryAcceptsOurSV256Gcm)
{
    // The largest frame we emit: 8 ASDUs plus the IV and the trailer.
    expect_library_accepts_our_frame< RSess::SEC_GCM >(SV256);
}

// --- Direction B: libiec61850 TX -> our parser ------------------------------

template< RSess::SecurityMode MODE >
static void expect_we_accept_library_frame(SV_TYPE type, unsigned expectedASDU)
{
    RFrameConfig cfg;
    cfg.mode = MODE;

    LibRSession lib(MODE);

    const std::vector< uint8_t > apdu = reference_l2_apdu(type, 0);
    const std::vector< uint8_t > udp =
        lib.Send(RSESSION_SPDU_ID_SV, 0x0001, apdu.data(), apdu.size());
    ASSERT_FALSE(udp.empty()) << "libiec61850 produced no frame in mode "
                              << RSess::to_string(MODE);

    std::vector< uint8_t > frame = LibRSession::WrapInFrame(udp, cfg);

    unsigned rssKey = 0;
    EXPECT_EQ(ProcessBusParser::get_proto_type(frame.data(), &rssKey,
                                               static_cast< unsigned >(frame.size())),
              BUS_PROTO_R_SV);

    RSess::Crypto crypto;
    RSess::SessionHeader session;
    SVStreamPassport pass;
    SVStreamState state;

    const int rc = ProcessBusParser::parse_r_sv_packet(
                       frame.data(), static_cast< int >(frame.size()),
                       MODE, crypto, session, pass, state);
    ASSERT_EQ(rc, 0) << "our parser rejected a libiec61850 R-SV frame in mode "
                     << RSess::to_string(MODE);

    EXPECT_EQ(pass.appid, 0x0001);
    EXPECT_EQ(pass.svid, "SVID0001");
    EXPECT_EQ(pass.num, expectedASDU);
    EXPECT_EQ(pass.crev, 1u);
    EXPECT_EQ(session.version, 2u);
}

TEST(RSVInteropRx, WeAcceptLibrarySV80None)
{
    expect_we_accept_library_frame< RSess::SEC_NONE >(SV80, 1);
}

TEST(RSVInteropRx, WeAcceptLibrarySV80Hmac)
{
    expect_we_accept_library_frame< RSess::SEC_HMAC >(SV80, 1);
}

TEST(RSVInteropRx, WeAcceptLibrarySV80Gcm)
{
    expect_we_accept_library_frame< RSess::SEC_GCM >(SV80, 1);
}

TEST(RSVInteropRx, WeAcceptLibrarySV256None)
{
    expect_we_accept_library_frame< RSess::SEC_NONE >(SV256, 8);
}

TEST(RSVInteropRx, WeAcceptLibrarySV256Gcm)
{
    expect_we_accept_library_frame< RSess::SEC_GCM >(SV256, 8);
}

TEST(RSVInteropRx, GooseFrameIsNotAcceptedAsSV)
{
    /*
     * SI and payload type must both be honoured: a routable GOOSE frame handed
     * to the SV parser has to be refused, not silently walked as an SV APDU.
     */
    RFrameConfig cfg;
    LibRSession lib(RSess::SEC_NONE);

    const std::vector< uint8_t > apdu = reference_l2_apdu(SV80, 0);
    const std::vector< uint8_t > udp =
        lib.Send(RSESSION_SPDU_ID_GOOSE, 0x0001, apdu.data(), apdu.size());
    ASSERT_FALSE(udp.empty());

    std::vector< uint8_t > frame = LibRSession::WrapInFrame(udp, cfg);

    RSess::Crypto crypto;
    RSess::SessionHeader session;
    SVStreamPassport pass;
    SVStreamState state;
    EXPECT_EQ(RSess::ERR_BAD_SI,
              ProcessBusParser::parse_r_sv_packet(
                  frame.data(), static_cast< int >(frame.size()),
                  RSess::SEC_NONE, crypto, session, pass, state));
}


TEST(RSVFrameSize, FitsStandardMtu)
{
    // See RGooseFrameSize.FitsStandardMtu. R-SV256 is the largest frame we emit.
    constexpr size_t MAX_ETHERNET_FRAME = 1514;

    for (SV_TYPE type : { SV80, SV256 }) {
        for (auto mode : { RSess::SEC_NONE, RSess::SEC_HMAC, RSess::SEC_GCM }) {
            RFrameConfig cfg;
            cfg.mode = mode;

            RSVTrafficGen gen(SV_NUM, type, cfg);
            std::cout << "  R-SV" << ((type == SV80) ? "80 " : "256")
                      << "  " << RSess::to_string(mode)
                      << "  frame = " << gen.GetSkeletonSize() << " bytes\n";

            EXPECT_LE(gen.GetSkeletonSize(), MAX_ETHERNET_FRAME);
        }

        SVTrafficGen l2(SV_NUM, type);
        std::cout << "  L2 SV" << ((type == SV80) ? "80 " : "256")
                  << " (baseline)  frame = " << l2.GetSkeletonSize() << " bytes\n";
    }
}
