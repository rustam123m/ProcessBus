/*
 * Pure RX-path counter: rx_burst -> free_bulk, no parsing, no demux.
 *
 * Use as a baseline against bus_processor: if rx_counter pps ≈
 * bus_processor pps, the bottleneck is below the app (PMD / NIC / RAM
 * / PCIe). If rx_counter pps is materially higher, time is being
 * spent in the processor's parse/demux/handoff hot path.
 *
 * Mirrors bus_processor's port shape (Platform::PROCESSOR_*) so the
 * comparison is apples-to-apples — same RX descriptor depth, same
 * mempool size, same udmabuf-backed descriptor ring on OPI3B.
 */

#include "dpdk_cpp/dpdk_port_class.hpp"
#include "dpdk_cpp/dpdk_mempool_class.hpp"
#include "common/utils.hpp"
#include "common/platform_config.hpp"
#ifdef PLATFORM_ORANGEPI3B
#  include "dpdk_cpp/udmabuf_heap.hpp"
#endif

#include <csignal>
#include <cstdio>
#include <rte_cycles.h>
#include <rte_ethdev.h>

static constexpr uint16_t BURST = 64;

volatile unsigned g_doWork = 0;
static void signal_handler(int /* sig */)
{
    g_doWork = 1;
}

static void rx_counter()
{
    if (rte_eth_dev_count_avail() < 1) {
        rte_exit(EXIT_FAILURE, "Need at least 1 DPDK port\n");
    }

    const unsigned MBUF_NUM    = Platform::PROCESSOR_MBUF_NUM;
    const unsigned CACHE_NUM   = Platform::MEMPOOL_CACHE_SIZE;
    const uint16_t RX_DESC_NUM = Platform::PROCESSOR_RX_DESC;
    const uint16_t TX_DESC_NUM = 64;  /* unused, smallest legal */

    DPDK::Mempool pool("rx_counter_pool", MBUF_NUM, CACHE_NUM);

#ifdef PLATFORM_ORANGEPI3B
    DPDK::UdmabufHeap udmaHeap("udmabuf0");
    const int descSocketID = udmaHeap.socket_id();
#else
    const int descSocketID = -1;
#endif

    uint16_t port_id = 0;
    DPDK::Port port = DPDK::PortBuilder(port_id)
                          .SetMemPool(pool.GetPtr())
                          .AdjustQueues(1, 1)
                          .SetDescriptors(RX_DESC_NUM, TX_DESC_NUM)
                          .SetDescriptorSocketId(descSocketID)
                          .Build();
    port.SetPromisc(true);
    port.SetAllMulticast(true);

    port.Start();
    if (!port.WaitLink()) {
        rte_exit(EXIT_FAILURE, "Link is still down after 60 sec...\n");
    }
    std::cout << port << std::endl;

    set_thread_priority(80);

    /* Stats deltas */
    rte_eth_stats prev = {}, curr = {};
    rte_eth_stats_get(port.GetID(), &prev);

    uint64_t pkts_win = 0, bytes_win = 0;
    const uint64_t ticks_per_sec = rte_get_tsc_hz();
    uint64_t next_print = rte_rdtsc() + ticks_per_sec;

    rte_mbuf* mbuf[BURST] = {};
    while (g_doWork == 0) {
        uint16_t rx = rte_eth_rx_burst(port.GetID(), 0, mbuf, BURST);
        if (rx) {
            pkts_win += rx;
            for (uint16_t i = 0; i < rx; ++i) {
                bytes_win += rte_pktmbuf_pkt_len(mbuf[i]);
            }
            rte_pktmbuf_free_bulk(mbuf, rx);
        }

        if (rte_rdtsc() >= next_print) {
            rte_eth_stats_get(port.GetID(), &curr);
            const uint64_t imissed_d  = curr.imissed   - prev.imissed;
            const uint64_t ierrors_d  = curr.ierrors   - prev.ierrors;
            const uint64_t rxnomb_d   = curr.rx_nombuf - prev.rx_nombuf;
            prev = curr;

            /*
             * Wire-rate Mb/s assumes 24 B per-frame overhead (preamble +
             * IFG + FCS) on top of L2. Keep both for clarity.
             */
            const double mbps_app  = (bytes_win * 8.0) / 1e6;
            const double mbps_wire = ((bytes_win + pkts_win * 24) * 8.0) / 1e6;

            std::printf("PPS=%-9" PRIu64 " AppMbps=%-7.1f WireMbps=%-7.1f"
                        " imissed=%-8" PRIu64 " ierrors=%-6" PRIu64
                        " nombuf=%-8" PRIu64 "\n",
                        pkts_win, mbps_app, mbps_wire,
                        imissed_d, ierrors_d, rxnomb_d);
            std::fflush(stdout);

            pkts_win = 0;
            bytes_win = 0;
            next_print += ticks_per_sec;
        }
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

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    rx_counter();
    rte_eal_cleanup();
    return 0;
}
