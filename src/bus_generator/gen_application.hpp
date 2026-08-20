#pragma once

#include "common/utils.hpp"
#include "common/shared_defs.hpp"
#include "common/r_frame_builder.hpp"
#include "dpdk_cpp/dpdk_cyclestat_class.hpp"
#include "dpdk_cpp/dpdk_port_class.hpp"

#include <memory>
#include <vector>

using StopVarType = volatile bool;

struct GenAppStat
{
    DPDK::CyclicStat procStat;
    unsigned         errSendCnt = 0;
    uint64_t         txPktCnt = 0;

    // Burst mode only: TX ring full during a burst, and bursts whose staging
    // overran the second they were meant to leave in.
    uint64_t         txRetryCnt = 0;
    uint64_t         txLateCnt = 0;
};

/**
 * @class GenApplication
 * @brief Main logic for bus generator app
 */
class GenApplication
{
public:
    using ptr = std::shared_ptr< GenApplication >;

    GenApplication(int argc, char *argv[]);

    void DisplayStatistic(unsigned interval_sec);
    void DisplayResults();

    bool IsTimeExpired() const {
        return m_runTimeSec > 0 && m_statDisplaySec >= m_runTimeSec;
    }

    // EAL init and link wait are excluded from the --time limit.
    void MarkRunStarted() { m_runStarted = true; }

    void Run(StopVarType &doWork);

private:
    // Burst mode lives beside the steady-state path, not inside it: nothing in
    // tx_packets_cycle/tx_r_worker_cycle or the schedules they walk is shared.
    void RunBurstCycles(rte_mempool *pool, uint16_t portID, StopVarType &doWork);

    // Refuses a burst that cannot be staged in the mempool; warns about one that
    // cannot fit in the second it has to leave in.
    void CheckBurstFits(size_t staged, unsigned workers, size_t frameSize,
                        uint16_t portID) const;

private:
    // Config
    unsigned m_gooseNum = 0,
             m_gooseSendFreq = 1,
             m_sv80Num = 0,
             m_sv256Num = 0,
             m_rgooseNum = 0,
             m_rgooseSendFreq = 1,
             m_rsv80Num = 0,
             m_rsv256Num = 0;

    unsigned m_gooseEntries = DEF_GOOSE_ENTRIES;

    // Send the whole second at the second boundary instead of spreading it.
    bool     m_burst = false;

    RFrameConfig m_rframe;

    // One TX worker per lcore for R-messages; a single entry for L2.
    unsigned      m_workerNum = 1;

    // Statistics
    std::vector< GenAppStat > m_stats;
    rte_eth_stats m_lastPortStat = {};
    unsigned      m_statDisplaySec = 0;
    unsigned      m_runTimeSec = 0;

    // Written by the main thread, read by the statistics thread.
    volatile bool m_runStarted = false;
};

