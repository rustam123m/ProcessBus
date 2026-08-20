#include "r_sv_traffic_gen.hpp"

#include <cinttypes>
#include <stdexcept>

namespace
{
    inline uint16_t be16(const uint8_t* p)
    {
        return static_cast< uint16_t >((p[0] << 8) | p[1]);
    }
}

RSVTrafficGen::RSVTrafficGen(unsigned num, SV_TYPE type, const RFrameConfig &cfg,
                             unsigned baseIdx)
    : m_ieds(num), m_baseIdx(baseIdx)
{
    if (!m_crypto.IsReady()) {
        throw std::runtime_error("Failed to initialise crypto contexts for R-SV");
    }

    // Builder only — see the same note in RGooseTrafficGen's constructor.
    SVTrafficGen l2(num, type);

    for (size_t i=0;i<m_ieds.size();++i) {
        snprintf(m_ieds[i].sID, sizeof(m_ieds[i].sID), "%04u", (unsigned)(m_baseIdx + i + 1));
    }
    m_freq = l2.GetSmpCntFreq();

    MakeSkeletonPacket(l2, cfg);

    m_units = l2.GetTxUnits();
}

void RSVTrafficGen::MakeSkeletonPacket(SVTrafficGen &l2, const RFrameConfig &cfg)
{
    const uint8_t* l2Buf = l2.GetSkeletonBuffer();

    // Length = 8 + APDU (sv_publisher.c:526-531).
    const size_t l2AppidOff = l2.GetAppidOffset();
    const size_t l2ApduOff  = l2AppidOff + 8;
    const uint16_t lengthField = be16(l2Buf + l2AppidOff + 2);
    if (lengthField < 8 || l2ApduOff + (lengthField - 8u) > l2.GetSkeletonSize()) {
        throw std::runtime_error("L2 SV skeleton has an inconsistent length field");
    }
    const uint16_t apduLen = static_cast< uint16_t >(lengthField - 8);

    const uint8_t ivLen = RSess::iv_length(cfg.mode);
    const size_t  udpPayloadSize = RSess::udp_payload_size(cfg.mode, apduLen);

    m_skeletonSize = RFrame::OFF_UDP_PAYLOAD + udpPayloadSize;
    if (m_skeletonSize > MAX_SV_PACKET_SIZE) {
        throw std::runtime_error("R-SV frame exceeds "
                                 + std::to_string((int)MAX_SV_PACKET_SIZE) + " bytes");
    }

    RFrame::build_prefix(m_skeleton, cfg, udpPayloadSize);

    for (unsigned i=0;i<RFrame::R_DST_SPREAD;++i) {
        RFrame::make_dst_fields(m_skeleton, RFrame::stream_dst_ip(cfg.dstIP, i), m_dst[i]);
    }

    RSess::BuildParams params;
    params.si          = RSess::SI_R_SV;
    params.payloadType = RSess::PAYLOAD_TYPE_SV;
    params.mode        = cfg.mode;
    params.spduNumber  = 0;
    params.keyId       = RSess::KEY_ID;
    params.appid       = 0;                 // written per packet
    params.apduLength  = apduLen;

    const size_t apduOff =
        RSess::build(m_skeleton + RFrame::OFF_UDP_PAYLOAD, params);

    std::memcpy(m_skeleton + RFrame::OFF_UDP_PAYLOAD + apduOff,
                l2Buf + l2ApduOff, apduLen);

    const size_t regionOff = RFrame::OFF_UDP_PAYLOAD
                           + RSess::payload_hdr_offset(ivLen) + 4;
    const size_t regionSize = RSess::PAYLOAD_ELEM_SIZE + apduLen;

    m_regionOffset = static_cast< uint16_t >(regionOff);
    m_spduOffset   = static_cast< uint16_t >(size_t(RFrame::OFF_UDP_PAYLOAD)
                                             + RSess::OFF_SPDU_NUMBER);

    m_plain.assign(m_skeleton + regionOff, m_skeleton + regionOff + regionSize);

    // Rebase Ethernet-relative offsets onto the payload header.
    m_appidOffset = 2;      // type(1) + simulation(1)
    const uint16_t apduBase = static_cast< uint16_t >(RSess::PAYLOAD_ELEM_SIZE);
    for (int i=0;i<MAX_SV_ASDU_NUM;++i) {
        for (int f=0;f<SV_ASDU_OFFSET_NUM;++f) {
            const uint16_t off = l2.GetAsduOffset(i, f);
            m_asduOffs[i][f] = (off == 0) ? 0
                                          : static_cast< uint16_t >(apduBase + (off - l2ApduOff));
        }
    }
}
