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

    void Run(StopVarType &doWork);

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

    RFrameConfig m_rframe;

    // One TX worker per lcore for R-messages; a single entry for L2.
    unsigned      m_workerNum = 1;

    // Statistics
    std::vector< GenAppStat > m_stats;
    rte_eth_stats m_lastPortStat = {};
    unsigned      m_statDisplaySec = 0;
    unsigned      m_runTimeSec = 0;
};

