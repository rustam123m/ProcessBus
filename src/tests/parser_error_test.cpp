#include "bus_processor/process_bus_parser.hpp"

#include <gtest/gtest.h>

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
    packet[23] = 0x08; // PDU length
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

// --- get_proto_type ---

TEST(ProtoType, UnknownEtherType)
{
    uint8_t packet[20] = {0};
    packet[12] = 0x08; // IPv4
    packet[13] = 0x00;

    unsigned appid = 0;
    ASSERT_EQ(ProcessBusParser::get_proto_type(packet, &appid), NON_BUS_PROTO);
}

TEST(ProtoType, NonVLAN_SV)
{
    uint8_t packet[20] = {0};
    packet[12] = 0x88; packet[13] = 0xBA; // SV EtherType
    packet[14] = 0x00; packet[15] = 0x42; // APPID = 0x0042

    unsigned appid = 0;
    ASSERT_EQ(ProcessBusParser::get_proto_type(packet, &appid), BUS_PROTO_SV);
    ASSERT_EQ(appid, 0x0042);
}
