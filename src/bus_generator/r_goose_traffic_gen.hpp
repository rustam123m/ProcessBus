#pragma once

#include "goose_traffic_gen.hpp"

#include "common/r_frame_builder.hpp"
#include "common/r_session.hpp"
#include "common/r_session_crypto.hpp"

#include <cstring>
#include <vector>

/*
 * MODE is a template parameter, so the non-secure path emits the same field
 * writes as the L2 generator.
 */
class RGooseTrafficGen
{
public:
    using Desc = GoosePacketDesc;
    using TxGooseUnit = TxUnit< GoosePacketDesc >;
    using TxUnitArray = std::vector< TxGooseUnit >;

    RGooseTrafficGen(unsigned MaxGooseNum, unsigned SndFreq, unsigned SignalsPerGoose,
                     const RFrameConfig &cfg, unsigned baseIdx = 0);

    template< RSess::SecurityMode MODE >
    inline size_t AmendPacket(uint8_t *packet, const GoosePacketDesc &desc)
    {
        GooseSourceIED &ied = m_ieds[desc.idx];
        const unsigned stream = desc.idx + m_baseIdx;
        const uint16_t appid = static_cast< uint16_t >((stream + 1) & 0xFFFF);

        // APPID in the UDP source port: the only one visible under AES-GCM.
        *(uint16_t *)(packet + RFrame::OFF_UDP_SRC_PORT) = RTE_STATIC_BSWAP16(appid);

        // Destination group of the stream, so a hashing NIC can split them.
        const RFrame::DstFields &dst = m_dst[stream & (RFrame::R_DST_SPREAD - 1)];
        std::memcpy(packet + RFrame::OFF_ETH_DMAC, dst.dmac, sizeof(dst.dmac));
        std::memcpy(packet + RFrame::OFF_IP_DST, &dst.dstIP_be, 4);
        *(uint16_t *)(packet + RFrame::OFF_IP_CSUM) = dst.ipCsum_be;

        ++ied.spduNumber;
        *(uint32_t *)(packet + m_spduOffset) = RTE_STATIC_BSWAP32(ied.spduNumber);

        // Under GCM amend the plaintext master: a recycled mbuf holds ciphertext.
        uint8_t *region = nullptr;
        if constexpr (MODE == RSess::SEC_GCM) {
            region = m_plain.data();
        } else {
            region = packet + m_regionOffset;
        }

        *(uint16_t *)(region + m_offsets[GOOSE_APPID_OFFSET]) = RTE_STATIC_BSWAP16(appid);
        *(uint64_t *)(region + m_offsets[GOOSE_GOID_OFFSET] + 4/*GOID*/) = *(uint64_t *)ied.sID;
        *(uint64_t *)(region + m_offsets[GOOSE_GOCB_REF_OFFSET] + 3/*IED*/) = *(uint64_t *)ied.sID;
        *(uint64_t *)(region + m_offsets[GOOSE_DS_REF_OFFSET] + 3/*IED*/) = *(uint64_t *)ied.sID;

        ied.timestamp += m_tsDeltaChange;
        *(uint64_t *)(region + m_offsets[GOOSE_TIMESTAMP_OFFSET]) = ied.timestamp;

        ++ied.stNum;
        *(uint32_t *)(region + m_offsets[GOOSE_ST_NUM_OFFSET]) = RTE_STATIC_BSWAP32(ied.stNum);
        *(uint32_t *)(region + m_offsets[GOOSE_SQ_NUM_OFFSET]) = RTE_STATIC_BSWAP32(ied.sqNum);

        region[m_offsets[GOOSE_D1_OFFSET]] = (ied.stNum % 2 == 0) ? 1 : 0;

        if constexpr (MODE == RSess::SEC_GCM) {
            std::memcpy(packet + m_regionOffset, m_plain.data(), m_plain.size());
        }
        if constexpr (MODE != RSess::SEC_NONE) {
            RSess::seal(packet + RFrame::OFF_UDP_PAYLOAD, MODE, m_crypto);
        }

        return m_skeletonSize;
    }

    size_t      GetSkeletonSize() const { return m_skeletonSize; }
    uint8_t*    GetSkeletonBuffer() { return m_skeleton; }

    RGooseTrafficGen::TxUnitArray& GetTxUnits() { return m_units; }

private:
    void        MakeSkeletonPacket(GooseTrafficGen &l2, const RFrameConfig &cfg);

private:
    std::vector< GooseSourceIED >   m_ieds;
    RGooseTrafficGen::TxUnitArray   m_units;
    RSess::Crypto                m_crypto;

    unsigned    m_baseIdx = 0;          // global index of this worker's first IED
    unsigned    m_tsDeltaChange = 0;

    // Payload-header relative, indexed by GOOSE_PARAM_OFFSETS.
    uint16_t    m_offsets[GOOSE_OFFSET_NUM] = { 0 };

    uint16_t    m_regionOffset = 0;     // Ethernet-relative start of that region
    uint16_t    m_spduOffset = 0;       // Ethernet-relative SPDU number field

    std::vector< uint8_t >  m_plain;    // plaintext master of the encrypted region

    RFrame::DstFields m_dst[RFrame::R_DST_SPREAD] = {};

    uint8_t     m_skeleton[MAX_GOOSE_PACKET_SIZE] = { 0 };
    size_t      m_skeletonSize = 0;
};
