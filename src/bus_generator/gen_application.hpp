#pragma once

#include "common/utils.hpp"
#include "common/shared_defs.hpp"
#include "common/r_frame_builder.hpp"
#include "dpdk_cpp/dpdk_cyclestat_class.hpp"

using StopVarType = volatile bool;

struct GenAppStat
{
    DPDK::CyclicStat procStat;
    unsigned         errSendCnt = 0;
};

/**
 * @class GenApplication
 * @brief Main logic for bus generator app
 */
class GenApplication
{
public:
    GenApplication(int argc, char *argv[]);

    void DisplayStatistic();

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

    // Statistics
    GenAppStat m_stat;
};

