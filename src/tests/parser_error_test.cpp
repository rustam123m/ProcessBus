#include "bus_processor/process_bus_parser.hpp"

#include <gtest/gtest.h>

#include <vector>

// --- GOOSE error paths ---

TEST(GooseParserErrors, PacketTooSmall)
{
    uint8_t packet[63] = {0};
    GoosePassport pass;
    GooseState state;

    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, 0, pass, state), -1);
    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, 32, pass, state), -1);
    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, 63, pass, state), -1);
}

TEST(GooseParserErrors, WrongEtherType)
{
    // 64-byte packet with wrong EtherType at bytes 12-13
    uint8_t packet[64] = {0};
    packet[12] = 0x08; // IPv4
    packet[13] = 0x00;

    GoosePassport pass;
    GooseState state;
    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, 64, pass, state), -1);
}

TEST(GooseParserErrors, WrongPDUTag)
{
    // Non-VLAN GOOSE with wrong PDU tag
    uint8_t packet[64] = {0};
    // DMAC + SMAC (12 bytes)
    packet[12] = 0x88; packet[13] = 0xB8; // GOOSE EtherType
    // APPID(2) + Len(2) + Res(4) = 8 bytes → PDU at offset 22
    packet[22] = 0x99; // wrong PDU tag (should be 0x61)

    GoosePassport pass;
    GooseState state;
    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, 64, pass, state), -3);
}

TEST(GooseParserErrors, MissingRequiredFields)
{
    // Valid GOOSE frame structure but no gocbRef/datSet/goID fields
    uint8_t packet[64] = {0};
    packet[12] = 0x88; packet[13] = 0xB8; // GOOSE EtherType
    packet[22] = 0x61; // PDU tag
    packet[23] = 0x02; // PDU length = 2
    // Only a timestamp field, no required fields
    packet[24] = 0x84; // timestamp tag
    packet[25] = 0x00; // but itemSize == 0 triggers -3

    GoosePassport pass;
    GooseState state;
    int retval = ProcessBusParser::parse_goose_packet(packet, 64, pass, state);
    // Either -3 (malformed TLV) or -100 (missing fields) — both are error paths
    ASSERT_NE(retval, 0);
}

// --- SV error paths ---

namespace
{
    void put_sv_len(std::vector<uint8_t> &out, size_t length)
    {
        if (length < 0x80) {
            out.push_back(static_cast< uint8_t >(length));
        } else if (length <= 0xff) {
            out.push_back(0x81);
            out.push_back(static_cast< uint8_t >(length));
        } else {
            ASSERT_LE(length, 0xffffu);
            out.push_back(0x82);
            out.push_back(static_cast< uint8_t >(length >> 8));
            out.push_back(static_cast< uint8_t >(length));
        }
    }

    std::vector<uint8_t> make_sv(uint8_t declaredAsdus, unsigned actualAsdus)
    {
        std::vector<uint8_t> sequence;
        for (unsigned i=0;i<actualAsdus;++i) {
            std::vector<uint8_t> asdu = {
                0x80,0x01,'S',                         // svID
                0x82,0x02,0x00,static_cast<uint8_t>(i), // smpCnt
                0x83,0x04,0x00,0x00,0x00,0x01,         // confRev
                0x85,0x01,0x01,                        // smpSynch
                0x87,0x40                               // eight value/Quality pairs
            };
            for (unsigned signal=0;signal<8;++signal) {
                asdu.insert(asdu.end(), {
                    0x00,0x00,static_cast<uint8_t>(i),
                    static_cast<uint8_t>(signal),        // INT32 value
                    0x00,0x00,0x00,0x00                 // Quality = good
                });
            }
            sequence.push_back(0x30);
            put_sv_len(sequence, asdu.size());
            sequence.insert(sequence.end(), asdu.begin(), asdu.end());
        }

        std::vector<uint8_t> pdu = {0x80,0x01,declaredAsdus,0xa2};
        put_sv_len(pdu, sequence.size());
        pdu.insert(pdu.end(), sequence.begin(), sequence.end());

        // Ethernet + non-VLAN SV application header.
        std::vector<uint8_t> frame(22, 0);
        frame[12] = 0x88;
        frame[13] = 0xba;
        frame[15] = 0x01; // APPID
        frame.push_back(0x60);
        put_sv_len(frame, pdu.size());
        frame.insert(frame.end(), pdu.begin(), pdu.end());
        return frame;
    }

    size_t nth_sv_data_tag(const std::vector<uint8_t> &frame, unsigned wanted)
    {
        unsigned found = 0;
        for (size_t i=0;i + 1<frame.size();++i) {
            if (frame[i] == 0x87 && frame[i + 1] == 0x40) {
                if (found++ == wanted) {
                    return i;
                }
            }
        }
        return frame.size();
    }

    int parse_sv(const std::vector<uint8_t> &frame)
    {
        SVStreamPassport pass;
        SVStreamState state;
        return ProcessBusParser::parse_sv_packet(
                   frame.data(), static_cast< int >(frame.size()), pass, state);
    }
}

TEST(SVParserErrors, PacketTooSmall)
{
    uint8_t packet[63] = {0};
    SVStreamPassport pass;
    SVStreamState state;

    ASSERT_EQ(ProcessBusParser::parse_sv_packet(packet, 0, pass, state), -1);
    ASSERT_EQ(ProcessBusParser::parse_sv_packet(packet, 32, pass, state), -1);
    ASSERT_EQ(ProcessBusParser::parse_sv_packet(packet, 63, pass, state), -1);
}

TEST(SVParserErrors, WrongEtherType)
{
    uint8_t packet[64] = {0};
    packet[12] = 0x08; // IPv4
    packet[13] = 0x00;

    SVStreamPassport pass;
    SVStreamState state;
    ASSERT_EQ(ProcessBusParser::parse_sv_packet(packet, 64, pass, state), -2);
}

TEST(SVParserErrors, WrongPDUTag)
{
    // Non-VLAN SV with wrong PDU tag
    uint8_t packet[64] = {0};
    packet[12] = 0x88; packet[13] = 0xBA; // SV EtherType
    // PDU at offset 22
    packet[22] = 0x99; // wrong tag (should be 0x60)

    SVStreamPassport pass;
    SVStreamState state;
    ASSERT_EQ(ProcessBusParser::parse_sv_packet(packet, 64, pass, state), -3);
}

TEST(SVParserErrors, MissingASDUSequenceTag)
{
    // SV with correct PDU tag but wrong ASDU sequence tag
    uint8_t packet[64] = {0};
    packet[12] = 0x88; packet[13] = 0xBA; // SV EtherType
    packet[22] = 0x60; // PDU tag
    packet[23] = 0x06; // PDU length
    packet[24] = 0x80; // noASDU tag
    packet[25] = 0x01; // length 1
    packet[26] = 0x01; // value: 1 ASDU
    packet[27] = 0x99; // wrong ASDU sequence tag (should be 0xa2)

    SVStreamPassport pass;
    SVStreamState state;
    ASSERT_EQ(ProcessBusParser::parse_sv_packet(packet, 64, pass, state), -4);
}

TEST(SVParserErrors, MissingASDUTag)
{
    // SV with correct ASDU sequence but wrong first ASDU tag
    uint8_t packet[64] = {0};
    packet[12] = 0x88; packet[13] = 0xBA; // SV EtherType
    packet[22] = 0x60; // PDU tag
    packet[23] = 0x07; // PDU length
    packet[24] = 0x80; // noASDU tag
    packet[25] = 0x01; // length 1
    packet[26] = 0x01; // value: 1 ASDU
    packet[27] = 0xa2; // ASDU sequence tag
    packet[28] = 0x02; // sequence length
    packet[29] = 0x99; // wrong ASDU tag (should be 0x30)

    SVStreamPassport pass;
    SVStreamState state;
    ASSERT_EQ(ProcessBusParser::parse_sv_packet(packet, 64, pass, state), -5);
}

TEST(SVParserValidation, AcceptsAllDeclaredASDUs)
{
    const std::vector<uint8_t> frame = make_sv(8, 8);
    ASSERT_EQ(parse_sv(frame), 0);
}

TEST(SVParserValidation, RejectsBadQualityInLastASDU)
{
    std::vector<uint8_t> frame = make_sv(8, 8);
    const size_t dataTag = nth_sv_data_tag(frame, 7);
    ASSERT_LT(dataTag, frame.size());

    // Corrupt the last Quality field of the last ASDU.
    frame[dataTag + 2 + 7 * 8 + 4] = 0x01;
    ASSERT_EQ(parse_sv(frame), SV_PARSE_ERR_QUALITY);
}

TEST(SVParserValidation, RejectsMissingDataInLastASDU)
{
    std::vector<uint8_t> frame = make_sv(8, 8);
    const size_t dataTag = nth_sv_data_tag(frame, 7);
    ASSERT_LT(dataTag, frame.size());

    frame[dataTag] = 0x89; // bounded unknown field; mandatory data is now absent
    ASSERT_NE(parse_sv(frame), 0);
}

TEST(SVParserValidation, RejectsASDUCountMismatch)
{
    ASSERT_NE(parse_sv(make_sv(8, 7)), 0);
}

// --- get_proto_type ---

TEST(ProtoType, UnknownEtherType)
{
    uint8_t packet[20] = {0};
    packet[12] = 0x08; // IPv4
    packet[13] = 0x00;

    unsigned appid = 0;
    ASSERT_EQ(ProcessBusParser::get_proto_type(packet, &appid, sizeof(packet)), NON_BUS_PROTO);
}

TEST(ProtoType, NonVLAN_SV)
{
    uint8_t packet[20] = {0};
    packet[12] = 0x88; packet[13] = 0xBA; // SV EtherType
    packet[14] = 0x00; packet[15] = 0x42; // APPID = 0x0042

    unsigned appid = 0;
    ASSERT_EQ(ProcessBusParser::get_proto_type(packet, &appid, sizeof(packet)), BUS_PROTO_SV);
    ASSERT_EQ(appid, 0x0042);
}

// --- allData walk ---

namespace
{
    size_t len_size(size_t len)
    {
        if (len < 0x80) {
            return 1;
        }
        return (len <= 0xFF) ? 2 : 3;
    }

    void put_len(std::vector<uint8_t> &out, size_t len)
    {
        if (len < 0x80) {
            out.push_back(uint8_t(len));
        } else if (len <= 0xFF) {
            out.push_back(0x81);
            out.push_back(uint8_t(len));
        } else {
            out.push_back(0x82);
            out.push_back(uint8_t(len >> 8));
            out.push_back(uint8_t(len));
        }
    }

    /*
     * Minimal non-VLAN GOOSE. Entries are appended verbatim so a test can build
     * a dataset that disagrees with numDatSetEntries. The PDU is padded via the
     * GoID: parse_goose_packet walks to `size`.
     */
    std::vector<uint8_t> make_goose(uint8_t declaredEntries,
                                    const std::vector<uint8_t> &entries)
    {
        std::vector<uint8_t> goid(4, 'G');
        for (;;) {
            const size_t allDataLen = 1 + len_size(entries.size()) + entries.size();
            const size_t goidLen    = 1 + len_size(goid.size()) + goid.size();
            const size_t pduLen     = 6 + 6 + goidLen + 3 + 3 + 3 + allDataLen;
            const size_t total      = 22 + 1 + len_size(pduLen) + pduLen;

            if (total >= 64) {
                std::vector<uint8_t> p = {
                    0x01,0x0C,0xCD,0x04,0x00,0x00, 0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,
                    0x88,0xB8, 0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
                };
                p.push_back(0x61);
                put_len(p, pduLen);
                p.insert(p.end(), {0x80,0x04,'T','E','S','T',
                                   0x82,0x04,'T','E','S','T'});
                p.push_back(0x83);
                put_len(p, goid.size());
                p.insert(p.end(), goid.begin(), goid.end());
                p.insert(p.end(), {0x85,0x01,0x01, 0x86,0x01,0x00,
                                   0x8A,0x01, declaredEntries});
                p.push_back(0xAB);
                put_len(p, entries.size());
                p.insert(p.end(), entries.begin(), entries.end());
                return p;
            }
            goid.push_back('G');
        }
    }

    int parse(const std::vector<uint8_t> &p)
    {
        GoosePassport pass;
        GooseState state;
        return ProcessBusParser::parse_goose_packet(p.data(), int(p.size()), pass, state);
    }
}

TEST(GooseDataSet, CountMatchesIsAccepted)
{
    ASSERT_EQ(parse(make_goose(3, {0x83,0x01,0x00, 0x83,0x01,0x01, 0x83,0x01,0x00})), 0);
}

TEST(GooseDataSet, DeclaredMoreThanPresent)
{
    ASSERT_EQ(parse(make_goose(4, {0x83,0x01,0x00, 0x83,0x01,0x01})), -102);
}

TEST(GooseDataSet, DeclaredFewerThanPresent)
{
    ASSERT_EQ(parse(make_goose(1, {0x83,0x01,0x00, 0x83,0x01,0x01})), -102);
}

TEST(GooseDataSet, TruncatedEntryIsRejected)
{
    // Last entry claims one value byte that lies outside allData.
    ASSERT_EQ(parse(make_goose(2, {0x83,0x01,0x00, 0x83,0x01})), -101);
}

TEST(GooseDataSet, EntryLengthOverrunsDataSet)
{
    ASSERT_EQ(parse(make_goose(1, {0x83,0x7F,0x00})), -101);
}

TEST(GooseDataSet, TagWithoutLengthByte)
{
    ASSERT_EQ(parse(make_goose(1, {0x83})), -101);
}

TEST(GooseDataSet, LongFormEntryLength)
{
    // 0x81 0x80 = long form, 128 value bytes; entry is 131 bytes total.
    std::vector<uint8_t> e = {0x84, 0x81, 0x80};
    e.insert(e.end(), 128, 0xAA);
    ASSERT_EQ(parse(make_goose(1, e)), 0);
}

TEST(GooseDataSet, FoundEntriesReportsWhatWasWalked)
{
    const auto p = make_goose(3, {0x83,0x01,0x00, 0x83,0x01,0x01, 0x83,0x01,0x00});
    GoosePassport pass;
    GooseState state;

    ASSERT_EQ(ProcessBusParser::parse_goose_packet(p.data(), int(p.size()), pass, state), 0);
    ASSERT_EQ(pass.num, 3u);
    ASSERT_EQ(pass.foundEntries, 3u);
}

TEST(GooseDataSet, FoundEntriesIsSetOnMismatchToo)
{
    const auto p = make_goose(4, {0x83,0x01,0x00, 0x83,0x01,0x01});
    GoosePassport pass;
    GooseState state;

    ASSERT_EQ(ProcessBusParser::parse_goose_packet(p.data(), int(p.size()), pass, state), -102);
    ASSERT_EQ(pass.num, 4u)          << "declared value is kept";
    ASSERT_EQ(pass.foundEntries, 2u) << "counted value tells which side is wrong";
}

TEST(GooseDataSet, FoundEntriesIsNotPartOfIdentity)
{
    // Registered sources never set it; comparing it would break every lookup.
    GoosePassport a, b;
    a.appid = b.appid = 1;
    a.num = b.num = 4;
    a.foundEntries = 4;
    b.foundEntries = 0;

    ASSERT_TRUE(a == b);
}
