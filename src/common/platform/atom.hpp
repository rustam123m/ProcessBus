#pragma once

namespace Platform
{
    // Mempool sizes
    constexpr unsigned PROCESSOR_MBUF_NUM = 4 * 512 * 1024;   // 2M
    constexpr unsigned GENERATOR_MBUF_NUM = 2 * 512 * 1024;   // 1M
    constexpr unsigned MEMPOOL_CACHE_SIZE = 64;

    // Ring descriptors
    constexpr unsigned PROCESSOR_RX_DESC = 64 * 1024 - 1;     // 65535
    constexpr unsigned PROCESSOR_TX_DESC = 128;
    constexpr unsigned GENERATOR_RX_DESC = 128;
    constexpr unsigned GENERATOR_TX_DESC = 64 * 1024 - 1;     // 65535

    // Tools (delay_meter, pkt_redirect)
    constexpr unsigned TOOL_MBUF_NUM = 32 * 1024;
    constexpr unsigned TOOL_DESC_NUM = 1024;
}
