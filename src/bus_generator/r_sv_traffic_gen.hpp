#pragma once

#include "sv_traffic_gen.hpp"

#include "common/r_frame_builder.hpp"
#include "common/r_session.hpp"
#include "common/r_session_crypto.hpp"

#include <cstring>
#include <vector>

class RSVTrafficGen
{
public:
    using Desc = SVPacketDesc;
    using TxSVUnit = TxUnit< SVPacketDesc >;
    using TxUnitArray = std::vector< TxSVUnit >;

    RSVTrafficGen(unsigned num, SV_TYPE type, const RFrameConfig &cfg);

    template< RSess::SecurityMode MODE >
    inline size_t AmendPacketSV80(uint8_t *packet, const SVPacketDesc &desc)
    {
        SVSourceIED &ied = m_ieds[desc.idx];
        uint8_t *region = PrepareFrame< MODE >(packet, desc, ied);

        *(uint32_t *)(region + m_asduOffs[0][SV_SVID_OFFSET] + 4) = *(uint32_t *)ied.sID;
        *(uint16_t *)(region + m_asduOffs[0][SV_SMP_CNT_OFFSET]) = RTE_STATIC_BSWAP16(ied.smpCnt);

        ied.smpCnt = (ied.smpCnt + 1 < m_freq) ? (ied.smpCnt + 1) : 0;

        return FinishFrame< MODE >(packet);
    }

    template< RSess::SecurityMode MODE >
    inline size_t AmendPacketSV256(uint8_t *packet, const SVPacketDesc &desc)
    {
        SVSourceIED &ied = m_ieds[desc.idx];
        uint8_t *region = PrepareFrame< MODE >(packet, desc, ied);

        for (int i=0;i<MAX_SV_ASDU_NUM;++i) {
            *(uint32_t *)(region + m_asduOffs[i][SV_SVID_OFFSET] + 4) = *(uint32_t *)ied.sID;
            *(uint16_t *)(region + m_asduOffs[i][SV_SMP_CNT_OFFSET]) = RTE_STATIC_BSWAP16(ied.smpCnt);

            ied.smpCnt = (ied.smpCnt + 1 < m_freq) ? (ied.smpCnt + 1) : 0;
        }

        return FinishFrame< MODE >(packet);
    }

    size_t      GetSkeletonSize() const { return m_skeletonSize; }
    uint8_t*    GetSkeletonBuffer() { return m_skeleton; }

    RSVTrafficGen::TxUnitArray& GetTxUnits() { return m_units; }

private:
    // Envelope fields common to both ASDU counts; returns the region to amend.
    template< RSess::SecurityMode MODE >
    inline uint8_t* PrepareFrame(uint8_t *packet, const SVPacketDesc &desc, SVSourceIED &ied)
    {
        const uint16_t appid = static_cast< uint16_t >((desc.idx + 1) & 0xFFFF);

        // APPID in the UDP source port — see RGooseTrafficGen::AmendPacket.
        *(uint16_t *)(packet + RFrame::OFF_UDP_SRC_PORT) = RTE_STATIC_BSWAP16(appid);

        // Destination spread — see RGooseTrafficGen::AmendPacket.
        const RFrame::DstFields &dst = m_dst[desc.idx & (RFrame::R_DST_SPREAD - 1)];
        std::memcpy(packet + RFrame::OFF_ETH_DMAC, dst.dmac, sizeof(dst.dmac));
        std::memcpy(packet + RFrame::OFF_IP_DST, &dst.dstIP_be, 4);
        *(uint16_t *)(packet + RFrame::OFF_IP_CSUM) = dst.ipCsum_be;

        ++ied.spduNumber;
        *(uint32_t *)(packet + m_spduOffset) = RTE_STATIC_BSWAP32(ied.spduNumber);

        uint8_t *region = nullptr;
        if constexpr (MODE == RSess::SEC_GCM) {
            region = m_plain.data();
        } else {
            region = packet + m_regionOffset;
        }

        *(uint16_t *)(region + m_appidOffset) = RTE_STATIC_BSWAP16(appid);
        return region;
    }

    template< RSess::SecurityMode MODE >
    inline size_t FinishFrame(uint8_t *packet)
    {
        if constexpr (MODE == RSess::SEC_GCM) {
            std::memcpy(packet + m_regionOffset, m_plain.data(), m_plain.size());
        }
        if constexpr (MODE != RSess::SEC_NONE) {
            RSess::seal(packet + RFrame::OFF_UDP_PAYLOAD, MODE, m_crypto);
        }
        return m_skeletonSize;
    }

    void        MakeSkeletonPacket(SVTrafficGen &l2, const RFrameConfig &cfg);

private:
    std::vector< SVSourceIED >  m_ieds;
    RSVTrafficGen::TxUnitArray  m_units;
    RSess::Crypto            m_crypto;

    // Offsets relative to the start of the payload header (the AES-GCM region).
    uint16_t    m_appidOffset = 0;
    uint16_t    m_asduOffs[MAX_SV_ASDU_NUM][SV_ASDU_OFFSET_NUM] = { 0 };

    uint16_t    m_regionOffset = 0;     // Ethernet-relative start of that region
    uint16_t    m_spduOffset = 0;       // Ethernet-relative SPDU number field

    std::vector< uint8_t >  m_plain;    // plaintext master of the encrypted region

    RFrame::DstFields m_dst[RFrame::R_DST_SPREAD] = {};

    uint8_t     m_skeleton[MAX_SV_PACKET_SIZE] = { 0 };
    size_t      m_skeletonSize = 0;
    unsigned    m_freq = 1;
};
