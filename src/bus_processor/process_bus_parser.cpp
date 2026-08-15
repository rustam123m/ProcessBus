#include "process_bus_parser.hpp"

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
}

#define NET_TO_CPU_U16(x)       RTE_STATIC_BSWAP16(*(uint16_t *)(x))
#define NET_TO_CPU_U32(x)       RTE_STATIC_BSWAP32(*(uint32_t *)(x))
#define NET_TO_CPU_U64(x)       RTE_STATIC_BSWAP64(*(uint64_t *)(x))

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

    // SV PDU (tag 0x60)
    if (pos >= size || buffer[pos++] != 0x60) {
        return -3;
    }
    decode_asn1_len(buffer, &pos);

    // Parse noASDU (tag 0x80)
    if (pos + 3 <= size && buffer[pos] == 0x80) {
        passport.num =  buffer[pos + 2];
        pos += 3;
    }

    // Sequence of ASDUs (tag 0xa2)
    if (pos >= size || buffer[pos++] != 0xa2) {
        return -4;
    }
    decode_asn1_len(buffer, &pos);

    // ASDU (tag 0x30)
    if (pos >= size || buffer[pos++] != 0x30) {
        return -5;
    }
    decode_asn1_len(buffer, &pos);

    // Parse ASDU fields
    while (pos < size) {
        uint8_t tag = buffer[pos++];
        int length = decode_asn1_len(buffer, &pos);
        if (pos + length > size || length <= 0) {
            break;
        }

        switch (tag) {
        case 0x80: // svID
            passport.svid =  make_stringview(buffer + pos, length);
            break;
        case 0x82: // smpCnt
            state.smpCnt = NET_TO_CPU_U16(buffer + pos);
            break;
        case 0x83: // confRev
            passport.crev = NET_TO_CPU_U32(buffer + pos);
            break;
        case 0x84: // refrTm
            if (length == 8) {
                /* std::copy(buffer.begin() + pos, buffer.begin() + pos + 8, pkt.refrTm.begin()); */
            }
            break;
        case 0x85: // smpSynch
            /* pkt.smpSynch = buffer[pos]; */
            break;
        case 0x86: // smpRate
            /* pkt.smpRate = (buffer[pos] << 8) | buffer[pos + 1]; */
            break;
        case 0x87: // data
            /* pkt.data = buffer.subspan(pos, length); */
            break;
        case 0x88: // smpMod
            /* pkt.smpMod = buffer[pos]; */
            break;
        }

        pos += length;
    }
    return 0;
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

    uint32_t pos = 0;
    if (apduSize < 2 || apdu[pos++] != 0x60) {
        return -3;
    }
    decode_asn1_len(apdu, &pos);

    // noASDU (tag 0x80)
    if (pos + 3 <= (uint32_t)apduSize && apdu[pos] == 0x80) {
        passport.num = apdu[pos + 2];
        pos += 3;
    }

    // Sequence of ASDUs (tag 0xa2)
    if (pos >= (uint32_t)apduSize || apdu[pos++] != 0xa2) {
        return -4;
    }
    decode_asn1_len(apdu, &pos);

    // ASDU (tag 0x30)
    if (pos >= (uint32_t)apduSize || apdu[pos++] != 0x30) {
        return -5;
    }
    decode_asn1_len(apdu, &pos);

    while (pos < (uint32_t)apduSize) {
        uint8_t tag = apdu[pos++];
        int length = decode_asn1_len(apdu, &pos);
        if (pos + length > (uint32_t)apduSize || length <= 0) {
            break;
        }

        switch (tag) {
        case 0x80: // svID
            passport.svid = make_stringview(apdu + pos, length);
            break;
        case 0x82: // smpCnt
            state.smpCnt = NET_TO_CPU_U16(apdu + pos);
            break;
        case 0x83: // confRev
            passport.crev = NET_TO_CPU_U32(apdu + pos);
            break;
        }

        pos += length;
    }
    return 0;
}
