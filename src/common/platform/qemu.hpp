#pragma once

namespace Platform
{
    // Mempool sizes — small for QEMU VM hugepage limits
    constexpr unsigned PROCESSOR_MBUF_NUM = 128 * 1024;
    constexpr unsigned GENERATOR_MBUF_NUM = 128 * 1024;
    constexpr unsigned MEMPOOL_CACHE_SIZE = 64;

    // Ring descriptors
    constexpr unsigned PROCESSOR_RX_DESC = 1024;
    constexpr unsigned PROCESSOR_TX_DESC = 128;
    constexpr unsigned GENERATOR_RX_DESC = 128;
    constexpr unsigned GENERATOR_TX_DESC = 1024;

    // Tools
    constexpr unsigned TOOL_MBUF_NUM = 8 * 1024;
    constexpr unsigned TOOL_DESC_NUM = 512;
}
