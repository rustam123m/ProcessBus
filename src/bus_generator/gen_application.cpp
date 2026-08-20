#include "gen_application.hpp"

#include "common/shared_defs.hpp"
#include "common/console_tables.hpp"
#include "common/platform_config.hpp"

#include "dpdk_cpp/dpdk_port_class.hpp"
#include "dpdk_cpp/dpdk_poolsetter_class.hpp"
#include "dpdk_cpp/dpdk_mempool_class.hpp"
#include "dpdk_cpp/dpdk_info_class.hpp"
#ifdef PLATFORM_ORANGEPI3B
#  include "dpdk_cpp/udmabuf_heap.hpp"
#endif

#include "goose_traffic_gen.hpp"
#include "sv_traffic_gen.hpp"
#include "r_goose_traffic_gen.hpp"
#include "r_sv_traffic_gen.hpp"

#include "cxxopts.hpp"

#include <rte_launch.h>
#include <rte_lcore.h>

#include <pthread.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

const unsigned BURST_SIZE = 32;

template< typename TxUnitArray >
static void display_tx_units_info(TxUnitArray &txUnits)
{
    size_t MaxNum = 0, MinNum = 0, TotalNum = 0;
    for (size_t i=0;i<txUnits.size();++i) {
        for (const auto &blk : txUnits[i].blocks) {
            size_t size = blk.packets.size();

            if (size > MaxNum) {
                MaxNum = size;
            }
            if (size < MinNum || MinNum == 0) {
                MinNum = size;
            }
            TotalNum += size;
        }
    }

    std::cout << "TxUnits: \n"
              << "\tTotal blocks:  " << txUnits.size() << "\n"
              << "\tTotal packets: " << TotalNum << "\n"
              << "\tMax in block:  " << MaxNum << "\n"
              << "\tMin in block:  " << MinNum
              << std::endl;
}

struct TxCycleConfig
{
    rte_mempool* pool = nullptr;
    uint16_t     nicPortID = 0;
    uint16_t     nicQueueID = 0;
};

template<
    typename GenClass,
    size_t (GenClass::*Amend)(uint8_t *packet, const typename GenClass::Desc &desc)
        = &GenClass::AmendPacket
>
static void tx_packets_cycle(TxCycleConfig &conf, GenAppStat &stat, GenClass &gen, StopVarType &doWork)
{
    typename GenClass::TxUnitArray &txUnits = gen.GetTxUnits();
    if (txUnits.empty()) {
        throw std::runtime_error("TX unit array is empty! Nothing to send!");
    }
    /* display_tx_units_info< typename GenClass::TxUnitArray >(txUnits); */

    std::cout << "\n\tStart main loop\n";
    set_thread_priority(DEF_GENERATOR_PRIORITY);

    // Main cycle
    rte_mbuf* mbufs[BURST_SIZE] = { nullptr };
    unsigned txUnitIdx = 0;

    stat.procStat.MarkStartCycling();
    uint64_t secStartTick = DPDK::Clocks::get_current_ticks();
    while (doWork) {
        stat.procStat.MarkProcBegin();
        for (const auto &blk : txUnits[txUnitIdx].blocks) {
            unsigned count = blk.packets.size(), sendNum = 0;
            while (sendNum < count) {
                unsigned num = ((count - sendNum) >= BURST_SIZE) ? BURST_SIZE
                                                                 : (count - sendNum);
                if (rte_pktmbuf_alloc_bulk(conf.pool, mbufs, num) == 0) {
                    for (size_t i=0;i<num;++i) {
                        uint8_t *packet = rte_pktmbuf_mtod(mbufs[i], uint8_t *);

                        mbufs[i]->pkt_len = (gen.*Amend)(packet, blk.packets[sendNum + i]);
                        mbufs[i]->data_len = mbufs[i]->pkt_len;
                    }

                    uint16_t nb_tx = rte_eth_tx_burst(conf.nicPortID, conf.nicQueueID, mbufs, num);
                    stat.txPktCnt += nb_tx;
                    if (nb_tx < num) {
                        for (uint16_t i=nb_tx;i<num;i++) {
                            rte_pktmbuf_free(mbufs[i]);
                        }
                        stat.errSendCnt += num - nb_tx;
                    }
                } else {
                    rte_eth_tx_done_cleanup(conf.nicPortID, conf.nicQueueID, 0);
                    if (!doWork) break;
                    continue;
                }

                sendNum += num;
            }
        }
        stat.procStat.MarkProcEnd();

        txUnitIdx = (txUnitIdx + 1) % txUnits.size();
        if (txUnitIdx == 0) {
            // New PPS(new second pulse)
            secStartTick = DPDK::Clocks::delay_until_ticks(
                               secStartTick + DPDK::Clocks::get_ticks_per_sec()
                           );
        } else {
            // Wait until the timestamp of sending next Unit
            DPDK::Clocks::delay_until_ticks(
                secStartTick + DPDK::Clocks::delay_us_to_ticks(txUnits[txUnitIdx].offsetUS)
            );
        }
    }
    stat.procStat.MarkFinishCycling();

    DPDK::Info::display_eth_stats(conf.nicPortID);
}


/*
 * One shared start tick for every TX worker: rte_eal_remote_launch staggers
 * thread start, so without this each queue would pace off its own clock read
 * and the per-second pulses would drift apart.
 */
struct RTxControl
{
    pthread_barrier_t barrier;
    uint64_t          startTick = 0;
};

struct RTxWorkerCtx
{
    std::function< void() > run;
};

static int r_tx_trampoline(void *arg)
{
    reinterpret_cast< RTxWorkerCtx* >(arg)->run();
    return 0;
}

// IED half-open slice [lo, hi) owned by worker @p i; remainder to the first workers.
static std::pair< unsigned, unsigned > ied_slice(unsigned num, unsigned workers, unsigned i)
{
    const unsigned per = num / workers, rem = num % workers;
    const unsigned lo = i * per + (i < rem ? i : rem);
    const unsigned hi = lo + per + (i < rem ? 1u : 0u);
    return { lo, hi };
}

template<
    typename GenClass,
    size_t (GenClass::*Amend)(uint8_t *packet, const typename GenClass::Desc &desc)
>
static void tx_r_worker_cycle(rte_mempool *pool, uint16_t portID, uint16_t queueID,
                              GenClass &gen, GenAppStat &stat,
                              RTxControl &ctrl, StopVarType &doWork)
{
    typename GenClass::TxUnitArray &txUnits = gen.GetTxUnits();
    set_thread_priority(DEF_GENERATOR_PRIORITY);

    rte_mbuf* mbufs[BURST_SIZE] = { nullptr };
    unsigned txUnitIdx = 0;

    // Synchronized start: the serial thread reads the tick, the rest take its copy.
    int rc = pthread_barrier_wait(&ctrl.barrier);
    if (rc == PTHREAD_BARRIER_SERIAL_THREAD) {
        ctrl.startTick = DPDK::Clocks::get_current_ticks();
    }
    pthread_barrier_wait(&ctrl.barrier);
    uint64_t secStartTick = ctrl.startTick;

    stat.procStat.MarkStartCycling();
    while (doWork) {
        stat.procStat.MarkProcBegin();
        for (const auto &blk : txUnits[txUnitIdx].blocks) {
            unsigned count = blk.packets.size(), sendNum = 0;
            while (sendNum < count) {
                unsigned num = ((count - sendNum) >= BURST_SIZE) ? BURST_SIZE
                                                                 : (count - sendNum);
                if (rte_pktmbuf_alloc_bulk(pool, mbufs, num) == 0) {
                    for (size_t i=0;i<num;++i) {
                        uint8_t *packet = rte_pktmbuf_mtod(mbufs[i], uint8_t *);

                        mbufs[i]->pkt_len = (gen.*Amend)(packet, blk.packets[sendNum + i]);
                        mbufs[i]->data_len = mbufs[i]->pkt_len;
                    }

                    uint16_t nb_tx = rte_eth_tx_burst(portID, queueID, mbufs, num);
                    stat.txPktCnt += nb_tx;
                    if (nb_tx < num) {
                        for (uint16_t i=nb_tx;i<num;i++) {
                            rte_pktmbuf_free(mbufs[i]);
                        }
                        stat.errSendCnt += num - nb_tx;
                    }
                } else {
                    rte_eth_tx_done_cleanup(portID, queueID, 0);
                    if (!doWork) break;
                    continue;
                }

                sendNum += num;
            }
        }
        stat.procStat.MarkProcEnd();

        txUnitIdx = (txUnitIdx + 1) % txUnits.size();
        if (txUnitIdx == 0) {
            secStartTick = DPDK::Clocks::delay_until_ticks(
                               secStartTick + DPDK::Clocks::get_ticks_per_sec()
                           );
        } else {
            DPDK::Clocks::delay_until_ticks(
                secStartTick + DPDK::Clocks::delay_us_to_ticks(txUnits[txUnitIdx].offsetUS)
            );
        }
    }
    stat.procStat.MarkFinishCycling();
}

template<
    typename GenClass,
    size_t (GenClass::*Amend)(uint8_t *packet, const typename GenClass::Desc &desc)
>
static void run_r_workers(rte_mempool *pool, uint16_t portID,
                          std::vector< std::unique_ptr< GenClass > > &gens,
                          std::vector< GenAppStat > &stats, StopVarType &doWork)
{
    const unsigned workers = gens.size();

    RTxControl ctrl;
    pthread_barrier_init(&ctrl.barrier, nullptr, workers);

    std::vector< RTxWorkerCtx > ctx(workers);

    // Workers 1..N-1 run one queue each on the DPDK worker lcores; index 0 is inline.
    unsigned idx = 1, lcore = 0;
    RTE_LCORE_FOREACH_WORKER(lcore) {
        if (idx >= workers) {
            break;
        }
        const unsigned w = idx;
        ctx[w].run = [=, &gens, &stats, &ctrl, &doWork]() {
            tx_r_worker_cycle< GenClass, Amend >(pool, portID, (uint16_t)w,
                                                 *gens[w], stats[w], ctrl, doWork);
        };
        rte_eal_remote_launch(r_tx_trampoline, &ctx[w], lcore);
        ++idx;
    }

    std::cout << "\n\tStart main loop with TX workers: " << workers << std::endl;
    tx_r_worker_cycle< GenClass, Amend >(pool, portID, 0, *gens[0], stats[0], ctrl, doWork);

    rte_eal_mp_wait_lcore();
    pthread_barrier_destroy(&ctrl.barrier);

    DPDK::Info::display_eth_stats(portID);
}

/*
 * Burst mode. The whole second leaves at the second boundary with minimum IPG,
 * then the wire is idle until the next one.
 *
 * Staging - allocation and AmendPacket - happens in that idle window, so the
 * only work left inside the burst is handing descriptors to the NIC. That is
 * what makes the burst NIC-paced: the core refills a ring the NIC drains at line
 * rate, rather than racing it while building packets.
 *
 * The unaccepted tail of a tx_burst is re-offered, never freed. Dropping it
 * would make the generator manufacture the very gap the receiver is being
 * measured for.
 */
template<
    typename GenClass,
    size_t (GenClass::*Amend)(uint8_t *packet, const typename GenClass::Desc &desc)
>
static void tx_burst_worker_cycle(rte_mempool *pool, uint16_t portID, uint16_t queueID,
                                  GenClass &gen, GenAppStat &stat,
                                  const std::vector< typename GenClass::Desc > &plan,
                                  RTxControl *ctrl, StopVarType &doWork)
{
    const size_t total = plan.size();
    if (total == 0) {
        throw std::runtime_error("Burst plan is empty! Nothing to send!");
    }

    set_thread_priority(DEF_GENERATOR_PRIORITY);
    std::vector< rte_mbuf* > staged(total, nullptr);

    uint64_t secStartTick = 0;
    if (ctrl != nullptr) {
        int rc = pthread_barrier_wait(&ctrl->barrier);
        if (rc == PTHREAD_BARRIER_SERIAL_THREAD) {
            ctrl->startTick = DPDK::Clocks::get_current_ticks();
        }
        pthread_barrier_wait(&ctrl->barrier);
        secStartTick = ctrl->startTick;
    } else {
        secStartTick = DPDK::Clocks::get_current_ticks();
    }

    const uint64_t ticksPerSec = DPDK::Clocks::get_ticks_per_sec();

    stat.procStat.MarkStartCycling();
    while (doWork) {
        size_t stagedNum = 0;
        while (stagedNum < total && doWork) {
            const unsigned num = static_cast< unsigned >(
                                     std::min< size_t >(BURST_SIZE, total - stagedNum));
            if (rte_pktmbuf_alloc_bulk(pool, staged.data() + stagedNum, num) != 0) {
                rte_eth_tx_done_cleanup(portID, queueID, 0);
                continue;
            }
            for (unsigned i=0;i<num;++i) {
                rte_mbuf *m = staged[stagedNum + i];
                m->pkt_len = (gen.*Amend)(rte_pktmbuf_mtod(m, uint8_t *),
                                          plan[stagedNum + i]);
                m->data_len = m->pkt_len;
            }
            stagedNum += num;
        }
        if (!doWork) {
            rte_pktmbuf_free_bulk(staged.data(), stagedNum);
            break;
        }

        secStartTick += ticksPerSec;
        const uint64_t now = DPDK::Clocks::get_current_ticks();
        if (now > secStartTick) {
            // Staging outran its own second: resynchronise rather than free-run,
            // so the cadence stays one burst per second and the count says so.
            ++stat.txLateCnt;
            secStartTick = now;
        }
        DPDK::Clocks::delay_until_ticks(secStartTick);

        stat.procStat.MarkProcBegin();
        size_t sent = 0;
        while (sent < stagedNum && doWork) {
            const uint16_t num = static_cast< uint16_t >(
                                     std::min< size_t >(BURST_SIZE, stagedNum - sent));
            const uint16_t nb = rte_eth_tx_burst(portID, queueID, staged.data() + sent, num);
            if (nb == 0) {
                ++stat.txRetryCnt;
            }
            sent += nb;
        }
        stat.procStat.MarkProcEnd();
        stat.txPktCnt += sent;

        /*
         * Only reachable when the run is stopping mid-burst: the fire loop
         * retries until the ring takes everything, so there is no failure path
         * here. Freeing the remainder is shutdown, not a send error - counting
         * it would break err_send == 0 as the "the generator was fine" gate.
         */
        if (sent < stagedNum) {
            rte_pktmbuf_free_bulk(staged.data() + sent, stagedNum - sent);
        }
    }
    stat.procStat.MarkFinishCycling();

    if (ctrl == nullptr) {
        DPDK::Info::display_eth_stats(portID);
    }
}

template<
    typename GenClass,
    size_t (GenClass::*Amend)(uint8_t *packet, const typename GenClass::Desc &desc)
>
static void run_r_burst_workers(rte_mempool *pool, uint16_t portID,
                                std::vector< std::unique_ptr< GenClass > > &gens,
                                std::vector< GenAppStat > &stats,
                                const std::vector< std::vector< typename GenClass::Desc > > &plans,
                                StopVarType &doWork)
{
    const unsigned workers = gens.size();

    RTxControl ctrl;
    pthread_barrier_init(&ctrl.barrier, nullptr, workers);

    std::vector< RTxWorkerCtx > ctx(workers);

    unsigned idx = 1, lcore = 0;
    RTE_LCORE_FOREACH_WORKER(lcore) {
        if (idx >= workers) {
            break;
        }
        const unsigned w = idx;
        ctx[w].run = [=, &gens, &stats, &plans, &ctrl, &doWork]() {
            tx_burst_worker_cycle< GenClass, Amend >(pool, portID, (uint16_t)w,
                                                     *gens[w], stats[w], plans[w],
                                                     &ctrl, doWork);
        };
        rte_eal_remote_launch(r_tx_trampoline, &ctx[w], lcore);
        ++idx;
    }

    std::cout << "\n\tStart burst loop with TX workers: " << workers << std::endl;
    tx_burst_worker_cycle< GenClass, Amend >(pool, portID, 0, *gens[0], stats[0],
                                             plans[0], &ctrl, doWork);

    rte_eal_mp_wait_lcore();
    pthread_barrier_destroy(&ctrl.barrier);

    DPDK::Info::display_eth_stats(portID);
}

template<
    typename GenClass,
    size_t (GenClass::*AmendNone)(uint8_t *, const typename GenClass::Desc &),
    size_t (GenClass::*AmendHmac)(uint8_t *, const typename GenClass::Desc &),
    size_t (GenClass::*AmendGcm)(uint8_t *, const typename GenClass::Desc &)
>
static void tx_r_burst_cycle_mt(rte_mempool *pool, uint16_t portID,
                                std::vector< std::unique_ptr< GenClass > > &gens,
                                std::vector< GenAppStat > &stats,
                                const std::vector< std::vector< typename GenClass::Desc > > &plans,
                                RSess::SecurityMode mode, StopVarType &doWork)
{
    switch (mode) {
    case RSess::SEC_NONE:
        run_r_burst_workers< GenClass, AmendNone >(pool, portID, gens, stats, plans, doWork);
        break;
    case RSess::SEC_HMAC:
        run_r_burst_workers< GenClass, AmendHmac >(pool, portID, gens, stats, plans, doWork);
        break;
    case RSess::SEC_GCM:
        run_r_burst_workers< GenClass, AmendGcm >(pool, portID, gens, stats, plans, doWork);
        break;
    }
}

/*
 * One plan per worker over that worker's own IED slice. Indices are
 * worker-local - the generator folds its baseIdx back in when amending - and the
 * repetition count is the generator's own schedule length, so the cadence is
 * read from the same place the steady-state path reads it.
 */
template< typename GenClass >
static std::vector< std::vector< typename GenClass::Desc > >
make_r_burst_plans(std::vector< std::unique_ptr< GenClass > > &gens, unsigned num)
{
    const unsigned workers = gens.size();
    std::vector< std::vector< typename GenClass::Desc > > plans;
    plans.reserve(workers);
    for (unsigned i=0;i<workers;++i) {
        auto [lo, hi] = ied_slice(num, workers, i);
        plans.push_back(make_burst_plan< typename GenClass::Desc >(
                            hi - lo, gens[i]->GetTxUnits().size()));
    }
    return plans;
}

template<
    typename GenClass,
    size_t (GenClass::*AmendNone)(uint8_t *, const typename GenClass::Desc &),
    size_t (GenClass::*AmendHmac)(uint8_t *, const typename GenClass::Desc &),
    size_t (GenClass::*AmendGcm)(uint8_t *, const typename GenClass::Desc &)
>
static void tx_r_packets_cycle_mt(rte_mempool *pool, uint16_t portID,
                                  std::vector< std::unique_ptr< GenClass > > &gens,
                                  std::vector< GenAppStat > &stats,
                                  RSess::SecurityMode mode, StopVarType &doWork)
{
    switch (mode) {
    case RSess::SEC_NONE:
        run_r_workers< GenClass, AmendNone >(pool, portID, gens, stats, doWork);
        break;
    case RSess::SEC_HMAC:
        run_r_workers< GenClass, AmendHmac >(pool, portID, gens, stats, doWork);
        break;
    case RSess::SEC_GCM:
        run_r_workers< GenClass, AmendGcm >(pool, portID, gens, stats, doWork);
        break;
    }
}

/*
 * One generator per worker, each built over just its IED slice: own crypto and
 * plaintext scratch (not thread-safe to share) and a slice-sized schedule, so
 * there is nothing to discard. The factory takes (baseIdx, count); the generator
 * folds baseIdx back into appid/sID so stream identity stays global.
 */
template< typename GenClass, typename Factory >
static std::vector< std::unique_ptr< GenClass > >
make_r_workers(unsigned workers, unsigned num, Factory factory)
{
    std::vector< std::unique_ptr< GenClass > > gens;
    gens.reserve(workers);
    for (unsigned i=0;i<workers;++i) {
        auto [lo, hi] = ied_slice(num, workers, i);
        gens.push_back(factory(lo, hi - lo));
    }
    return gens;
}

GenApplication::GenApplication(int argc, char *argv[])
{
    try {
        cxxopts::Options options("bus_generator", "Options: <dpdk_opts> -- <app_opts>");

        options.add_options()
            ("h,help", "Print usage")
            ("goose", "The number of unique GOOSE to generate and the frequency", cxxopts::value<std::vector<int>>())
            ("sv80", "The number of unique SV with 80 points", cxxopts::value<int>())
            ("sv256", "The number of unique SV with 256 points", cxxopts::value<int>())
            ("rgoose", "The number of unique R-GOOSE to generate and the frequency", cxxopts::value<std::vector<int>>())
            ("rsv80", "The number of unique R-SV with 80 points", cxxopts::value<int>())
            ("rsv256", "The number of unique R-SV with 256 points", cxxopts::value<int>())
            ("r-mode", "R-GOOSE/R-SV security: none|hmac|gcm", cxxopts::value<std::string>())
            ("dst-ip", "R-GOOSE/R-SV destination multicast group", cxxopts::value<std::string>())
            ("src-ip", "R-GOOSE/R-SV source address", cxxopts::value<std::string>())
            ("goose-entries", "Dataset entries per GOOSE/R-GOOSE (must match the processor)",
                              cxxopts::value<unsigned>())
            ("burst", "Send the whole second at the second boundary, not spread over it")
            ("time", "Stop after N seconds, 0 to run until interrupted", cxxopts::value<unsigned>());

        auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            rte_exit(0, "");
        }

        if (result.count("goose")) {
            auto gooseOpts = result["goose"].as<std::vector<int>>();
            if (gooseOpts.size() != 2) {
                throw std::invalid_argument("The goose option is invalid, must be: N,M");
            }

            m_gooseNum = gooseOpts[0];
            m_gooseSendFreq = gooseOpts[1];
        }
        if (result.count("sv80")) {
            m_sv80Num = result["sv80"].as<int>();
        }
        if (result.count("sv256")) {
            m_sv256Num = result["sv256"].as<int>();
        }

        if (result.count("rgoose")) {
            auto opts = result["rgoose"].as<std::vector<int>>();
            if (opts.size() != 2) {
                throw std::invalid_argument("The rgoose option is invalid, must be: N,M");
            }

            m_rgooseNum = opts[0];
            m_rgooseSendFreq = opts[1];
        }
        if (result.count("rsv80")) {
            m_rsv80Num = result["rsv80"].as<int>();
        }
        if (result.count("rsv256")) {
            m_rsv256Num = result["rsv256"].as<int>();
        }
        if (result.count("r-mode")) {
            m_rframe.mode = RSess::parse_security_mode(result["r-mode"].as<std::string>());
        }
        if (result.count("dst-ip")) {
            const std::string ip = result["dst-ip"].as<std::string>();
            if (!RFrame::parse_ipv4(ip.c_str(), m_rframe.dstIP)) {
                throw std::invalid_argument("Invalid --dst-ip: " + ip);
            }
        }
        if (result.count("burst")) {
            m_burst = true;
        }
        if (result.count("time")) {
            m_runTimeSec = result["time"].as<unsigned>();
        }
        if (result.count("goose-entries")) {
            m_gooseEntries = result["goose-entries"].as<unsigned>();
            if (m_gooseEntries == 0) {
                throw std::invalid_argument("--goose-entries must be >= 1");
            }
        }
        if (result.count("src-ip")) {
            const std::string ip = result["src-ip"].as<std::string>();
            if (!RFrame::parse_ipv4(ip.c_str(), m_rframe.srcIP)) {
                throw std::invalid_argument("Invalid --src-ip: " + ip);
            }
        }
    } catch (const std::exception &e) {
        std::cerr << "cxxopts: Error parsing options: " << e.what() << std::endl;
        throw;
    }

    // R-messages get one TX worker per lcore (capped at the stream count); the
    // L2 flags take priority in Run(), so an L2 request stays single-worker.
    const bool l2 = (m_gooseNum > 0 || m_sv80Num > 0 || m_sv256Num > 0);
    unsigned rNum = 0;
    if (!l2) {
        rNum = m_rgooseNum ? m_rgooseNum : (m_rsv80Num ? m_rsv80Num : m_rsv256Num);
    }
    if (rNum > 0) {
        m_workerNum = std::min(rte_lcore_count(), rNum);
    }
    m_stats.resize(m_workerNum);
}

// "Main" for the inline worker, "LCoreN" for the launched ones.
static std::string worker_label(size_t i)
{
    return (i == 0) ? "Main" : ("LCore" + std::to_string(i));
}

void GenApplication::DisplayStatistic(unsigned interval_sec)
{
    rte_eth_stats start = m_lastPortStat;
    unsigned port_id = 0;
    rte_eth_stats_get(port_id, &m_lastPortStat);
    if (m_runStarted) {
        m_statDisplaySec += interval_sec;
    }

    uint64_t tx_pps = (m_lastPortStat.opackets - start.opackets) / interval_sec;
    uint64_t tx_bps = (m_lastPortStat.obytes - start.obytes) / interval_sec;

    unsigned errSend = 0;
    for (const auto &s : m_stats) {
        errSend += s.errSendCnt;
    }

    std::cout << std::format("\nTime {} sec\n\n", m_statDisplaySec);

    std::cout << std::format(
                        "            | TX         |\n"
                        "--------------------------\n"
                        "Load(Mbps)  | {:<10.1f} |\n"
                        "PPS         | {:<10} |\n"
                        "Packets     | {:<10} |\n"
                        "Errors      | {:<10} |\n"
                        "TxRingFull  | {:<10} |\n",
                        tx_bps * 8 / 1000000.0,
                        tx_pps,
                        m_lastPortStat.opackets,
                        m_lastPortStat.oerrors,
                        errSend
                 )
              << std::endl;

    Console::CyclicStat::PrintTableHeader();
    for (size_t i=0;i<m_stats.size();++i) {
        Console::CyclicStat::PrintSampleRow(worker_label(i), m_stats[i].procStat) << "\n";
    }
}

void GenApplication::DisplayResults()
{
    unsigned errSend = 0;
    uint64_t txPkt = 0;
    size_t   busiest = 0;
    for (size_t i=0;i<m_stats.size();++i) {
        errSend += m_stats[i].errSendCnt;
        txPkt   += m_stats[i].txPktCnt;
        if (m_stats[i].procStat.GetLoadPerc() > m_stats[busiest].procStat.GetLoadPerc()) {
            busiest = i;
        }
    }

    Console::CyclicStat::PrintTableHeader({"TxRingFull"});
    for (size_t i=0;i<m_stats.size();++i) {
        Console::CyclicStat::PrintTableRow(worker_label(i), m_stats[i].procStat)
                          << std::format(" {:<10} |\n", m_stats[i].errSendCnt);
    }

    /*
     * err_send is the TX-ring-full count: a run with any is not a valid data
     * point. load/min/max report the busiest worker, i.e. the per-core ceiling.
     */
    DPDK::CyclicStat &top = m_stats[busiest].procStat;
    std::cout << std::format(
                     "\nSUMMARY_GEN\n"
                     "\tSent          \ttx_packets={}\n"
                     "\tTX ring full  \terr_send={}\n"
                     "\tBusiest loop  \tload_pct={:.3f}\tmin_us={}\tmax_us={}\n",
                     txPkt,
                     errSend,
                     top.GetLoadPerc(),
                     top.GetMinProcUS(),
                     top.GetMaxProcUS());

    /*
     * Burst-only counters, so a steady-state log keeps exactly the shape the
     * stored batches and their parsers already have. tx_retry counts bursts of
     * TX-ring-full - those packets were re-offered, not lost - and tx_late
     * counts bursts whose staging overran the second they belonged to.
     */
    if (m_burst) {
        uint64_t retries = 0, late = 0;
        for (const auto &st : m_stats) {
            retries += st.txRetryCnt;
            late    += st.txLateCnt;
        }
        std::cout << std::format("\tBurst pacing  \ttx_retry={}\ttx_late={}\n",
                                 retries, late);
    }

    std::cout << "END_SUMMARY_GEN\n" << std::endl;
}

template< typename Plans >
static size_t total_burst(const Plans &plans)
{
    size_t n = 0;
    for (const auto &p : plans) {
        n += p.size();
    }
    return n;
}

/*
 * A staged burst holds every one of its mbufs at once, which the steady-state
 * path never does - it recycles 32 at a time. So the pool, not the TX ring, is
 * the binding constraint here.
 */
void GenApplication::CheckBurstFits(size_t staged, unsigned workers, size_t frameSize,
                                    uint16_t portID) const
{
    const uint64_t need = static_cast< uint64_t >(staged)
                        + static_cast< uint64_t >(workers) * Platform::GENERATOR_TX_DESC
                        + static_cast< uint64_t >(workers) * Platform::MEMPOOL_CACHE_SIZE;
    if (need >= Platform::GENERATOR_MBUF_NUM) {
        throw std::runtime_error("Burst of " + std::to_string(staged)
            + " packets does not fit the mempool: need " + std::to_string(need)
            + " mbufs, have " + std::to_string(Platform::GENERATOR_MBUF_NUM)
            + " - use a smaller --goose/--rgoose frequency");
    }

    /*
     * A burst that takes longer than a second to put on the wire cannot be one
     * burst per second. This warns rather than refuses: link_speed is read from
     * the PHY and has been seen reporting a stale value right after link-up, and
     * a wrong refusal would cost a whole batch.
     */
    rte_eth_link link = {};
    if (rte_eth_link_get_nowait(portID, &link) == 0 && link.link_speed > 0) {
        const double bits = (double)staged * (frameSize + 20/*preamble+IFG*/) * 8;
        const double sec = bits / ((double)link.link_speed * 1000000.0);
        std::cout << std::format("\nBurst: {} packets, {:.1f} ms of wire time at {} Mbit/s\n",
                                 staged, sec * 1000.0, link.link_speed);
        if (sec > 1.0) {
            std::cout << "WARNING: the burst cannot fit in one second - the cadence "
                         "will collapse into continuous traffic (watch tx_late)\n";
        }
    }
}

void GenApplication::RunBurstCycles(rte_mempool *pool, uint16_t portID, StopVarType &doWork)
{
    if (m_gooseNum > 0) {
        GooseTrafficGen gen(m_gooseNum, m_gooseSendFreq, m_gooseEntries);

        DPDK::PoolSetter(gen.GetSkeletonBuffer(), gen.GetSkeletonSize())
                        .FillPackets(pool);

        auto plan = make_burst_plan< GooseTrafficGen::Desc >(m_gooseNum, m_gooseSendFreq);
        CheckBurstFits(plan.size(), 1, gen.GetSkeletonSize(), portID);

        std::cout << "\n\tStart burst loop\n";
        tx_burst_worker_cycle< GooseTrafficGen, &GooseTrafficGen::AmendPacket >(
            pool, portID, 0, gen, m_stats[0], plan, nullptr, doWork);
    } else if (m_sv80Num > 0 || m_sv256Num > 0) {
        const SV_TYPE type = (m_sv80Num > 0) ? SV_TYPE::SV80 : SV_TYPE::SV256;
        const unsigned num = (m_sv80Num > 0) ? m_sv80Num : m_sv256Num;
        SVTrafficGen gen(num, type);

        DPDK::PoolSetter(gen.GetSkeletonBuffer(), gen.GetSkeletonSize())
                        .FillPackets(pool);

        // SV has no frequency flag: its cadence belongs to the profile, so the
        // repetition count comes from the schedule the generator built itself.
        auto plan = make_burst_plan< SVTrafficGen::Desc >(num, gen.GetTxUnits().size());
        CheckBurstFits(plan.size(), 1, gen.GetSkeletonSize(), portID);

        std::cout << "\n\tStart burst loop\n";
        if (type == SV_TYPE::SV80) {
            tx_burst_worker_cycle< SVTrafficGen, &SVTrafficGen::AmendPacketSV80 >(
                pool, portID, 0, gen, m_stats[0], plan, nullptr, doWork);
        } else {
            tx_burst_worker_cycle< SVTrafficGen, &SVTrafficGen::AmendPacketSV256 >(
                pool, portID, 0, gen, m_stats[0], plan, nullptr, doWork);
        }
    } else if (m_rgooseNum > 0) {
        auto gens = make_r_workers< RGooseTrafficGen >(m_workerNum, m_rgooseNum,
            [&](unsigned lo, unsigned count){ return std::make_unique< RGooseTrafficGen >(
                     count, m_rgooseSendFreq, m_gooseEntries, m_rframe, lo); });

        DPDK::PoolSetter(gens[0]->GetSkeletonBuffer(), gens[0]->GetSkeletonSize())
                        .FillPackets(pool);

        auto plans = make_r_burst_plans< RGooseTrafficGen >(gens, m_rgooseNum);
        CheckBurstFits(total_burst(plans), m_workerNum, gens[0]->GetSkeletonSize(), portID);

        tx_r_burst_cycle_mt< RGooseTrafficGen,
                             &RGooseTrafficGen::AmendPacket< RSess::SEC_NONE >,
                             &RGooseTrafficGen::AmendPacket< RSess::SEC_HMAC >,
                             &RGooseTrafficGen::AmendPacket< RSess::SEC_GCM > >(
            pool, portID, gens, m_stats, plans, m_rframe.mode, doWork);
    } else if (m_rsv80Num > 0 || m_rsv256Num > 0) {
        const SV_TYPE type = (m_rsv80Num > 0) ? SV_TYPE::SV80 : SV_TYPE::SV256;
        const unsigned num = (m_rsv80Num > 0) ? m_rsv80Num : m_rsv256Num;

        auto gens = make_r_workers< RSVTrafficGen >(m_workerNum, num,
            [&](unsigned lo, unsigned count){ return std::make_unique< RSVTrafficGen >(
                     count, type, m_rframe, lo); });

        DPDK::PoolSetter(gens[0]->GetSkeletonBuffer(), gens[0]->GetSkeletonSize())
                        .FillPackets(pool);

        auto plans = make_r_burst_plans< RSVTrafficGen >(gens, num);
        CheckBurstFits(total_burst(plans), m_workerNum, gens[0]->GetSkeletonSize(), portID);

        if (type == SV_TYPE::SV80) {
            tx_r_burst_cycle_mt< RSVTrafficGen,
                                 &RSVTrafficGen::AmendPacketSV80< RSess::SEC_NONE >,
                                 &RSVTrafficGen::AmendPacketSV80< RSess::SEC_HMAC >,
                                 &RSVTrafficGen::AmendPacketSV80< RSess::SEC_GCM > >(
                pool, portID, gens, m_stats, plans, m_rframe.mode, doWork);
        } else {
            tx_r_burst_cycle_mt< RSVTrafficGen,
                                 &RSVTrafficGen::AmendPacketSV256< RSess::SEC_NONE >,
                                 &RSVTrafficGen::AmendPacketSV256< RSess::SEC_HMAC >,
                                 &RSVTrafficGen::AmendPacketSV256< RSess::SEC_GCM > >(
                pool, portID, gens, m_stats, plans, m_rframe.mode, doWork);
        }
    } else {
        std::cerr << "You have to specify GOOSE, SV, R-GOOSE or R-SV to generate!\n";
    }
}

void GenApplication::Run(StopVarType &doWork)
{
    // DPDK settings (platform-specific)
    const unsigned MBUF_NUM = Platform::GENERATOR_MBUF_NUM,
                   CACHE_NUM = Platform::MEMPOOL_CACHE_SIZE,
                   RX_DESC_NUM = Platform::GENERATOR_RX_DESC,
                   TX_DESC_NUM = Platform::GENERATOR_TX_DESC;

    // Memory pool for skeletons
    DPDK::Mempool pool("bus_gen_pool", MBUF_NUM, CACHE_NUM);

#ifdef PLATFORM_ORANGEPI3B
    /*
     * Pull descriptor rings out of cacheable hugepages and into a CMA
     * region mapped Normal/Non-Cacheable via u-dma-buf. RK3566's PCIe
     * is not cache-coherent, so cacheable descriptor rings get clobbered
     * at first wrap (~1024 packets). The heap survives until the
     * UdmabufHeap object is destroyed, which is after the port closes.
     */
    DPDK::UdmabufHeap udmaHeap("udmabuf0");
    const int descSocketID = udmaHeap.socket_id();
#else
    const int descSocketID = -1; // -1 → keep PortBuilder default (rte_socket_id)
#endif

    // R-messages fan out across m_workerNum TX queues; L2 stays single-queue.
    const bool l2 = (m_gooseNum > 0 || m_sv80Num > 0 || m_sv256Num > 0);
    const bool isR = !l2 && (m_rgooseNum > 0 || m_rsv80Num > 0 || m_rsv256Num > 0);
    const uint16_t txQueues = isR ? static_cast< uint16_t >(m_workerNum) : 1;

    if (isR) {
        // One TX ring plus a burst per worker must fit the pool (OPi3B binds first).
        const uint64_t need = static_cast< uint64_t >(txQueues) * TX_DESC_NUM
                            + static_cast< uint64_t >(txQueues) * CACHE_NUM + BURST_SIZE;
        if (need >= MBUF_NUM) {
            throw std::runtime_error("Too many TX cores (" + std::to_string(txQueues)
                + ") for the mempool: need " + std::to_string(need) + " mbufs, have "
                + std::to_string(MBUF_NUM) + " — use fewer lcores");
        }
    } else if (rte_lcore_count() > 1) {
        std::cout << "L2 GOOSE/SV generation is single-core; extra lcores stay idle.\n";
    }

    uint16_t nicPortID = 0;
    DPDK::Port port = DPDK::PortBuilder(nicPortID)
                            .SetMemPool(pool.GetPtr())
                            .AdjustQueues(1, txQueues)
                            .SetDescriptors(RX_DESC_NUM, TX_DESC_NUM)
                            .SetDescriptorSocketId(descSocketID)
                            .Build();

    // Start NIC port
    port.Start();
    if (!port.WaitLink()) {
        throw std::runtime_error("Link is still down after 60 sec...");
    }
    // Link info
    std::cout << port << "\n";

    MarkRunStarted();

    TxCycleConfig conf { .pool=pool.GetPtr(), .nicPortID=port.GetID(), .nicQueueID=0 };

    // Main cycle
    if (m_burst) {
        RunBurstCycles(pool.GetPtr(), port.GetID(), doWork);
    } else if (m_gooseNum > 0) {
        // GOOSE
        GooseTrafficGen gen(m_gooseNum, m_gooseSendFreq, m_gooseEntries);

        DPDK::PoolSetter(gen.GetSkeletonBuffer(), gen.GetSkeletonSize())
                        .FillPackets(pool.GetPtr());

        // Generate cycle with GOOSE packets
        tx_packets_cycle< GooseTrafficGen >(conf, m_stats[0], gen, doWork);
    } else if (m_sv80Num > 0) {
        // SV 80 points
        SVTrafficGen gen(m_sv80Num, SV_TYPE::SV80);

        DPDK::PoolSetter(gen.GetSkeletonBuffer(), gen.GetSkeletonSize())
                        .FillPackets(pool.GetPtr());

        // Generate cycle with SV packets
        tx_packets_cycle< SVTrafficGen, &SVTrafficGen::AmendPacketSV80 >(conf, m_stats[0], gen, doWork);
    } else if (m_sv256Num > 0) {
        // SV 256 points
        SVTrafficGen gen(m_sv256Num, SV_TYPE::SV256);

        DPDK::PoolSetter(gen.GetSkeletonBuffer(), gen.GetSkeletonSize())
                        .FillPackets(pool.GetPtr());

        // Generate cycle with SV packets
        tx_packets_cycle< SVTrafficGen, &SVTrafficGen::AmendPacketSV256 >(conf, m_stats[0], gen, doWork);
    } else if (m_rgooseNum > 0) {
        // R-GOOSE (IEC 61850-90-5)
        auto gens = make_r_workers< RGooseTrafficGen >(m_workerNum, m_rgooseNum,
            [&](unsigned lo, unsigned count){ return std::make_unique< RGooseTrafficGen >(
                     count, m_rgooseSendFreq, m_gooseEntries, m_rframe, lo); });

        DPDK::PoolSetter(gens[0]->GetSkeletonBuffer(), gens[0]->GetSkeletonSize())
                        .FillPackets(pool.GetPtr());

        tx_r_packets_cycle_mt< RGooseTrafficGen,
                               &RGooseTrafficGen::AmendPacket< RSess::SEC_NONE >,
                               &RGooseTrafficGen::AmendPacket< RSess::SEC_HMAC >,
                               &RGooseTrafficGen::AmendPacket< RSess::SEC_GCM > >(
            pool.GetPtr(), port.GetID(), gens, m_stats, m_rframe.mode, doWork);
    } else if (m_rsv80Num > 0) {
        // R-SV 80 points
        auto gens = make_r_workers< RSVTrafficGen >(m_workerNum, m_rsv80Num,
            [&](unsigned lo, unsigned count){ return std::make_unique< RSVTrafficGen >(
                     count, SV_TYPE::SV80, m_rframe, lo); });

        DPDK::PoolSetter(gens[0]->GetSkeletonBuffer(), gens[0]->GetSkeletonSize())
                        .FillPackets(pool.GetPtr());

        tx_r_packets_cycle_mt< RSVTrafficGen,
                               &RSVTrafficGen::AmendPacketSV80< RSess::SEC_NONE >,
                               &RSVTrafficGen::AmendPacketSV80< RSess::SEC_HMAC >,
                               &RSVTrafficGen::AmendPacketSV80< RSess::SEC_GCM > >(
            pool.GetPtr(), port.GetID(), gens, m_stats, m_rframe.mode, doWork);
    } else if (m_rsv256Num > 0) {
        // R-SV 256 points
        auto gens = make_r_workers< RSVTrafficGen >(m_workerNum, m_rsv256Num,
            [&](unsigned lo, unsigned count){ return std::make_unique< RSVTrafficGen >(
                     count, SV_TYPE::SV256, m_rframe, lo); });

        DPDK::PoolSetter(gens[0]->GetSkeletonBuffer(), gens[0]->GetSkeletonSize())
                        .FillPackets(pool.GetPtr());

        tx_r_packets_cycle_mt< RSVTrafficGen,
                               &RSVTrafficGen::AmendPacketSV256< RSess::SEC_NONE >,
                               &RSVTrafficGen::AmendPacketSV256< RSess::SEC_HMAC >,
                               &RSVTrafficGen::AmendPacketSV256< RSess::SEC_GCM > >(
            pool.GetPtr(), port.GetID(), gens, m_stats, m_rframe.mode, doWork);
    } else {
        std::cerr << "You have to specify GOOSE, SV, R-GOOSE or R-SV to generate!\n";
    }

    // Finish delimiter
    std::cout << std::format("\n\n{:*<80}\n{:*^80}\n{:*<80}\n\n",
                             "", " FINISH ", "");

    port.Stop();
    DisplayResults();
}

