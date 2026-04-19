#pragma once

// Placeholder for future ARM64 RockChip support.
// Values to be tuned when hardware is available.

namespace Platform
{
    // Mempool sizes
    constexpr unsigned PROCESSOR_MBUF_NUM = 1 * 1024 * 1024;
    constexpr unsigned GENERATOR_MBUF_NUM = 512 * 1024;
    constexpr unsigned MEMPOOL_CACHE_SIZE = 64;

    // Ring descriptors
    constexpr unsigned PROCESSOR_RX_DESC = 4096;
    constexpr unsigned PROCESSOR_TX_DESC = 128;
    constexpr unsigned GENERATOR_RX_DESC = 128;
    constexpr unsigned GENERATOR_TX_DESC = 4096;

    // Tools
    constexpr unsigned TOOL_MBUF_NUM = 16 * 1024;
    constexpr unsigned TOOL_DESC_NUM = 1024;
}
