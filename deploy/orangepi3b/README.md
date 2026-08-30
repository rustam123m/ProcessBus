# OrangePi 3B — Onboarding

Hardware: Rockchip RK3566, Cortex-A55 ×4 @ 1.6 GHz, 1.92 GiB LPDDR4. The M.2
M-Key PCIe 2.0 ×1 path is populated by an external Intel I225-V NIC (BDF
`0000:01:00.0`). Built-in GMAC `end0` is used for management / af_packet
sniffing.

OS: Armbian (Debian trixie userland) with PREEMPT_RT 6.18.27 kernel.

CPU split: **CPU 0 = housekeeping**, **CPUs 1–3 = isolated RT** (set in
`/boot/armbianEnv.txt` extraargs — see §1.4).

Why this is its own platform: the RK3566 PCIe root complex is **not
cache-coherent**. DPDK descriptor rings on regular cacheable hugepage
memory exhibit cache-line aliasing between adjacent 16-byte descriptors
on the 64-byte cache line and stall the i225 TX queue at exactly
`IGC_DEFAULT_TXD` packets (1024 by default — first ring wrap). The fix is
to back the rings with `u-dma-buf` mapped Normal-Non-Cacheable; that lifts
the descriptors out of the cache entirely. Payload mbufs stay in cacheable
hugepages and use explicit `dc cvac/civac` maintenance.

---

## 1. One-time manual setup (on the target)

### 1.1 Packages

```bash
sudo apt install -y --no-install-recommends \
    build-essential pciutils kmod iproute2 \
    python3 ethtool linux-headers-$(uname -r)
```

### 1.2 User account

The deploy flow assumes `jarvis` with sudo. Default Armbian image creates
this; `123` is the password used in this repo's setup helpers.

`deploy.sh` runs `sudo bash setup_platform.sh` over SSH without a TTY, so sudo
must not prompt:

```bash
echo "jarvis ALL=(ALL) NOPASSWD:ALL" | sudo tee /etc/sudoers.d/jarvis
sudo chmod 440 /etc/sudoers.d/jarvis
```

### 1.3 SSH (over `end0`)

`end0` is the only routable interface — when the i225 is bound to
`uio_pci_generic`, that port has no kernel netdev. Keep `end0` cabled to
your management LAN; do NOT use the i225 port for management.

```bash
ssh-copy-id jarvis@<opi3b-ip>
```

### 1.4 Kernel command line

Edit `/boot/armbianEnv.txt`, append to `extraargs`:

```
isolcpus=domain,managed_irq,1-3 nohz_full=1-3 rcu_nocbs=1-3 rcu_nocb_poll
irqaffinity=0 processor.max_cstate=1 nosoftlockup nowatchdog
nmi_watchdog=0 mitigations=off cma=256M
```

`cma=256M` is required: u-dma-buf draws the descriptor region from CMA.
Reboot and verify:

```bash
cat /proc/cmdline
grep -i cma /proc/meminfo      # expect CmaTotal: ~262144 kB
```

Hugepages are NOT reserved at boot here (Armbian's `extraargs` parsing is
fragile). They are allocated at runtime by `setup_platform.sh` via
`/proc/sys/vm/nr_hugepages`. CMA must already be reserved, since hugepage
allocation will pull from `MIGRATE_CMA` if other ZONE_NORMAL is exhausted.

### 1.5 Cross-build u-dma-buf.ko once

From the dev box, in the `pbus_builder_arm64` container, against the
running OPI3B kernel source tree (we use the Armbian kernel-source
worktree — see memory `reference_armbian_build`):

```bash
# In ~/repos/OrangePi/armbian-build (kernel source worktree):
make -C ~/repos/OrangePi/armbian-build/.tmp/kernel-rockchip64-current \
     M="$(pwd)/3rdparty/udmabuf" \
     ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

The resulting `u-dma-buf.ko` belongs next to this README — `deploy.sh`
will pick it up from there.

---

## 2. Routine use

### 2.1 From the dev box

```bash
# Push files only (no rebuild, no scenarios)
./deploy/orangepi3b/deploy.sh

# Rebuild then push
./deploy/orangepi3b/deploy.sh --rebuild

# Override target host
./deploy/orangepi3b/deploy.sh --host 192.168.1.141
```

`deploy.sh`:
1. (optional) `ci/build.sh --platform=orangepi3b --rebuild`
2. Bundles `install-orangepi3b/*`, this dir's `*.sh`, `u-dma-buf.ko`
3. SCPs to `$USER@$HOST:/home/jarvis/pbus/`
4. Runs `setup_platform.sh` if sentinel `/var/run/pbus_rt` is missing

### 2.2 On the target

```bash
cd /home/jarvis/pbus

# Generator (single GOOSE stream at 50 kpps)
sudo bash run_generator.sh --goose 1,50000

# Processor (input from i225)
sudo bash run_processor.sh --goose 1

# Processor with af_packet on end0 (e.g. local loopback test where
# the generator owns the i225)
sudo USE_AF_PACKET=1 bash run_processor.sh --goose 1
```

Tunables (env vars):
- `LCORES`     — DPDK `-l` argument; default 1 for gen, 2 for proc
- `NIC_PCI_ADDR` — i225 BDF (default `0000:01:00.0`)
- `HOST_IFACE` — built-in GMAC for af_packet (default `end0`)
- `USE_AF_PACKET` — processor-only: 1 to use af_packet vdev on `end0`

### 2.3 Memory budget on 1.9 GiB

`PROCESSOR_MBUF_NUM=512K` ⇒ ~1.28 GiB pool, plus DPDK heap and rings.
Default `HUGEPAGES=768` (1.5 GiB) covers processor-only stress. To run
gen and proc on the same board simultaneously, set `HUGEPAGES=896` and
keep userland quiet.

```bash
sudo HUGEPAGES=896 bash setup_platform.sh
```

---

## 3. Verification

```bash
# All wired up?
test -e /var/run/pbus_rt && echo 'set up' || echo 'NOT set up'

# u-dma-buf with the right mapping mode
cat /sys/class/u-dma-buf/udmabuf0/sync_mode    # must be 2 (MT_NORMAL_NC)

# i225 bound to uio
readlink /sys/bus/pci/devices/0000:01:00.0/driver  # ends in /uio_pci_generic

# Hugepage budget
grep ^Huge /proc/meminfo
```

Sanity oracle for the descriptor region (mmaps `/dev/udmabuf0`, writes
pattern, verifies via fresh mapping):

```bash
gcc -O2 deploy/orangepi3b/udmabuf_check.c -o /tmp/udmabuf_check
sudo /tmp/udmabuf_check
```

---

## 4. Rollback

`u-dma-buf.ko` is unloaded with `sudo rmmod u_dma_buf`; the i225 is
returned to the kernel `igc` driver via `sudo dpdk-devbind.py --bind=igc
0000:01:00.0`. Reboot wipes hugepage allocation and the sentinel — a
fresh boot is fully clean without explicit teardown.

---

## 5. References

* aarch64 Memory Attributes & PCIe coherency:
  `Documentation/arm64/memory.rst` in the kernel source.
* u-dma-buf (ikwzm): <https://github.com/ikwzm/udmabuf>
* DPDK external-heap API:
  `lib/eal/include/rte_malloc.h` — `rte_malloc_heap_create`,
  `rte_malloc_heap_memory_add`, `rte_malloc_heap_get_socket`.
* Memory item `aarch64 pgprot_noncached() is Device memory, not
  Normal NC` — why we use `sync_mode=2` and not `sync_mode=1`.
