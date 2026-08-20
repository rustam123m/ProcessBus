#pragma once

#include "common/utils.hpp"
#include "common/shared_defs.hpp"
#include "common/goose_container.hpp"
#include "common/sv_container.hpp"
#include "common/r_frame_builder.hpp"
#include "common/r_session.hpp"

#include "dpdk_cpp/dpdk_cyclestat_class.hpp"
#include "dpdk_cpp/dpdk_port_class.hpp"

using StopVarType = volatile bool;

class RX_Application;

/**
 * @brief Pipeline handler on each logical core in DPDK
 */
struct LCoreProcessor
{
    rte_ring*           m_ring = nullptr;
    RX_Application*     m_app = nullptr;
    unsigned            m_lcore = 0;
    uint64_t            m_noFreeDesc = 0;
    DPDK::CyclicStat    m_procStat;

    LCoreProcessor(rte_ring *ring, RX_Application *app, unsigned lcore)
        : m_ring(ring), m_app(app), m_lcore(lcore)
    {}
};

/**
 * @brief RX and process packets from ProcessBus
 */
class RX_Application
{
public:
    using ptr = std::shared_ptr< RX_Application >;
    RX_Application(int argc, char *argv[]);

    void DisplayStatistic(unsigned interval_sec);
    void DisplayResults();

    bool IsTimeExpired() const {
        return m_runTimeSec > 0 && m_statDisplaySec >= m_runTimeSec;
    }

    // EAL init and link wait are excluded from the --time limit.
    void MarkRunStarted() { m_runStarted = true; }

    // One lost frame increments both counters: takes the larger per stream.
    void CountGaps(uint64_t &goose, uint64_t &sv);

    void RegisterCycleStat(const std::string &label, DPDK::CyclicStat *stat) {
        m_cycleStats.emplace_back(label, stat);
    }

    void Run(StopVarType &doWork);

private:
    void ParseCmdOptions(int argc, char* argv[]);
    void Init(int argc, char* argv[]);

public:
/* private */
    // Settings
    unsigned        m_confGooseNum = 0,
                    m_confSV80Num = 0,
                    m_confSV256Num = 0,
                    m_confRGooseNum = 0,
                    m_confRSV80Num = 0,
                    m_confRSV256Num = 0;

    RSess::SecurityMode m_rMode = RSess::SEC_NONE;

    unsigned        m_gooseEntries = DEF_GOOSE_ENTRIES;
    uint32_t        m_rDstIP = R_DEFAULT_DST_IP;

    // Runtime
    GooseContainer  m_gooseMap;
    SVContainer     m_svMap;

    // Statistic
    uint64_t        m_rxGoosePktCnt = 0, m_rxSVPktCnt = 0,
                    m_errGooseParserCnt = 0, m_errSVParserCnt = 0,
                    m_rxUnknownGooseCnt = 0, m_rxUnknownSVCnt = 0,
                    m_errAuthCnt = 0,
                    m_pktToKernelCnt = 0,
                    // Frames dropped because the worker ring was full.
                    m_errRingFullCnt = 0;
    rte_eth_stats   m_lastPortStat = {};
    unsigned        m_statDisplaySec = 0;
    unsigned        m_runTimeSec = 0;

    // 0 keeps the platform default. The NIC clamps whatever is asked for, so
    // this only ever lowers the ring - see the granted counts in the banner.
    unsigned        m_rxDescNum = 0;

    // Written by the main thread, read by the statistics thread.
    volatile bool   m_runStarted = false;

    // Owned by the loop functions, alive for the whole run.
    std::vector< std::pair< std::string, DPDK::CyclicStat* > > m_cycleStats;
};

