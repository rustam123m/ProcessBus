#include "dpdk_cpp/dpdk_port_class.hpp"
#include "dpdk_cpp/dpdk_mempool_class.hpp"
#include "dpdk_cpp/dpdk_poolsetter_class.hpp"
#include "dpdk_cpp/dpdk_clocks_class.hpp"
#include "common/utils.hpp"
#include "common/platform_config.hpp"

#include <csignal>
#include <rte_ip.h>
#include <rte_udp.h>

static constexpr uint16_t RX_DESC       = Platform::TOOL_DESC_NUM;
static constexpr uint16_t TX_DESC       = Platform::TOOL_DESC_NUM;
static constexpr unsigned NB_MBUFS      = Platform::TOOL_MBUF_NUM;
static constexpr unsigned MBUF_CACHE    = 32;
static constexpr uint16_t BURST         = 32;
static constexpr uint16_t TX_BURST      = 1;
static constexpr uint16_t PACKET_SIZE   = 1500;

#pragma pack(push, 1)
struct UdpMeasurePacket
{
    union {
        struct {
            rte_ether_hdr   eth;
            rte_ipv4_hdr    ip;
            rte_udp_hdr     udp;

            uint64_t        tsc;  // payload
        };
        uint8_t padding[PACKET_SIZE];
    };
};
#pragma pack(pop)

struct DelayMeterConfig
{
    rte_ether_addr  dmac;
    rte_ether_addr  smac;

    uint32_t        srcIP = 0;
    uint32_t        dstIP = 0;
    uint16_t        srcPort = 0;
    uint16_t        dstPort = 0;
};

struct DelayStatistic
{
    uint64_t txPktNum = 0;
    uint64_t rxPtkNum = 0;
    uint64_t sumAllDeltas = 0;
    uint64_t maxDeltaClk = 0;
    uint64_t minDeltaClk = UINT64_MAX;
    uint64_t prevCycleClk = 0;

    inline void print() {
        double avg_us = rxPtkNum ? (double)sumAllDeltas / rxPtkNum * 1e6 / rte_get_tsc_hz() : 0.0;
        double max_us = (double)maxDeltaClk * 1e6 / rte_get_tsc_hz();
        double min_us = (double)minDeltaClk * 1e6 / rte_get_tsc_hz();

        printf("TX=%" PRIu64 " RX=%" PRIu64 ": avg = %.3f, max = %.3f, min = %.3f us\n",
               txPktNum, rxPtkNum, avg_us, max_us, min_us);
    }

    inline void put(uint64_t delta) {
        sumAllDeltas += delta;
        if (delta > maxDeltaClk) {
            maxDeltaClk = delta;
        }
        if (delta < minDeltaClk) {
            minDeltaClk = delta;
        }
        ++rxPtkNum;
    }

    inline void reset(uint64_t lastCycleClk) {
        txPktNum = 0;
        rxPtkNum = 0;
        sumAllDeltas = 0;
        maxDeltaClk = 0;
        minDeltaClk = UINT64_MAX;
        prevCycleClk = lastCycleClk;
    }
};

size_t init_mease_packet(uint8_t *skeleton, const DelayMeterConfig &conf)
{
    const uint16_t payload_len = sizeof(uint64_t);
    const uint16_t udp_len = (uint16_t)(sizeof(rte_udp_hdr) + payload_len);
    const uint16_t ip_len  = (uint16_t)(sizeof(rte_ipv4_hdr) + udp_len);

    UdpMeasurePacket &pkt = *(UdpMeasurePacket *)skeleton;
    // ETH
    pkt.eth.dst_addr = conf.dmac;
    pkt.eth.src_addr = conf.smac;
    pkt.eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    // IPv4
    pkt.ip.version_ihl   = (4u << 4) | (sizeof(rte_ipv4_hdr) / 4);
    pkt.ip.type_of_service = 0;
    pkt.ip.packet_id     = 0;
    pkt.ip.fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    pkt.ip.time_to_live  = 64;
    pkt.ip.next_proto_id = IPPROTO_UDP;
    pkt.ip.src_addr      = conf.srcIP;
    pkt.ip.dst_addr      = conf.dstIP;

    // UDP
    pkt.udp.src_port     = conf.srcPort;
    pkt.udp.dst_port     = conf.dstPort;

    // set lengths
    pkt.udp.dgram_len = rte_cpu_to_be_16(udp_len);
    pkt.ip.total_length = rte_cpu_to_be_16(ip_len);

    // checksums (software)
    pkt.ip.hdr_checksum = 0;
    /* pkt.ip.hdr_checksum = rte_ipv4_cksum(&pkt.ip); */
    pkt.udp.dgram_cksum = 0;
    /* pkt.udp.dgram_cksum = rte_ipv4_udptcp_cksum(&pkt.ip, &pkt.udp); */

    // payload = tsc now
    pkt.tsc = 0;
    return sizeof(UdpMeasurePacket);
}

volatile unsigned g_doWork = 0;
static void signal_handler(int sig)
{
    g_doWork = 1;
}

static void delay_meter(int argc, char *argv[])
{
    // TX port, RX port
    uint16_t port_tx = 0, port_rx = 1;
    if (rte_eth_dev_count_avail() < 2) {
        rte_exit(EXIT_FAILURE, "Need at least 2 DPDK ports\n");
    }

    // RT
    init_linuxrt();

    // TX port
    DPDK::Mempool txPool("delay_meter_tx", NB_MBUFS, MBUF_CACHE);
    DPDK::Port portTx = DPDK::PortBuilder(port_tx)
                        .SetMemPool(txPool.GetPtr())
                        .AdjustQueues(1, 1)
                        .SetDescriptors(RX_DESC, TX_DESC)
                        .Build();
    // RX port
    DPDK::Mempool rxPool("delay_meter_rx", NB_MBUFS, MBUF_CACHE);
    DPDK::Port portRx = DPDK::PortBuilder(port_rx)
                        .SetMemPool(rxPool.GetPtr())
                        .AdjustQueues(1, 1)
                        .SetDescriptors(RX_DESC, RX_DESC)
                        .Build();

    // DelayMeter configuration
    DelayMeterConfig conf;
    portTx.GetMAC(conf.smac);
    portRx.GetMAC(conf.dmac);
    conf.srcIP = rte_cpu_to_be_32(0x0a000001); // 10.0.0.1
    conf.dstIP = rte_cpu_to_be_32(0x0a000002); // 10.0.0.2
    conf.srcPort = rte_cpu_to_be_16(12345);
    conf.dstPort = rte_cpu_to_be_16(12345);

    // Set skeleton for each mbuf
    uint8_t skeleton[1500] = { 0 };
    size_t pktSize = init_mease_packet(skeleton, conf);
    DPDK::PoolSetter(skeleton, pktSize).FillPackets(txPool.GetPtr());
    std::cout << "DelayMeter packet size: " << pktSize << std::endl;

    portTx.Start();
    portRx.Start();
    if ( !(portTx.WaitLink(10) && portRx.WaitLink(10)) ) {
        rte_exit(EXIT_FAILURE, "Link is down!");
    }

    // Link info
    std::cout << "RX: \n" << portRx << "\nTX: \n" << portTx << "\n";

    // RT
    set_thread_priority(80);

    // Statistics
    DelayStatistic stat;

    std::cout << "Start main cycle" << std::endl;

    // Main loop
    const uint64_t tskClkPerSec = rte_get_tsc_hz(),
                   waitClk = tskClkPerSec / 1000; // 1ms
    rte_mbuf* mbuf[BURST] = {};
    while (g_doWork == 0) {
        // TX burst
        uint64_t sendClk = 0;
        if (rte_pktmbuf_alloc_bulk(txPool.GetPtr(), mbuf, TX_BURST) == 0) {
            sendClk = rte_rdtsc();
            for (uint16_t i=0;i<TX_BURST;++i) {
                UdpMeasurePacket *p = (UdpMeasurePacket *)rte_pktmbuf_mtod(mbuf[i], uint8_t*);
                p->tsc = sendClk;

                mbuf[i]->pkt_len = sizeof(UdpMeasurePacket);
                mbuf[i]->data_len = sizeof(UdpMeasurePacket);
            }

            // Send frame
            uint16_t num = rte_eth_tx_burst(portTx.GetID(), 0, mbuf, TX_BURST);
            stat.txPktNum += num;

            // Free unsent
            for (uint16_t i=num;i<TX_BURST;++i) {
                if (mbuf[i]) {
                    rte_pktmbuf_free(mbuf[i]);
                }
            }
        } else {
            std::cerr << "Can't allocate mbufs!\n"
                      << txPool << "\n"
                      << rxPool;
            continue;
        }

        // RX burst
        uint64_t currentClk = 0;
        do {
            uint16_t rxNum = rte_eth_rx_burst(portRx.GetID(), 0, mbuf, BURST);
            currentClk = rte_rdtsc();
            if (rxNum == 0) {
                continue;
            }

            for (uint16_t i=0;i<rxNum;++i) {
                if (rte_pktmbuf_data_len(mbuf[i]) == sizeof(UdpMeasurePacket)) {
                    auto* p = (UdpMeasurePacket *)(rte_pktmbuf_mtod(mbuf[i], const void*));
                    if (/* p->eth.ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)
                        && p->ip.next_proto_id == IPPROTO_UDP
                        && */
                        p->udp.src_port == conf.srcPort
                        && p->udp.dst_port == conf.dstPort) {

                        stat.put(currentClk - p->tsc);
                    }
                }
            }
            rte_pktmbuf_free_bulk(mbuf, rxNum);
        } while ((currentClk - sendClk)  < waitClk);

        // Statistics each second
        if (currentClk - stat.prevCycleClk > tskClkPerSec) {
            stat.print();

            stat.reset(currentClk);
        }

        usleep(5 * 1000);
    }
}

int main(int argc, char* argv[])
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "EAL init failed\n");
    }
    argc -= ret;
    argv += ret;

    std::signal(SIGINT, signal_handler);  // Ctrl+C
    std::signal(SIGTERM, signal_handler); // kill -TERM

    // App
    delay_meter(argc, argv);

    rte_eal_cleanup();
    return 0;
}

