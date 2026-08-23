#include "process_bus_parser.hpp"

#include "common/sv_profile.hpp"

#include <rte_mbuf.h>

#include <cstring>
#include <iostream>

namespace
{
    inline int decode_asn1_len(const uint8_t* buffer, uint32_t *pos)
    {
        uint32_t p = *pos;
        uint8_t lenByte = buffer[p++];
        int length;

        if (!(lenByte & 0x80)) {
            length = lenByte;
        } else {
            switch (lenByte & 0x7F) {
            case 1:
                length = buffer[p];
                p += 1;
                break;
            case 2:
                length = (buffer[p] << 8) | buffer[p + 1];
                p += 2;
                break;
            default:
                length = 0;
                for (int n = lenByte & 0x7F; n > 0; --n) {
                    length = (length << 8) | buffer[p++];
                }
                break;
            }
        }
        *pos = p;
        return length;
    }

    inline uint32_t decode_asn1_number(const uint8_t* buffer, uint32_t size)
    {
        switch (size) {
        case 1:
            return buffer[0];
        case 2:
            return (static_cast<uint32_t>(buffer[0]) << 8)
                    | buffer[1];
        case 3:
            return (static_cast<uint32_t>(buffer[0]) << 16)
                    | (static_cast<uint32_t>(buffer[1]) << 8)
                    | buffer[2];
        case 4:
            return (static_cast<uint32_t>(buffer[0]) << 24)
                    | (static_cast<uint32_t>(buffer[1]) << 16)
                    | (static_cast<uint32_t>(buffer[2]) << 8)
                    | buffer[3];
        }
        return 0;
    }

    // Returns number of entries on this level, -1 if malformed
    int parse_alldata(const uint8_t* buffer, uint32_t pos, uint32_t end, int depth)
    {
        // Nesting is bounded so a crafted frame can't run the stack out.
        constexpr int MAX_DATA_DEPTH = 8;

        if (depth > MAX_DATA_DEPTH) {
            return -1;
        }

        int num = 0;
        while (pos < end) {
            uint8_t tag = buffer[pos++];
            if (pos >= end) {
                return -1;
            }

            int itemSize = decode_asn1_len(buffer, &pos);
            if (itemSize < 0 || pos + (uint32_t)itemSize > end) {
                return -1;
            }

            // 0xA1 array, 0xA2 structure
            if ((tag == 0xA1 || tag == 0xA2)
                && parse_alldata(buffer, pos, pos + itemSize, depth + 1) < 0) {
                return -1;
            }

            pos += itemSize;
            ++num;
        }
        return num;
    }

    inline std::string_view make_stringview(const uint8_t* data, int length)
    {
        return {
            reinterpret_cast< const char* >(data),
            static_cast< size_t >(length)
        };
    }

    /*
     * The old SV walker used the unbounded decoder above and ignored container
     * ends. Keep the hot GOOSE parser unchanged, but make every SV length read
     * fail closed before it can step outside its enclosing TLV.
     */
    bool decode_asn1_len_bounded(const uint8_t* buffer, uint32_t end,
                                 uint32_t &pos, uint32_t &length)
    {
        if (pos >= end) {
            return false;
        }

        const uint8_t first = buffer[pos++];
        if ((first & 0x80) == 0) {
            length = first;
            return true;
        }

        const uint8_t octets = first & 0x7f;
        if (octets == 0 || octets > sizeof(length) || octets > end - pos) {
            return false;               // indefinite or out-of-bounds length
        }

        length = 0;
        for (uint8_t i=0;i<octets;++i) {
            length = (length << 8) | buffer[pos++];
        }
        return true;
    }

    bool take_tlv(const uint8_t* buffer, uint32_t end, uint32_t &pos,
                  uint8_t expectedTag, uint32_t &valueEnd)
    {
        if (pos >= end || buffer[pos++] != expectedTag) {
            return false;
        }

        uint32_t length = 0;
        if (!decode_asn1_len_bounded(buffer, end, pos, length)
            || length > end - pos) {
            return false;
        }
        valueEnd = pos + length;
        return true;
    }

    /*
     * Common L2/R-SV APDU walker. One frame has one 0x60 PDU containing
     * noASDU 0x30 structures. The first ASDU supplies the stream passport and
     * first smpCnt; every ASDU is still fully walked and validated.
     */
    int parse_sv_apdu(const uint8_t* apdu, uint32_t apduSize,
                      SVStreamPassport &passport, SVStreamState &state)
    {
        uint32_t pos = 0, pduEnd = 0;
        if (!take_tlv(apdu, apduSize, pos, 0x60, pduEnd)) {
            return -3;
        }

        if (pos >= pduEnd || apdu[pos++] != 0x80) {       // noASDU
            return -4;
        }
        uint32_t noAsduLength = 0;
        if (!decode_asn1_len_bounded(apdu, pduEnd, pos, noAsduLength)
            || noAsduLength == 0 || noAsduLength > 2
            || noAsduLength > pduEnd - pos) {
            return -4;
        }
        const uint32_t declaredAsdus = decode_asn1_number(apdu + pos, noAsduLength);
        if (declaredAsdus == 0 || declaredAsdus > UINT16_MAX) {
            return -4;
        }
        passport.num = static_cast< uint16_t >(declaredAsdus);
        pos += noAsduLength;

        uint32_t sequenceEnd = 0;
        if (!take_tlv(apdu, pduEnd, pos, 0xa2, sequenceEnd)
            || sequenceEnd != pduEnd) {
            return -4;
        }

        uint32_t actualAsdus = 0;
        while (pos < sequenceEnd) {
            uint32_t asduEnd = 0;
            if (!take_tlv(apdu, sequenceEnd, pos, 0x30, asduEnd)) {
                return -5;
            }

            bool foundSvid = false, foundSmpCnt = false;
            bool foundConfRev = false, foundSmpSynch = false, foundData = false;
            std::string_view svid;
            uint16_t smpCnt = 0;
            uint32_t confRev = 0;
            uint8_t previousTag = 0;

            while (pos < asduEnd) {
                const uint8_t tag = apdu[pos++];
                if (tag <= previousTag) {
                    return -6;               // duplicate or out-of-order field
                }
                previousTag = tag;

                uint32_t length = 0;
                if (!decode_asn1_len_bounded(apdu, asduEnd, pos, length)
                    || length > asduEnd - pos) {
                    return -6;
                }

                switch (tag) {
                case 0x80: // svID
                    if (foundSvid || length == 0) {
                        return -6;
                    }
                    svid = make_stringview(apdu + pos, static_cast< int >(length));
                    foundSvid = true;
                    break;
                case 0x81: // datset (optional)
                    if (length == 0) {
                        return -6;
                    }
                    break;
                case 0x82: // smpCnt
                    if (foundSmpCnt || length != 2) {
                        return -6;
                    }
                    smpCnt = static_cast< uint16_t >(decode_asn1_number(apdu + pos, length));
                    foundSmpCnt = true;
                    break;
                case 0x83: // confRev
                    if (foundConfRev || length != 4) {
                        return -6;
                    }
                    confRev = decode_asn1_number(apdu + pos, length);
                    foundConfRev = true;
                    break;
                case 0x84: // refrTm
                    if (length != 8) {
                        return -6;
                    }
                    break;
                case 0x85: // smpSynch
                    if (foundSmpSynch || length != 1) {
                        return -6;
                    }
                    foundSmpSynch = true;
                    break;
                case 0x86: // smpRate
                    if (length != 2) {
                        return -6;
                    }
                    break;
                case 0x87: // sequence of INT32 value / 32-bit Quality pairs
                    if (foundData || length != SVProfile::ASDU_DATA_SIZE) {
                        return -6;
                    }
                    for (uint32_t off=SVProfile::VALUE_SIZE;off<length;
                         off+=SVProfile::CHANNEL_SIZE) {
                        // Byte-wise: the data field is not aligned in the frame.
                        const uint32_t quality = decode_asn1_number(
                                apdu + pos + off, SVProfile::QUALITY_SIZE);
                        if (quality != SVProfile::QUALITY_GOOD) {
                            return SV_PARSE_ERR_QUALITY;
                        }
                    }
                    foundData = true;
                    break;
                case 0x88: // smpMod
                    if (length != 2) {
                        return -6;
                    }
                    break;
                default:
                    return -6;
                }
                pos += length;
            }

            if (!foundSvid || !foundSmpCnt || !foundConfRev
                || !foundSmpSynch || !foundData) {
                return -6;
            }
            if (actualAsdus == 0) {
                passport.svid = svid;
                passport.crev = confRev;
                state.smpCnt = smpCnt;
            } else if (svid != passport.svid || confRev != passport.crev) {
                // One frame is dispatched as one stream, so all ASDUs must
                // describe that same stream configuration.
                return -6;
            }
            ++actualAsdus;
        }

        return (actualAsdus == declaredAsdus) ? 0 : -7;
    }
}

#define NET_TO_CPU_U16(x)       RTE_STATIC_BSWAP16(*(uint16_t *)(x))
#define NET_TO_CPU_U32(x)       RTE_STATIC_BSWAP32(*(uint32_t *)(x))
#define NET_TO_CPU_U64(x)       RTE_STATIC_BSWAP64(*(uint64_t *)(x))

namespace
{
    // Big-endian reads over possibly unaligned envelope fields.
    inline uint16_t rd_u16(const uint8_t* p)
    {
        return static_cast< uint16_t >((static_cast< uint16_t >(p[0]) << 8) | p[1]);
    }

    inline uint32_t rd_u32(const uint8_t* p)
    {
        return (static_cast< uint32_t >(p[0]) << 24)
             | (static_cast< uint32_t >(p[1]) << 16)
             | (static_cast< uint32_t >(p[2]) << 8)
             |  static_cast< uint32_t >(p[3]);
    }

    // A valid header sums (with end-around carry) to 0xFFFF, checksum included.
    bool ipv4_checksum_ok(const uint8_t* ipHeader)
    {
        uint32_t sum = 0;
        for (unsigned i=0;i<20;i+=2) {
            sum += rd_u16(ipHeader + i);
        }
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        return sum == 0xFFFF;
    }
}

int ProcessBusParser::validate_r_envelope(const uint8_t *buffer, unsigned size,
                                          uint64_t olFlags, uint32_t &dstIP)
{
    // Ethernet EtherType must be IPv4.
    if (size < 14 + 20 || buffer[12] != 0x08 || buffer[13] != 0x00) {
        return R_ENV_ERR_NOT_IPV4;
    }

    const uint8_t* ip = buffer + 14;

    // Version 4, IHL 5: a bare 20-byte header, no options.
    if (ip[0] != 0x45) {
        return R_ENV_ERR_IHL;
    }

    // The declared datagram bounds everything below and must fit the frame.
    // Ethernet padding after it is allowed (frame may be longer).
    const uint16_t ipTotalLen = rd_u16(ip + 2);
    if (ipTotalLen < 28 || 14u + ipTotalLen > size) {
        return R_ENV_ERR_IP_LEN;
    }

    // More-Fragments flag or nonzero fragment offset: fragmentation unsupported.
    const uint16_t flagsFrag = rd_u16(ip + 6);
    if ((flagsFrag & 0x2000) != 0 || (flagsFrag & 0x1FFF) != 0) {
        return R_ENV_ERR_FRAGMENT;
    }

    if (ip[9] != 17) {                      // UDP
        return R_ENV_ERR_PROTO;
    }

    const uint8_t* udp = ip + 20;
    if (rd_u16(udp + 2) != RSess::UDP_DST_PORT) {
        return R_ENV_ERR_UDP_PORT;
    }

    const uint16_t udpLen = rd_u16(udp + 4);
    if (udpLen < 8 || udpLen != ipTotalLen - 20u) {
        return R_ENV_ERR_UDP_LEN;
    }

    // IPv4 header checksum, from the NIC when it verified it, else in software.
    const uint64_t ck = olFlags & RTE_MBUF_F_RX_IP_CKSUM_MASK;
    if (ck == RTE_MBUF_F_RX_IP_CKSUM_BAD) {
        return R_ENV_ERR_IP_CKSUM;
    }
    if (ck == RTE_MBUF_F_RX_IP_CKSUM_UNKNOWN && !ipv4_checksum_ok(ip)) {
        return R_ENV_ERR_IP_CKSUM;
    }

    dstIP = rd_u32(ip + 16);
    return R_ENV_OK;
}

int ProcessBusParser::parse_goose_packet(const uint8_t *buffer, int size,
                                         GoosePassport &passport,
                                         GooseState &state)
{
    if (size < 64) {
        return -1;
    }

    passport.dmac = MAC(buffer);

    uint32_t pos = 14;
    if (buffer[12] == 0x81 && buffer[13] == 0x00 &&
        buffer[16] == 0x88 && buffer[17] == 0xB8) {
        // VLAN -> GOOSE
        pos += 4;
    } else if (buffer[12] == 0x88 && buffer[13] == 0xB8) {
        // GOOSE without VLAN
    } else {
        return -1;
    }

    passport.appid = NET_TO_CPU_U16(buffer + pos);
    pos += 8; // APPID, Length, Reserv1, Reserv2

    // PDU
    if (buffer[pos++] != 0x61) {
        return -3;
    }
    int pduSize = decode_asn1_len(buffer, &pos);

    bool found_gocbref = false, found_dataset = false, found_goid = false;
    while (pos < size) {
        uint8_t tag = buffer[pos++];
        int itemSize = decode_asn1_len(buffer, &pos);
        if (pos + itemSize > size || itemSize == 0) {
            return -3;
        }

        switch (tag) {
        case 0x80: /* gocbRef */
            passport.gocbref = make_stringview(buffer + pos, itemSize);
            found_gocbref = true;
            break;
        case 0x81: /* timeAllowedToLive */
            break;
        case 0x82: /* DatSet */
            passport.dataset = make_stringview(buffer + pos, itemSize);
            found_dataset = true;
            break;
        case 0x83: /* GoID */
            passport.goid = make_stringview(buffer + pos, itemSize);
            found_goid = true;
            break;
        case 0x84:
            if (itemSize >= 4 && itemSize <= 8) {
                uint64_t ts = NET_TO_CPU_U64(buffer + pos);
                ts >>= (8 - itemSize) * 8;
                state.timestamp = ts;
            }
            break;
        case 0x85:
            state.stNum = decode_asn1_number(buffer + pos, itemSize);
            break;
        case 0x86:
            state.sqNum = decode_asn1_number(buffer + pos, itemSize);
            break;
        case 0x87: /* Simulation */
            break;
        case 0x88: /* CRev */
            passport.crev = decode_asn1_number(buffer + pos, itemSize);
            break;
        case 0x89: /* NdsCom */
            break;
        case 0x8a: /* Num DataSet entries */
            passport.num = decode_asn1_number(buffer + pos, itemSize);
            break;
        case 0xab: /* allData */
        {
            const int num = parse_alldata(buffer, pos, pos + itemSize, 0);
            if (num < 0) {
                return -101;
            }
            passport.allDataOffset = pos;
            passport.foundEntries = num;
            break;
        }
        case 0x30: // SEQUENCE
        case 0x31: // SET
            for (uint32_t end = pos + itemSize; pos < end;) {
                uint8_t innerTag = buffer[pos++];
                pos += decode_asn1_len(buffer, &pos);
            }
            continue;
        case 0xA0: // Context-specific 0
        case 0xA1: // Context-specific 1
            pos += decode_asn1_len(buffer, &pos);
            continue;
        default:
            break;
        }

        pos += itemSize;
    }
    if (passport.foundEntries != passport.num) {
        return -102;
    }
    return (found_gocbref && found_dataset && found_goid) ? 0 : -100;
}

int ProcessBusParser::parse_sv_packet(const uint8_t *buffer, int size,
                                      SVStreamPassport &passport,
                                      SVStreamState &state)
{
    if (size < 64) {
        return -1;
    }

    passport.dmac = MAC(buffer);

    uint32_t pos = 14;
    if (buffer[12] == 0x81 && buffer[13] == 0x00 &&
        buffer[16] == 0x88 && buffer[17] == 0xBA) {
        // VLAN -> SV
        pos += 4;
    } else if (buffer[12] == 0x88 && buffer[13] == 0xBA) {
        // SV without VLAN
    } else {
        return -2;
    }

    passport.appid = NET_TO_CPU_U16(buffer + pos);
    pos += 8; // APPID, Length, Reserv1, Reserv2

    return parse_sv_apdu(buffer + pos, static_cast< uint32_t >(size) - pos,
                         passport, state);
}


namespace
{
    /*
     * Unwrap the routable envelope: bound the session PDU, parse the session
     * header, verify/decrypt, then read the payload header.
     *
     * apduBase set to the start of the APDU on success
     * Returns APDU size, or a negative error code
     */
    int unwrap_r_frame(uint8_t *buffer, int size, uint8_t expectedSI, uint8_t expectedType,
                       RSess::SecurityMode mode, RSess::Crypto &crypto,
                       RSess::SessionHeader &session, RSess::PayloadHeader &phdr,
                       const uint8_t *&apduBase)
    {
#ifdef PLATFORM_ORANGEPI3B
        // RK3566 has no PCIe cache coherency: in-place GCM decryption dirties
        // the mbuf, and the write-back corrupts the next packet DMA'd into the
        // recycled buffer. Decrypt a copy.
        static thread_local uint8_t scratch[2048];
        if (mode == RSess::SEC_GCM && size >= 0
            && static_cast< size_t >(size) <= sizeof(scratch)) {
            std::memcpy(scratch, buffer, static_cast< size_t >(size));
            buffer = scratch;
        }
#endif
        /*
         * The frame may be padded to 60 bytes, so the IPv4 total length bounds
         * the session PDU, not the frame length.
         */
        if (size < (int)(RSess::ETH_IP_UDP_SIZE + RSess::apdu_offset(0))) {
            return RSess::ERR_TOO_SHORT;
        }
        const uint16_t ipTotalLen = NET_TO_CPU_U16(buffer + 16);
        if (ipTotalLen < 28 || 14 + ipTotalLen > size) {
            return RSess::ERR_TOO_SHORT;
        }

        uint8_t* udp = buffer + RSess::ETH_IP_UDP_SIZE;
        const size_t udpSize = ipTotalLen - 28u;   // minus IPv4 (20) and UDP (8)

        const int phOff = RSess::parse_session(udp, udpSize, session);
        if (phOff < 0) {
            return phOff;
        }
        if (session.si != expectedSI) {
            return RSess::ERR_BAD_SI;
        }

        /*
         * unseal() is in another translation unit, so SEC_NONE would pay a call
         * per packet for nothing.
         */
        if (mode != RSess::SEC_NONE
            && !RSess::unseal(udp, udpSize, mode, session, crypto)) {
            return R_PARSE_ERR_AUTH;
        }

        const int apduOff = RSess::parse_payload(udp, udpSize, session, phdr);
        if (apduOff < 0) {
            return apduOff;
        }
        if (phdr.payloadType != expectedType) {
            return RSess::ERR_BAD_PAYLOAD_TYPE;
        }

        apduBase = udp + apduOff;
        return static_cast< int >(phdr.apduSize);
    }
}

int ProcessBusParser::parse_r_goose_packet(uint8_t *buffer, int size,
                                           RSess::SecurityMode mode,
                                           RSess::Crypto &crypto,
                                           RSess::SessionHeader &session,
                                           GoosePassport &passport,
                                           GooseState &state)
{
    RSess::PayloadHeader phdr;
    const uint8_t* apdu = nullptr;

    const int apduSize = unwrap_r_frame(buffer, size,
                                        RSess::SI_R_GOOSE, RSess::PAYLOAD_TYPE_GOOSE,
                                        mode, crypto, session, phdr, apdu);
    if (apduSize < 0) {
        return apduSize;
    }

    passport.dmac = MAC(buffer);
    passport.appid = phdr.appid;

    /*
     * A copy of parse_goose_packet's walk: the L2 path must not gain a
     * base-pointer indirection.
     */
    uint32_t pos = 0;
    if (apduSize < 2 || apdu[pos++] != 0x61) {
        return -3;
    }
    decode_asn1_len(apdu, &pos);

    bool found_gocbref = false, found_dataset = false, found_goid = false;
    while (pos < (uint32_t)apduSize) {
        uint8_t tag = apdu[pos++];
        int itemSize = decode_asn1_len(apdu, &pos);
        if (pos + itemSize > (uint32_t)apduSize || itemSize == 0) {
            return -3;
        }

        switch (tag) {
        case 0x80: /* gocbRef */
            passport.gocbref = make_stringview(apdu + pos, itemSize);
            found_gocbref = true;
            break;
        case 0x81: /* timeAllowedToLive */
            break;
        case 0x82: /* DatSet */
            passport.dataset = make_stringview(apdu + pos, itemSize);
            found_dataset = true;
            break;
        case 0x83: /* GoID */
            passport.goid = make_stringview(apdu + pos, itemSize);
            found_goid = true;
            break;
        case 0x84:
            if (itemSize >= 4 && itemSize <= 8) {
                uint64_t ts = NET_TO_CPU_U64(apdu + pos);
                ts >>= (8 - itemSize) * 8;
                state.timestamp = ts;
            }
            break;
        case 0x85:
            state.stNum = decode_asn1_number(apdu + pos, itemSize);
            break;
        case 0x86:
            state.sqNum = decode_asn1_number(apdu + pos, itemSize);
            break;
        case 0x87: /* Simulation */
            break;
        case 0x88: /* CRev */
            passport.crev = decode_asn1_number(apdu + pos, itemSize);
            break;
        case 0x89: /* NdsCom */
            break;
        case 0x8a: /* Num DataSet entries */
            passport.num = decode_asn1_number(apdu + pos, itemSize);
            break;
        case 0xab: /* allData */
        {
            const int num = parse_alldata(apdu, pos, pos + itemSize, 0);
            if (num < 0) {
                return -101;
            }
            passport.allDataOffset = pos;
            passport.foundEntries = num;
            break;
        }
        case 0x30: // SEQUENCE
        case 0x31: // SET
            for (uint32_t end = pos + itemSize; pos < end;) {
                ++pos; // tag
                pos += decode_asn1_len(apdu, &pos);
            }
            continue;
        case 0xA0: // Context-specific 0
        case 0xA1: // Context-specific 1
            pos += decode_asn1_len(apdu, &pos);
            continue;
        default:
            break;
        }

        pos += itemSize;
    }
    if (passport.foundEntries != passport.num) {
        return -102;
    }
    return (found_gocbref && found_dataset && found_goid) ? 0 : -100;
}

int ProcessBusParser::parse_r_sv_packet(uint8_t *buffer, int size,
                                        RSess::SecurityMode mode,
                                        RSess::Crypto &crypto,
                                        RSess::SessionHeader &session,
                                        SVStreamPassport &passport,
                                        SVStreamState &state)
{
    RSess::PayloadHeader phdr;
    const uint8_t* apdu = nullptr;

    const int apduSize = unwrap_r_frame(buffer, size,
                                        RSess::SI_R_SV, RSess::PAYLOAD_TYPE_SV,
                                        mode, crypto, session, phdr, apdu);
    if (apduSize < 0) {
        return apduSize;
    }

    passport.dmac = MAC(buffer);
    passport.appid = phdr.appid;

    return parse_sv_apdu(apdu, static_cast< uint32_t >(apduSize), passport, state);
}
