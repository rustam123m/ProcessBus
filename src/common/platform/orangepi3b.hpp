#pragma once

// Orange Pi 3B (Rockchip RK3566 / Cortex-A55, 1.9 GB RAM).
// Sized conservatively for 512 MB of 2 MB hugepages — leaves headroom
// on a board that also runs a kernel + Armbian userland.

namespace Platform
{
    // Mempool sizes
    constexpr unsigned PROCESSOR_MBUF_NUM = 128 * 1024;
    constexpr unsigned GENERATOR_MBUF_NUM = 128 * 1024;
    constexpr unsigned MEMPOOL_CACHE_SIZE = 64;

    // Ring descriptors — i225 PCIe NIC
    constexpr unsigned PROCESSOR_RX_DESC = 1024;
    constexpr unsigned PROCESSOR_TX_DESC = 128;
    constexpr unsigned GENERATOR_RX_DESC = 128;
    constexpr unsigned GENERATOR_TX_DESC = 1024;

    // Tools (delay_meter, pkt_redirect)
    constexpr unsigned TOOL_MBUF_NUM = 8 * 1024;
    constexpr unsigned TOOL_DESC_NUM = 512;
}
