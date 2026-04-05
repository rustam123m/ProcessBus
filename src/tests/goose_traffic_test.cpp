#include "bus_generator/goose_traffic_gen.hpp"
#include "bus_processor/process_bus_parser.hpp"

// GOOSE parsing by libiec61850 (receiver — no AF_PACKET needed)
#include "mms_value.h"
#include "goose_publisher.h"

#include "goose_receiver.h"
#include "goose_subscriber.h"

#include <gtest/gtest.h>
#include <stdexcept>

class GooseParserByLib
{
public:
    GooseParserByLib()
    {
        char port[8] = "";
        m_subscriber = GooseSubscriber_create(port, nullptr);
        if (m_subscriber == nullptr) {
            throw std::runtime_error("Can't create GooseSubscriber from libiec61850!");
        }
        GooseSubscriber_setObserver(m_subscriber);
        GooseSubscriber_setListener(m_subscriber, GooseParserByLib::callback_handler, this);

        m_receiver = GooseReceiver_create();
        if (m_receiver == nullptr) {
            throw std::runtime_error("Can't create GooseReceiver from libiec61850!");
        }
        GooseReceiver_addSubscriber(m_receiver, m_subscriber);
    }

    int ParseGoose(uint8_t *buffer, size_t size)
    {
        m_isFound = false;
        GooseReceiver_handleMessage(m_receiver, buffer, size);
        return m_isFound ? 0 : -1;
    }

    static void callback_handler(GooseSubscriber subscriber, void* parameter)
    {
        GooseParserByLib *obj = reinterpret_cast< GooseParserByLib* >(parameter);
        obj->m_isFound = true;

        /* print_goose(obj->m_subscriber); */
    }

    static void print_goose(GooseSubscriber subscriber)
    {
        printf("GOOSE message:\n");
        printf("\tvlanTag: %s\n", GooseSubscriber_isVlanSet(subscriber) ? "found" : "NOT found");
        if (GooseSubscriber_isVlanSet(subscriber)) {
            printf("\t\tvlanId: %u\n", GooseSubscriber_getVlanId(subscriber));
            printf("\t\tvlanPrio: %u\n", GooseSubscriber_getVlanPrio(subscriber));
        }

        printf("\tappId: %d\n", GooseSubscriber_getAppId(subscriber));
        uint8_t macBuf[6];
        GooseSubscriber_getSrcMac(subscriber,macBuf);
        printf("\tsrcMac: %02X:%02X:%02X:%02X:%02X:%02X\n",
                macBuf[0],macBuf[1],macBuf[2],macBuf[3],macBuf[4],macBuf[5]);
        GooseSubscriber_getDstMac(subscriber,macBuf);
        printf("\tdstMac: %02X:%02X:%02X:%02X:%02X:%02X\n",
                macBuf[0],macBuf[1],macBuf[2],macBuf[3],macBuf[4],macBuf[5]);
        printf("\tgoId: %s\n", GooseSubscriber_getGoId(subscriber));
        printf("\tgoCbRef: %s\n", GooseSubscriber_getGoCbRef(subscriber));
        printf("\tdataSet: %s\n", GooseSubscriber_getDataSet(subscriber));
        printf("\tconfRev: %u\n", GooseSubscriber_getConfRev(subscriber));
        printf("\tndsCom: %s\n", GooseSubscriber_needsCommission(subscriber) ? "true" : "false");
        printf("\tsimul: %s\n", GooseSubscriber_isTest(subscriber) ? "true" : "false");
        printf("\tstNum: %u sqNum: %u\n", GooseSubscriber_getStNum(subscriber),
                GooseSubscriber_getSqNum(subscriber));
        printf("\ttimeToLive: %u\n", GooseSubscriber_getTimeAllowedToLive(subscriber));

        uint64_t timestamp = GooseSubscriber_getTimestamp(subscriber);
        printf("\ttimestamp: %u.%u\n", (uint32_t) (timestamp / 1000), (uint32_t) (timestamp % 1000));
        printf("\tmessage is %s\n", GooseSubscriber_isValid(subscriber) ? "valid" : "INVALID");

        char buffer[1024];
        MmsValue* values = GooseSubscriber_getDataSetValues(subscriber);
        MmsValue_printToBuffer(values, buffer, 1024);
        printf("\tAllData: %s\n", buffer);
    }

private:
    GooseReceiver   m_receiver;
    GooseSubscriber m_subscriber;
    bool            m_isFound = false;
};


TEST(BusGenerator, BasicUsage)
{
    // Generate a bunch of Goose messages for fast modify
    const unsigned GooseNum = 10, SndFreq = 1, SignalNum = 16;
    GooseTrafficGen gen(GooseNum, SndFreq, SignalNum);

    std::vector< uint8_t > buffer(MAX_GOOSE_PACKET_SIZE);
    memcpy(buffer.data(), gen.GetSkeletonBuffer(), gen.GetSkeletonSize());

    auto units = gen.GetTxUnits();
    for (const auto &unit : units) {
        for (size_t i=0;i<unit.blocks.size();++i) {
            auto &blk = unit.blocks[i];

            for (size_t j=0;j<blk.packets.size();++j) {
                gen.AmendPacket(buffer.data(), blk.packets[j]);

                // Check the packet validity
            }
            // Sending this block
        }
    }
}

TEST(BusGenerator, CheckPacketsByLibiec61850)
{
    GooseParserByLib gooseLibPaser;

    // Generate a bunch of Goose messages for fast modify
    const unsigned GooseNum = 10, SndFreq = 10, SignalNum = 16;
    /* const unsigned GooseNum = 1000, SndFreq = 1000, SignalNum = 16; */
    GooseTrafficGen gen(GooseNum, SndFreq, SignalNum);

    std::vector< uint8_t > buffer(MAX_GOOSE_PACKET_SIZE);
    memcpy(buffer.data(), gen.GetSkeletonBuffer(), gen.GetSkeletonSize());

    auto &units = gen.GetTxUnits();
    for (const auto &unit : units) {
        for (size_t i=0;i<unit.blocks.size();++i) {
            auto &blk = unit.blocks[i];

            for (size_t j=0;j<blk.packets.size();++j) {
                gen.AmendPacket(buffer.data(), blk.packets[j]);

                // Check the packet validity by libiec61850
                int retval = gooseLibPaser.ParseGoose(buffer.data(), gen.GetSkeletonSize());
                ASSERT_EQ(retval, 0) << "Block = " << i << ", packet = " << j;
            }
        }
    }
}

TEST(GooseFastParser, Timestamp4Bytes)
{
    // Minimal non-VLAN GOOSE with a 4-byte timestamp = 0xAABBCCDD
    uint8_t packet[] = {
        0x01, 0x0C, 0xCD, 0x04, 0x00, 0x00,               // DMAC
        0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5,               // SMAC
        0x88, 0xB8,                                         // EtherType (GOOSE)
        0x00, 0x01, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00,    // APPID, Len, Res1, Res2
        0x61, 0x28,                                         // PDU tag + length(40)
        0x80, 0x04, 0x54, 0x45, 0x53, 0x54,                // gocbRef = "TEST"
        0x82, 0x04, 0x54, 0x45, 0x53, 0x54,                // datSet  = "TEST"
        0x83, 0x04, 0x54, 0x45, 0x53, 0x54,                // goID    = "TEST"
        0x84, 0x04, 0xAA, 0xBB, 0xCC, 0xDD,                // timestamp (4 bytes)
        0x85, 0x01, 0x01,                                   // stNum = 1
        0x86, 0x01, 0x00,                                   // sqNum = 0
        0x8A, 0x01, 0x01,                                   // numDatSetEntries = 1
        0xAB, 0x05, 0x83, 0x01, 0x00, 0x83, 0x01,          // allData (pad to 64)
    };
    static_assert(sizeof(packet) == 64);

    GoosePassport passport;
    GooseState state;
    int retval = ProcessBusParser::parse_goose_packet(packet, sizeof(packet), passport, state);
    ASSERT_EQ(retval, 0);
    ASSERT_EQ(state.timestamp, 0xAABBCCDD) << "4-byte timestamp must not include adjacent bytes";
}

TEST(GooseFastParser, GetProtoType_NonVLAN)
{
    // Non-VLAN GOOSE: EtherType 0x88B8 at bytes 12-13, APPID 0x0001 at bytes 14-15
    uint8_t packet[] = {
        0x01, 0x0C, 0xCD, 0x04, 0x00, 0x00, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5,
        0x88, 0xB8, 0x00, 0x01, 0x00, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x61, 0x81,
        0xA6, 0x80, 0x1E, 0x49, 0x45, 0x44, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x30, 0x31
    };

    unsigned appid = 0;
    BUS_PROTO type = ProcessBusParser::get_proto_type(packet, &appid);
    ASSERT_EQ(type, BUS_PROTO_GOOSE);
    ASSERT_EQ(appid, 0x0001) << "get_proto_type returned wrong APPID for non-VLAN GOOSE";
}

TEST(GooseFastParser, RealPacket)
{
    uint8_t packet[] = {
        0x01, 0x0C, 0xCD, 0x04, 0x00, 0x00, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5,
        0x88, 0xB8, 0x00, 0x01, 0x00, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x61, 0x81,
        0xA6, 0x80, 0x1E, 0x49, 0x45, 0x44, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x30, 0x31, 0x4C, 0x44, 0x4E, 0x61, 0x6D, 0x65, 0x2F, 0x4C, 0x4C, 0x4E,
        0x30, 0x24, 0x47, 0x4F, 0x24, 0x47, 0x4F, 0x43, 0x42, 0x81, 0x02, 0x07,
        0xD0, 0x82, 0x1E, 0x49, 0x45, 0x44, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x30, 0x31, 0x4C, 0x44, 0x4E, 0x61, 0x6D, 0x65, 0x2F, 0x4C, 0x4C, 0x4E,
        0x30, 0x24, 0x44, 0x61, 0x74, 0x61, 0x53, 0x65, 0x74, 0x83, 0x0C, 0x47,
        0x4F, 0x49, 0x44, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x84,
        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x85, 0x04, 0x00,
        0x00, 0x00, 0x04, 0x86, 0x04, 0x00, 0x00, 0x00, 0x00, 0x87, 0x01, 0x00,
        0x88, 0x01, 0x01, 0x89, 0x01, 0x00, 0x8A, 0x01, 0x10, 0xAB, 0x30, 0x83,
        0x01, 0x01, 0x83, 0x01, 0x01, 0x83, 0x01, 0x00, 0x83, 0x01, 0x01, 0x83,
        0x01, 0x00, 0x83, 0x01, 0x01, 0x83, 0x01, 0x00, 0x83, 0x01, 0x01, 0x83,
        0x01, 0x00, 0x83, 0x01, 0x01, 0x83, 0x01, 0x00, 0x83, 0x01, 0x01, 0x83,
        0x01, 0x00, 0x83, 0x01, 0x01, 0x83, 0x01, 0x00, 0x83, 0x01, 0x01
    };

    GoosePassport passport;
    GooseState state;

    int retval = ProcessBusParser::parse_goose_packet(packet, sizeof(packet), passport, state);
    ASSERT_EQ(retval, 0);

    GooseParserByLib gooseLibPaser;
    retval = gooseLibPaser.ParseGoose(packet, sizeof(packet));
    ASSERT_EQ(retval, 0);
}
