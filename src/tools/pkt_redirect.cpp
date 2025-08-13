#include "dpdk_cpp/dpdk_port_class.hpp"
#include "dpdk_cpp/dpdk_mempool_class.hpp"
#include "common/utils.hpp"

#include <csignal>

static constexpr uint16_t RX_DESC       = 4 * 1024;
static constexpr uint16_t TX_DESC       = 4 * 1024;
static constexpr unsigned NB_MBUFS      = 32 * 1024;
static constexpr unsigned MBUF_CACHE    = 32;
static constexpr uint16_t BURST         = 64;

volatile unsigned g_doWork = 0;
static void signal_handler(int sig)
{
    g_doWork = 1;
}

static void pkt_redirect(int argc, char* argv[])
{
    // TX port, RX port
    uint16_t port_tx = 0, port_rx = 1;
    if (rte_eth_dev_count_avail() < 2) {
        rte_exit(EXIT_FAILURE, "Need at least 2 DPDK ports\n");
    }

    DPDK::Mempool pool("redirect", NB_MBUFS, MBUF_CACHE);

    // RX port
    DPDK::Port portRx = DPDK::PortBuilder(port_rx)
                        .SetMemPool(pool.GetPtr())
                        .AdjustQueues(1, 1)
                        .SetDescriptors(RX_DESC, RX_DESC)
                        .Build();
    portRx.SetPromisc(true);
    // TX port
    DPDK::Port portTx = DPDK::PortBuilder(port_tx)
                        .SetMemPool(pool.GetPtr())
                        .AdjustQueues(1, 1)
                        .SetDescriptors(RX_DESC, TX_DESC)
                        .Build();
    portTx.SetPromisc(true);

    portTx.Start();
    portRx.Start();
    if ( !(portTx.WaitLink(10) && portRx.WaitLink(10)) ) {
        rte_exit(EXIT_FAILURE, "Link is down!");
    }

    // Link info
    std::cout << "RX: \n" << portRx << "\nTX: \n" << portTx << "\n";

    // RT
    set_thread_priority(80);

    std::cout << "Start main loop\n";

    rte_mbuf* mbuf[BURST] = {};
    while (g_doWork == 0) {
        uint16_t rx = rte_eth_rx_burst(portRx.GetID(), 0, mbuf, BURST);
        if (rx == 0) {
            continue;
        }

        uint16_t sent = 0;
        while (sent < rx) {
            sent += rte_eth_tx_burst(portTx.GetID(), 0, &mbuf[sent], rx - sent);
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

    std::signal(SIGINT, signal_handler);  // Ctrl+C
    std::signal(SIGTERM, signal_handler); // kill -TERM

    // App
    pkt_redirect(argc, argv);

    rte_eal_cleanup();
    return 0;
}

