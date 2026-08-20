#pragma once

#include "pipeline.hpp"
#include "process_bus_parser.hpp"

#include <rte_mbuf.h>
#include <rte_prefetch.h>

constexpr unsigned  RX_BURST_SIZE = 32;
constexpr unsigned  PREFETCH_DIST = 4;

namespace PBus
{
    // Prefetch 3 × 64B cache lines = 192B — covers SV80 and typical GOOSE PDUs.
    static inline void prefetch_payload(rte_mbuf *m)
    {
        const char *p = rte_pktmbuf_mtod(m, const char *);
        rte_prefetch0(p);
        rte_prefetch0(p + 64);
        rte_prefetch0(p + 128);
    }

    /*
     * Frame processing:
     * {mbuf} -> RouterStage -> GooseStage -> SampledValuesStage
     * -> RGooseStage -> RSampledValuesStage -> IPStage
     */

    enum EnumStages
    {
        START_STAGE = 0,

        ROUTER   = 0,
        GOOSE    = 1,
        SV       = 2,
        R_GOOSE  = 3,
        R_SV     = 4,
        IP       = 5,

        STAGE_NUM
    };

    template< typename TMatrix, unsigned TFrameIdx >
    struct RouterStage
    {
        static void ApplyTo(TMatrix& matrix) {
            typename TMatrix::Frame &frame = matrix.stages[TFrameIdx];

            for (unsigned i=0;i<frame.num && i<PREFETCH_DIST;++i) {
                prefetch_payload(frame.buf[i]);
            }
            for (unsigned i=0;i<frame.num;++i) {
                if (i + PREFETCH_DIST < frame.num) {
                    prefetch_payload(frame.buf[i+PREFETCH_DIST]);
                }
                const uint8_t *packet = rte_pktmbuf_mtod(frame.buf[i], const uint8_t *);
                const unsigned size = rte_pktmbuf_pkt_len(frame.buf[i]);

                unsigned appid = 0;
                BUS_PROTO type = ProcessBusParser::get_proto_type(packet, &appid, size);
                switch (type) {
                case BUS_PROTO_SV: {
                    matrix.stages[SV].PutBuffer(frame.buf[i]);
                    break;
                }
                case BUS_PROTO_GOOSE: {
                    matrix.stages[GOOSE].PutBuffer(frame.buf[i]);
                    break;
                }
                case BUS_PROTO_R_GOOSE: {
                    matrix.stages[R_GOOSE].PutBuffer(frame.buf[i]);
                    break;
                }
                case BUS_PROTO_R_SV: {
                    matrix.stages[R_SV].PutBuffer(frame.buf[i]);
                    break;
                }
                default: {
                    matrix.stages[IP].PutBuffer(frame.buf[i]);
                    break;
                }
                }
            }

            // Clean frame for the next cycle
            frame.num = 0;
        }
    };

    template< typename TMatrix, unsigned TFrameIdx >
    struct GooseStage
    {
        static void ApplyTo(TMatrix &matrix) {
            typename TMatrix::Frame &frame = matrix.stages[TFrameIdx];

            RX_Application &app = *matrix.app;
            for (unsigned i=0;i<frame.num && i<PREFETCH_DIST;++i) {
                prefetch_payload(frame.buf[i]);
            }
            for (unsigned i=0;i<frame.num;++i) {
                if (i + PREFETCH_DIST < frame.num) {
                    prefetch_payload(frame.buf[i+PREFETCH_DIST]);
                }
                const uint8_t *packet = rte_pktmbuf_mtod(frame.buf[i], const uint8_t *);
                const unsigned size = rte_pktmbuf_pkt_len(frame.buf[i]);

                GoosePassport pass;
                GooseState state;
                int retval = ProcessBusParser::parse_goose_packet(packet, size, pass, state);
                if (retval == 0) {
                    auto src = app.m_gooseMap.find(pass);
                    if (src != app.m_gooseMap.end()) {
                        src->second->ProcessState(pass, state);

                        ++app.m_rxGoosePktCnt;
                    } else {
                        ++app.m_rxUnknownGooseCnt;
                    }
                } else {
                    // Invalid GOOSE packet
                    ++app.m_errGooseParserCnt;
                }
            }

            // Clean frame for the next cycle
            frame.num = 0;
        }
    };

    template< typename TMatrix, unsigned TFrameIdx >
    struct SampledValuesStage
    {
        static void ApplyTo(TMatrix &matrix) {
            typename TMatrix::Frame &frame = matrix.stages[TFrameIdx];

            RX_Application &app = *matrix.app;
            for (unsigned i=0;i<frame.num && i<PREFETCH_DIST;++i) {
                prefetch_payload(frame.buf[i]);
            }
            for (unsigned i=0;i<frame.num;++i) {
                if (i + PREFETCH_DIST < frame.num) {
                    prefetch_payload(frame.buf[i+PREFETCH_DIST]);
                }
                const uint8_t *packet = rte_pktmbuf_mtod(frame.buf[i], const uint8_t *);
                const unsigned size = rte_pktmbuf_pkt_len(frame.buf[i]);

                SVStreamPassport pass;
                SVStreamState state;
                int retval = ProcessBusParser::parse_sv_packet(packet, size, pass, state);
                if (retval == 0) {
                    auto src = app.m_svMap.find(pass);
                    if (src != app.m_svMap.end()) {
                        src->second->ProcessState(pass, state);

                        ++app.m_rxSVPktCnt;
                    } else {
                        ++app.m_rxUnknownSVCnt;
                    }
                } else {
                    ++app.m_errSVParserCnt;
                }
            }

            // Clean frame for the next cycle
            frame.num = 0;
        }
    };

    template< typename TMatrix, unsigned TFrameIdx >
    struct RGooseStage
    {
        static void ApplyTo(TMatrix &matrix) {
            typename TMatrix::Frame &frame = matrix.stages[TFrameIdx];

            RX_Application &app = *matrix.app;
            for (unsigned i=0;i<frame.num && i<PREFETCH_DIST;++i) {
                prefetch_payload(frame.buf[i]);
            }
            for (unsigned i=0;i<frame.num;++i) {
                if (i + PREFETCH_DIST < frame.num) {
                    prefetch_payload(frame.buf[i+PREFETCH_DIST]);
                }
                // Mutable: AES-GCM decrypts the payload in place.
                uint8_t *packet = rte_pktmbuf_mtod(frame.buf[i], uint8_t *);
                const unsigned size = rte_pktmbuf_pkt_len(frame.buf[i]);

                GoosePassport pass;
                GooseState state;
                RSess::SessionHeader session;
                int retval = ProcessBusParser::parse_r_goose_packet(
                                 packet, size, app.m_rMode, matrix.crypto,
                                 session, pass, state);
                if (retval == 0) {
                    auto src = app.m_gooseMap.find(pass);
                    if (src != app.m_gooseMap.end()) {
                        src->second->ProcessSessionState(session.spduNumber);
                        src->second->ProcessState(pass, state);

                        ++app.m_rxGoosePktCnt;
                    } else {
                        ++app.m_rxUnknownGooseCnt;
                    }
                } else if (retval == R_PARSE_ERR_AUTH) {
                    // A forged frame is expected on a bus, not an error.
                    ++app.m_errAuthCnt;
                } else {
                    ++app.m_errGooseParserCnt;
                }
            }

            // Clean frame for the next cycle
            frame.num = 0;
        }
    };

    template< typename TMatrix, unsigned TFrameIdx >
    struct RSampledValuesStage
    {
        static void ApplyTo(TMatrix &matrix) {
            typename TMatrix::Frame &frame = matrix.stages[TFrameIdx];

            RX_Application &app = *matrix.app;
            for (unsigned i=0;i<frame.num && i<PREFETCH_DIST;++i) {
                prefetch_payload(frame.buf[i]);
            }
            for (unsigned i=0;i<frame.num;++i) {
                if (i + PREFETCH_DIST < frame.num) {
                    prefetch_payload(frame.buf[i+PREFETCH_DIST]);
                }
                uint8_t *packet = rte_pktmbuf_mtod(frame.buf[i], uint8_t *);
                const unsigned size = rte_pktmbuf_pkt_len(frame.buf[i]);

                SVStreamPassport pass;
                SVStreamState state;
                RSess::SessionHeader session;
                int retval = ProcessBusParser::parse_r_sv_packet(
                                 packet, size, app.m_rMode, matrix.crypto,
                                 session, pass, state);
                if (retval == 0) {
                    auto src = app.m_svMap.find(pass);
                    if (src != app.m_svMap.end()) {
                        src->second->ProcessSessionState(session.spduNumber);
                        src->second->ProcessState(pass, state);

                        ++app.m_rxSVPktCnt;
                    } else {
                        ++app.m_rxUnknownSVCnt;
                    }
                } else if (retval == R_PARSE_ERR_AUTH) {
                    ++app.m_errAuthCnt;
                } else {
                    ++app.m_errSVParserCnt;
                }
            }

            // Clean frame for the next cycle
            frame.num = 0;
        }
    };

    template< typename TMatrix, unsigned TFrameIdx >
    struct IPStage
    {
        static void ApplyTo(TMatrix &matrix) {
            typename TMatrix::Frame &frame = matrix.stages[TFrameIdx];

            matrix.app->m_pktToKernelCnt += frame.num;

            // Clean frame for the next cycle
            frame.num = 0;
        }
    };

    /*
     * DataMatrix represents a pipeline's table:
     * { mbuf } x { stages } where each stage has its own frame which is an array of mbufs
     *
     * Owns the lcore's crypto context: backend contexts are stateful and not
     * thread-safe, so one per worker.
     */
    struct DataMatrix : Pipeline::Matrix< EnumStages,
                                          Pipeline::Frame< rte_mbuf, RX_BURST_SIZE >,
                                          RX_Application >
    {
        using Base = Pipeline::Matrix< EnumStages,
                                       Pipeline::Frame< rte_mbuf, RX_BURST_SIZE >,
                                       RX_Application >;
        explicit DataMatrix(RX_Application *ptr) : Base(ptr) {}

        RSess::Crypto crypto;
    };

    using FramePipeline = Pipeline::StaticChain< RouterStage< DataMatrix, ROUTER >,
                                                 GooseStage< DataMatrix, GOOSE >,
                                                 SampledValuesStage< DataMatrix, SV >,
                                                 RGooseStage< DataMatrix, R_GOOSE >,
                                                 RSampledValuesStage< DataMatrix, R_SV >,
                                                 IPStage< DataMatrix, IP > >;
}
