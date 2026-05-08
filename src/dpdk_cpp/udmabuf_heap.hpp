#pragma once

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 ProcessBus contributors
 *
 * UdmabufHeap — RAII wrapper that pulls an u-dma-buf-allocated CMA region
 * into DPDK as an external malloc heap.
 *
 * Why this exists: on SoCs whose PCIe is not cache-coherent (Rockchip
 * RK3566 / Cortex-A55) DPDK descriptor rings sharing a 64-byte cache line
 * with neighbouring 16-byte descriptors get clobbered by CPU writebacks.
 * The fix is to allocate descriptor rings from Normal/Non-Cacheable
 * memory; u-dma-buf's `sync_mode=1` mapping (selected via O_SYNC) gives us
 * exactly that, backed by physically contiguous CMA.
 *
 * Caller pre-conditions:
 *   - u-dma-buf module loaded (udmabuf0=<size> dma_mask_bit=64).
 *   - Region large enough for IGC_MAX_TXD * 16 + IGC_MAX_RXD * 16
 *     (~1 MB for i225). Sizing is the caller's responsibility.
 *
 * The heap and its memory persist for the lifetime of this object. Pass
 * socket_id() to rte_eth_{rx,tx}_queue_setup so the PMD's underlying
 * rte_memzone_reserve_aligned() routes through this heap.
 */

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_malloc.h>
#include <rte_memory.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace DPDK
{
    class UdmabufHeap
    {
    public:
        /*
         * `name` is the u-dma-buf instance name, e.g. "udmabuf0".
         * `heap` is the DPDK-side heap name; defaults to "udmabuf_heap".
         */
        explicit UdmabufHeap(std::string name,
                             std::string heap = "udmabuf_heap")
            : m_name(std::move(name))
            , m_heap(std::move(heap))
        {
            const std::string sysfsBase = "/sys/class/u-dma-buf/" + m_name;

            /*
             * On aarch64 u-dma-buf's SYNC_MODE_NONCACHED maps to
             * pgprot_noncached() = MT_DEVICE_nGnRnE (Device memory) which
             * forbids unaligned access — DPDK's heap-add memsets SIGBUS.
             * Force sync_mode=2 (WRITECOMBINE) → MT_NORMAL_NC, which is
             * non-cacheable AND allows unaligned. Has the same effect for
             * the descriptor-aliasing fix because there's still no D-cache
             * line for these addresses.
             */
            writeSysfs(sysfsBase + "/sync_mode", "2");

            m_size = readU64(sysfsBase + "/size");
            m_iova = readU64(sysfsBase + "/phys_addr");

            if (m_size == 0 || m_iova == 0) {
                throw std::runtime_error("u-dma-buf '" + m_name +
                                         "' has zero size or zero phys_addr");
            }

            const std::string devPath = "/dev/" + m_name;
            m_fd = ::open(devPath.c_str(), O_RDWR | O_SYNC);
            if (m_fd < 0) {
                throw std::runtime_error("Cannot open " + devPath +
                                         ": " + std::string(std::strerror(errno)));
            }

            m_va = ::mmap(nullptr, m_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, m_fd, 0);
            if (m_va == MAP_FAILED) {
                ::close(m_fd);
                m_fd = -1;
                throw std::runtime_error("mmap " + devPath +
                                         " failed: " + std::string(std::strerror(errno)));
            }

            if (rte_malloc_heap_create(m_heap.c_str()) != 0) {
                releaseMmap();
                throw std::runtime_error("rte_malloc_heap_create('" + m_heap +
                                         "') failed: " + std::string(rte_strerror(rte_errno)));
            }
            m_heapCreated = true;

            const size_t pageSize = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
            if (m_size % pageSize != 0) {
                cleanup();
                throw std::runtime_error("u-dma-buf size not page-aligned");
            }
            const size_t nPages = m_size / pageSize;
            std::vector<rte_iova_t> iovas(nPages);
            for (size_t i = 0; i < nPages; i++) {
                iovas[i] = m_iova + i * pageSize;
            }

            if (rte_malloc_heap_memory_add(m_heap.c_str(), m_va, m_size,
                                           iovas.data(), nPages, pageSize) != 0) {
                cleanup();
                throw std::runtime_error("rte_malloc_heap_memory_add failed: " +
                                         std::string(rte_strerror(rte_errno)));
            }
            m_memoryAdded = true;

            m_socketID = rte_malloc_heap_get_socket(m_heap.c_str());
            if (m_socketID < 0) {
                cleanup();
                throw std::runtime_error("rte_malloc_heap_get_socket failed");
            }
        }

        ~UdmabufHeap() { cleanup(); }

        UdmabufHeap(const UdmabufHeap&) = delete;
        UdmabufHeap& operator=(const UdmabufHeap&) = delete;
        UdmabufHeap(UdmabufHeap&&) = delete;
        UdmabufHeap& operator=(UdmabufHeap&&) = delete;

        int          socket_id() const { return m_socketID; }
        rte_iova_t   iova()      const { return m_iova; }
        size_t       size()      const { return m_size; }
        const void*  va()        const { return m_va; }
        const std::string& name() const { return m_name; }

    private:
        static unsigned long readU64(const std::string& path)
        {
            FILE* f = std::fopen(path.c_str(), "r");
            if (!f) {
                throw std::runtime_error("Cannot open " + path +
                                         ": " + std::string(std::strerror(errno)));
            }
            char buf[64] = {0};
            const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            if (n == 0) {
                throw std::runtime_error("Empty " + path);
            }
            return std::strtoul(buf, nullptr, 0);
        }

        static void writeSysfs(const std::string& path, const std::string& value)
        {
            FILE* f = std::fopen(path.c_str(), "w");
            if (!f) {
                throw std::runtime_error("Cannot open " + path + " for write: " +
                                         std::string(std::strerror(errno)));
            }
            const size_t n = std::fwrite(value.data(), 1, value.size(), f);
            std::fclose(f);
            if (n != value.size()) {
                throw std::runtime_error("Short write to " + path);
            }
        }

        void releaseMmap()
        {
            if (m_va && m_va != MAP_FAILED) {
                ::munmap(m_va, m_size);
                m_va = nullptr;
            }
            if (m_fd >= 0) {
                ::close(m_fd);
                m_fd = -1;
            }
        }

        void cleanup()
        {
            if (m_memoryAdded) {
                rte_malloc_heap_memory_remove(m_heap.c_str(), m_va, m_size);
                m_memoryAdded = false;
            }
            if (m_heapCreated) {
                rte_malloc_heap_destroy(m_heap.c_str());
                m_heapCreated = false;
            }
            releaseMmap();
            m_socketID = -1;
        }

        std::string  m_name;
        std::string  m_heap;
        int          m_fd = -1;
        void*        m_va = nullptr;
        size_t       m_size = 0;
        rte_iova_t   m_iova = 0;
        int          m_socketID = -1;
        bool         m_heapCreated = false;
        bool         m_memoryAdded = false;
    };
}
