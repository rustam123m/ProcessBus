/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 ProcessBus contributors
 *
 * udmabuf_check — kill-switch sanity test for u-dma-buf on the OrangePi 3B.
 *
 * Run after `insmod u-dma-buf.ko udmabuf0=<size> dma_mask_bit=64`. The test
 * confirms that:
 *
 *   1. /sys/class/u-dma-buf/udmabuf0/{size,phys_addr} are readable.
 *   2. /dev/udmabuf0 can be opened with O_RDWR | O_SYNC and mmap'd.
 *   3. A pattern written via that mapping survives a fresh mmap of the
 *      same physical region — i.e. CPU writes hit DRAM, not just a cache
 *      line that nobody else can see.
 *
 * If this fails, the whole "uncached descriptor ring" plan is dead in the
 * water and we re-think (custom kernel module, different sync_mode, etc.).
 *
 * Build (host or pbus_builder_arm64):
 *   aarch64-linux-gnu-gcc -O2 -Wall -o udmabuf_check udmabuf_check.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

static unsigned long
read_u64_sysfs(const char *path)
{
    char buf[64] = {0};
    int  fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (read(fd, buf, sizeof buf - 1) <= 0) {
        fprintf(stderr, "read %s: %s\n", path, strerror(errno));
        exit(1);
    }
    close(fd);
    return strtoull(buf, NULL, 0);
}

int
main(int argc, char **argv)
{
    const char *name    = (argc > 1) ? argv[1] : "udmabuf0";
    char        sz_path[128], pa_path[128], dev_path[128];

    snprintf(sz_path,  sizeof sz_path,  "/sys/class/u-dma-buf/%s/size",      name);
    snprintf(pa_path,  sizeof pa_path,  "/sys/class/u-dma-buf/%s/phys_addr", name);
    snprintf(dev_path, sizeof dev_path, "/dev/%s",                           name);

    unsigned long size = read_u64_sysfs(sz_path);
    unsigned long phys = read_u64_sysfs(pa_path);
    printf("%s: size=%lu (0x%lx), phys_addr=0x%lx\n",
           name, size, size, phys);

    /* Map 1: writer (uncached because of O_SYNC + sync_mode=1 default). */
    int fd_w = open(dev_path, O_RDWR | O_SYNC);
    if (fd_w < 0) {
        fprintf(stderr, "open %s for write: %s\n", dev_path, strerror(errno));
        return 1;
    }
    void *va_w = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_w, 0);
    if (va_w == MAP_FAILED) {
        fprintf(stderr, "mmap rw: %s\n", strerror(errno));
        return 1;
    }

    uint32_t      *p   = (uint32_t *)va_w;
    unsigned long  n   = size / sizeof(uint32_t);
    for (unsigned long i = 0; i < n; i++)
        p[i] = (uint32_t)(0xa55aa55a ^ i);

    /*
     * Read back via the same mapping. Any failure here means basic
     * mmap plumbing is broken.
     */
    int errs = 0;
    for (unsigned long i = 0; i < n; i++) {
        uint32_t expected = (uint32_t)(0xa55aa55a ^ i);
        if (p[i] != expected) {
            if (errs < 5)
                fprintf(stderr, "[same-map %lu] expected 0x%08x got 0x%08x\n",
                        i, expected, p[i]);
            errs++;
        }
    }
    printf("same-mapping roundtrip: %s (%d errors / %lu words)\n",
           errs == 0 ? "OK" : "FAIL", errs, n);
    if (errs)
        return 1;

    /*
     * Map 2: fresh open, fresh mmap. If our writes only landed in CPU
     * cache, this view (which the kernel may have set up uncached) sees
     * stale or zero memory. Both maps in same process — kernel keeps the
     * physical region pinned regardless.
     */
    int fd_r = open(dev_path, O_RDONLY | O_SYNC);
    if (fd_r < 0) {
        fprintf(stderr, "open %s for read: %s\n", dev_path, strerror(errno));
        return 1;
    }
    const uint32_t *q = mmap(NULL, size, PROT_READ, MAP_SHARED, fd_r, 0);
    if (q == MAP_FAILED) {
        fprintf(stderr, "mmap ro: %s\n", strerror(errno));
        return 1;
    }

    errs = 0;
    for (unsigned long i = 0; i < n; i++) {
        uint32_t expected = (uint32_t)(0xa55aa55a ^ i);
        if (q[i] != expected) {
            if (errs < 5)
                fprintf(stderr, "[fresh-map %lu] expected 0x%08x got 0x%08x\n",
                        i, expected, q[i]);
            errs++;
        }
    }
    printf("fresh-mapping visibility: %s (%d errors / %lu words)\n",
           errs == 0 ? "OK" : "FAIL", errs, n);

    munmap(va_w, size);
    munmap((void *)q, size);
    close(fd_w);
    close(fd_r);
    return errs == 0 ? 0 : 1;
}
