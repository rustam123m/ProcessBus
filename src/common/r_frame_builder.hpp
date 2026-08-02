#pragma once

#include "r_session.hpp"
#include "mac_addr.hpp"

#include <cstdint>

// Host byte order. Defaults per docs/r-messages/plan-implementation.md §3.3.
constexpr uint32_t R_DEFAULT_DST_IP = 0xEFC00101;   // 239.192.1.1
constexpr uint32_t R_DEFAULT_SRC_IP = 0xC0A80A01;   // 192.168.10.1

struct RFrameConfig
{
    uint32_t    srcIP = R_DEFAULT_SRC_IP;
    uint32_t    dstIP = R_DEFAULT_DST_IP;
    uint16_t    dstPort = RSess::UDP_DST_PORT;
    uint8_t     ttl = 64;                   // routable: must survive a hop
    uint8_t     srcMAC[6] = { 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5 };

    RSess::SecurityMode mode = RSess::SEC_NONE;
};

namespace RFrame
{
    // Offsets from the start of the Ethernet frame.
    enum Offset : size_t
    {
        OFF_ETH_DMAC    = 0,
        OFF_ETH_SMAC    = 6,
        OFF_ETH_TYPE    = 12,
        OFF_IP          = 14,
        OFF_IP_PROTO    = 23,
        OFF_IP_CSUM     = 24,
        OFF_IP_SRC      = 26,
        OFF_IP_DST      = 30,
        OFF_UDP         = 34,
        OFF_UDP_SRC_PORT= 34,
        OFF_UDP_DST_PORT= 36,
        OFF_UDP_LENGTH  = 38,
        OFF_UDP_CSUM    = 40,
        OFF_UDP_PAYLOAD = 42,
    };

    /*
     * Write the 42-byte Ethernet/IPv4/UDP prefix.
     * udpPayloadSize size of the session PDU that follows
     * Returns RFrame::OFF_UDP_PAYLOAD
     */
    size_t build_prefix(uint8_t* buf, const RFrameConfig& cfg, size_t udpPayloadSize);

    // Destination MAC an R-frame to dstIP carries (RFC 1112).
    inline MAC multicast_mac(uint32_t dstIP)
    {
        const uint8_t bytes[6] = {
            0x01, 0x00, 0x5E,
            static_cast< uint8_t >((dstIP >> 16) & 0x7F),
            static_cast< uint8_t >((dstIP >> 8) & 0xFF),
            static_cast< uint8_t >(dstIP & 0xFF),
        };
        return MAC(bytes);
    }

    // Parse dotted-quad into host byte order. Returns false on malformed input.
    bool parse_ipv4(const char* text, uint32_t& out);
}
