#include "r_frame_builder.hpp"

#include <cstdio>
#include <cstring>

namespace
{
    inline void put_u16(uint8_t* p, uint16_t v)
    {
        p[0] = static_cast< uint8_t >(v >> 8);
        p[1] = static_cast< uint8_t >(v);
    }

    inline void put_u32(uint8_t* p, uint32_t v)
    {
        p[0] = static_cast< uint8_t >(v >> 24);
        p[1] = static_cast< uint8_t >(v >> 16);
        p[2] = static_cast< uint8_t >(v >> 8);
        p[3] = static_cast< uint8_t >(v);
    }

    // Standard one's-complement checksum over the 20-byte IPv4 header.
    uint16_t ipv4_checksum(const uint8_t* header)
    {
        uint32_t sum = 0;
        for (size_t i=0;i<20;i+=2) {
            sum += static_cast< uint32_t >((header[i] << 8) | header[i + 1]);
        }
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        return static_cast< uint16_t >(~sum);
    }
}

namespace RFrame
{

size_t build_prefix(uint8_t* buf, const RFrameConfig& cfg, size_t udpPayloadSize)
{
    // Ethernet: RFC 1112 mapping of the IPv4 multicast group onto a MAC address.
    std::memcpy(buf + OFF_ETH_DMAC, multicast_mac(cfg.dstIP).data(), 6);
    std::memcpy(buf + OFF_ETH_SMAC, cfg.srcMAC, 6);
    put_u16(buf + OFF_ETH_TYPE, 0x0800);

    // IPv4
    uint8_t* ip = buf + OFF_IP;
    ip[0] = 0x45;                                       // version 4, IHL 5
    ip[1] = 0x00;                                       // DSCP / ECN
    put_u16(ip + 2, static_cast< uint16_t >(20 + 8 + udpPayloadSize));
    put_u16(ip + 4, 0);                                 // identification
    put_u16(ip + 6, 0x4000);                            // don't fragment
    ip[8] = cfg.ttl;
    ip[9] = 17;                                         // UDP
    put_u16(ip + 10, 0);                                // checksum placeholder
    put_u32(ip + 12, cfg.srcIP);
    put_u32(ip + 16, cfg.dstIP);
    put_u16(ip + 10, ipv4_checksum(ip));

    // Source port is written per packet; the checksum stays zero.
    put_u16(buf + OFF_UDP_SRC_PORT, 0);
    put_u16(buf + OFF_UDP_DST_PORT, cfg.dstPort);
    put_u16(buf + OFF_UDP_LENGTH, static_cast< uint16_t >(8 + udpPayloadSize));
    put_u16(buf + OFF_UDP_CSUM, 0);

    return OFF_UDP_PAYLOAD;
}

bool parse_ipv4(const char* text, uint32_t& out)
{
    unsigned b[4] = {};
    char tail = 0;
    if (std::sscanf(text, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) != 4) {
        return false;
    }
    for (unsigned octet : b) {
        if (octet > 255) {
            return false;
        }
    }
    out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return true;
}

}
