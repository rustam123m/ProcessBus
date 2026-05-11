#pragma once

// Orange Pi 3B (Rockchip RK3566 / Cortex-A55, 1.9 GB RAM).
// Sized for 32 × 32 MiB hugepages (1 GiB) — fits A55's 32-entry dTLB exactly.

namespace Platform
{
    // 400K mbufs × ~2304 B ≈ 922 MiB ≈ 29 × 32 MiB pages.
    constexpr unsigned PROCESSOR_MBUF_NUM = 400 * 1024;
    constexpr unsigned GENERATOR_MBUF_NUM = 128 * 1024;
    constexpr unsigned MEMPOOL_CACHE_SIZE = 64;

    // Ring descriptors — i225 PCIe NIC. 32K is IGC_MAX_{RX,TX}D.
    // Each ring is asymmetric: each app gets the maximum on its primary
    // direction and a small ring on the other direction.
    constexpr unsigned PROCESSOR_RX_DESC = 32 * 1024;
    constexpr unsigned PROCESSOR_TX_DESC = 128;
    constexpr unsigned GENERATOR_RX_DESC = 128;
    constexpr unsigned GENERATOR_TX_DESC = 32 * 1024;

    // Tools (delay_meter, pkt_redirect)
    constexpr unsigned TOOL_MBUF_NUM = 8 * 1024;
    constexpr unsigned TOOL_DESC_NUM = 512;
}
