#include "bus_generator/goose_traffic_gen.hpp"
#include "bus_processor/process_bus_parser.hpp"

// GOOSE generation by libiec61850
#include "mms_value.h"
#include "goose_publisher.h"

#include <gtest/gtest.h>
#include <stdexcept>

class GooseMakerByLib
{
public:
    size_t MakePacket(uint8_t *buffer)
    {
        CommParameters ethParams = {
            .vlanPriority = 4,
            .vlanId = 101,
            .appId = m_appid,
            .dstAddress = { 0x01, 0x0C, 0xCD, 0x01, 0x00, 0x01 }
        };

        LinkedList dataSetValues = LinkedList_create();
        for (unsigned i=0;i<m_numEntries;++i) {
            LinkedList_add(dataSetValues, MmsValue_newBoolean(i % 2 == 0));
        }

        size_t packetSize = 0;
        GoosePublisher publisher = GoosePublisher_create(&ethParams, "lo");
        if (publisher) {
            GoosePublisher_setGoCbRef(publisher, GetGOCBRef().c_str());
            GoosePublisher_setDataSetRef(publisher, GetDataSetRef().c_str());
            GoosePublisher_setGoID(publisher, m_goid.c_str());
            GoosePublisher_setConfRev(publisher, m_crev);
            GoosePublisher_setTimeAllowedToLive(publisher, 500);

            int retval = GoosePublisher_generateMessage(publisher, &ethParams,
                                                        dataSetValues,
                                                        buffer, MAX_GOOSE_PACKET_SIZE,
                                                        &packetSize);
            if (retval < 0 || packetSize == 0) {
                throw std::runtime_error("Can't generate GOOSE with libiec61850"
                                         + std::to_string(retval));
            }

            GoosePublisher_destroy(publisher);
            LinkedList_destroy(dataSetValues);
        } else {
            throw std::runtime_error("Can't create GoosePublisher from libiec61850!");
        }
        return packetSize;
    }

    GooseMakerByLib& SetAppID(uint16_t appid) {
        m_appid = appid;
        return *this;
    }
    GooseMakerByLib& SetIED(const std::string &ied) {
        m_iedName = ied;
        return *this;
    }
    GooseMakerByLib& SetLD(const std::string &ld) {
        m_ldName = ld;
        return *this;
    }
    GooseMakerByLib& SetDataSet(const std::string &dataset) {
        m_dataSet = dataset;
        return *this;
    }
    GooseMakerByLib& SetGOID(const std::string &goid) {
        m_goid = goid;
        return *this;
    }
    GooseMakerByLib& SetGOCB(const std::string &gocb) {
        m_gocb = gocb;
        return *this;
    }
    GooseMakerByLib& SetCRev(uint32_t crev) {
        m_crev = crev;
        return *this;
    }

    uint16_t         GetAppID() const {
        return m_appid;
    }
    std::string      GetGOID() const {
        return m_goid;
    }
    std::string      GetGOCBRef() const {
        return m_iedName + m_ldName + "/LLN0$GO$" + m_gocb;
    }
    std::string      GetDataSetRef() const {
        return m_iedName + m_ldName + "/LLN0$" + m_dataSet;
    }
    uint32_t         GetCRev() const {
        return m_crev;
    }
    uint32_t         GetNumEntries() const {
        return m_numEntries;
    }

private:
    uint16_t    m_appid = 0x0000;
    std::string m_iedName = "DefaultIED";
    std::string m_ldName = "DefaultLD";
    std::string m_gocb = "DefaultGOCB";
    std::string m_dataSet = "DefaultDataSet";
    std::string m_goid = "DefaultGOID";
    uint32_t    m_crev = 1;
    uint32_t    m_numEntries = 4;
};


TEST(GooseFastParser, BasicUsage)
{
    uint8_t packet[MAX_GOOSE_PACKET_SIZE] = { 0 };
    size_t size = 256;
    GoosePassport passport;
    GooseState state;

    int retval = ProcessBusParser::parse_goose_packet(packet, size, passport, state);
    ASSERT_NE(retval, 0);

    GooseMakerByLib goose;
    size = goose.SetAppID(777)
                .SetIED("IEDName")
                .SetLD("LDName")
                .SetGOCB("GOCBName")
                .SetDataSet("DataSetName")
                .SetCRev(123)
                .MakePacket(packet);
    retval = ProcessBusParser::parse_goose_packet(packet, size, passport, state);
    ASSERT_EQ(retval, 0) << "Can't parse packet: Size = " << size;

    /* std::cout << passport; */
    ASSERT_EQ(passport.appid, goose.GetAppID()) << passport;
    ASSERT_EQ(passport.goid, goose.GetGOID()) << passport;
    ASSERT_EQ(passport.gocbref, goose.GetGOCBRef()) << passport;
    ASSERT_EQ(passport.dataset, goose.GetDataSetRef()) << passport;
    ASSERT_EQ(passport.crev, goose.GetCRev()) << passport;
    ASSERT_EQ(passport.num, goose.GetNumEntries()) << passport;
}

TEST(GooseFastParser, GetProtoType_VLAN)
{
    uint8_t packet[MAX_GOOSE_PACKET_SIZE] = { 0 };
    GooseMakerByLib goose;
    size_t size = goose.SetAppID(0x1234).MakePacket(packet);
    ASSERT_GT(size, 0u);

    unsigned appid = 0;
    BUS_PROTO type = ProcessBusParser::get_proto_type(packet, &appid, size);
    ASSERT_EQ(type, BUS_PROTO_GOOSE);
    ASSERT_EQ(appid, 0x1234) << "get_proto_type returned wrong APPID for VLAN GOOSE";
}

TEST(GooseContainer, BasicUsage)
{
    GooseContainer gooseMap;

    GoosePassport passport;
    ASSERT_EQ(gooseMap.find(passport), gooseMap.end());

    // Test GOOSE packet
    uint8_t packet[MAX_GOOSE_PACKET_SIZE] = { 0 };
    GooseMakerByLib goose;
    GooseState state;
    size_t packetSize = goose.MakePacket(packet);
    int retval = ProcessBusParser::parse_goose_packet(packet, packetSize, passport, state);
    ASSERT_EQ(retval, 0) << passport;

    // GOOSE 1
    auto g1 = std::make_shared< GooseSource >();
    g1->SetMAC(MAC("01:0C:CD:01:12:34"))
        .SetAppID(0x1111)
        .SetDataSetRef("TestDataSetRef1")
        .SetGOCBRef("TestGOCBRef1")
        .SetGOID("TestGOID1")
        .SetCRev(54321)
        .SetNumEntries(16);
    gooseMap[g1->GetPassport()] = g1;

    // GOOSE 2
    auto g2 = std::make_shared< GooseSource >();
    g2->SetMAC(MAC("01:0C:CD:01:12:34"))
        .SetAppID(0x2222)
        .SetDataSetRef("TestDataSetRef2")
        .SetGOCBRef("TestGOCBRef2")
        .SetGOID("TestGOID2")
        .SetCRev(54321)
        .SetNumEntries(16);
    gooseMap[g2->GetPassport()] = g2;

    auto it = gooseMap.find(passport);
    ASSERT_EQ(it, gooseMap.end());

    // Check GOOSE1
    auto itG1 = gooseMap.find(g1->GetPassport());
    ASSERT_NE(itG1, gooseMap.end());
    ASSERT_NE(itG1->second, nullptr);
    ASSERT_EQ(itG1->second->GetPassport(), g1->GetPassport());
    ASSERT_NE(itG1->second->GetPassport(), g2->GetPassport());
    ASSERT_NE(passport, g1->GetPassport());

    // Check GOOSE2
    auto itG2 = gooseMap.find(g2->GetPassport());
    ASSERT_NE(itG2, gooseMap.end());
    ASSERT_NE(itG2->second, nullptr);
    ASSERT_EQ(itG2->second->GetPassport(), g2->GetPassport());
    ASSERT_NE(itG2->second->GetPassport(), g1->GetPassport());
    ASSERT_NE(passport, g2->GetPassport());
}

/*
 * Regression guard for the allData walk. Both mutations are invisible to a
 * parser that skips the dataset, so these fail if the walk is ever removed.
 */
TEST(GooseFastParser, DataSetMutationsAreDetected)
{
    uint8_t packet[MAX_GOOSE_PACKET_SIZE] = { 0 };
    GooseMakerByLib goose;
    size_t size = goose.MakePacket(packet);

    GoosePassport pass;
    GooseState state;
    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, size, pass, state), 0);
    ASSERT_EQ(pass.num, goose.GetNumEntries());

    // numDatSetEntries is "8A 01 <n>", immediately followed by allData "AB <len>".
    size_t at = 0;
    for (size_t i=0;i+3<size;++i) {
        if (packet[i] == 0x8A && packet[i+1] == 0x01
            && packet[i+2] == goose.GetNumEntries() && packet[i+3] == 0xAB) {
            at = i;
            break;
        }
    }
    ASSERT_NE(at, 0u) << "numDatSetEntries/allData not found";

    // 1. Declared count no longer matches what the dataset holds.
    uint8_t saved = packet[at + 2];
    packet[at + 2] = saved + 1;
    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, size, pass, state), -102);
    packet[at + 2] = saved;

    // 2. First entry claims a length that runs past the end of allData.
    uint8_t &firstLen = packet[at + 5 + 1];
    saved = firstLen;
    firstLen = 0x7F;
    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, size, pass, state), -101);
    firstLen = saved;

    ASSERT_EQ(ProcessBusParser::parse_goose_packet(packet, size, pass, state), 0);
}
