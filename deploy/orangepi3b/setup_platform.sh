#!/bin/bash
# OrangePi 3B / RK3566 platform setup for ProcessBus DPDK fast path.
# Run on the target as root after each boot. Idempotent.
#
# What this does:
#   - allocates 2 MiB hugepages and mounts hugetlbfs (CMA-backed)
#   - loads u-dma-buf with sync_mode=2 (Normal-NC descriptors required by
#     the non-coherent RK3566 PCIe; sync_mode=1 would be MT_DEVICE which
#     forbids unaligned and SIGBUSes DPDK heap-add)
#   - loads uio_pci_generic and binds the i225 NIC for DPDK
#   - puts end0 in promisc+allmulti so the af_packet PMD or tcpdump can
#     sniff GOOSE/SV multicast on the kernel-bound port
#
# Memory budget on a 1.9 GiB OPI3B (PROCESSOR_MBUF_NUM=512 K):
#   processor pool ≈ 1.28 GiB | generator pool ≈ 0.32 GiB | DPDK heap +
#   descriptor zones ≈ 100 MiB. Default 768 × 2 MiB = 1.5 GiB hugepages
#   covers processor-only stress tests with slack; running both apps at
#   once needs HUGEPAGES=896 (≈1.75 GiB) and a quiet userland.

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"

# --- Tunables (env-overridable) ---
HUGEPAGES="${HUGEPAGES:-768}"          # 2 MiB pages — 768 = 1.5 GiB
UDMABUF_BYTES="${UDMABUF_BYTES:-4194304}"   # 4 MiB CMA region for descriptor rings
SYNC_MODE="${SYNC_MODE:-2}"            # 2 = MT_NORMAL_NC (correct on aarch64)
NIC_PCI_ADDR="${NIC_PCI_ADDR:-0000:01:00.0}" # i225-V
HOST_IFACE="${HOST_IFACE:-end0}"       # built-in GMAC (af_packet sniff target)
DPDK_DRIVER="uio_pci_generic"

# --- Sanity: kernel module file must be next to this script ---
UDMABUF_KO="$SCRIPT_DIR/u-dma-buf.ko"
if ! lsmod | grep -q "^u_dma_buf "; then
    if [ ! -f "$UDMABUF_KO" ]; then
        echo "ERROR: $UDMABUF_KO not found." >&2
        echo "Cross-build u-dma-buf against the running kernel and copy" >&2
        echo "the .ko next to this script. See README.md." >&2
        exit 1
    fi
fi

# --- Load kernel modules ---
modprobe "$DPDK_DRIVER"

if ! lsmod | grep -q "^u_dma_buf "; then
    insmod "$UDMABUF_KO" udmabuf0="$UDMABUF_BYTES" dma_mask_bit=64
fi

# Force Normal-NC mapping. MUST be set before any DPDK process opens
# /dev/udmabuf0 — re-applying is harmless.
echo "$SYNC_MODE" > /sys/class/u-dma-buf/udmabuf0/sync_mode
ACTUAL_SYNC=$(cat /sys/class/u-dma-buf/udmabuf0/sync_mode)
if [ "$ACTUAL_SYNC" != "$SYNC_MODE" ]; then
    echo "ERROR: sync_mode is $ACTUAL_SYNC, expected $SYNC_MODE" >&2
    exit 1
fi

# --- Hugepages: runtime allocation (Armbian default cmdline lacks
#     hugepages=N; persistence belongs in /etc/sysctl.d/99-pbus.conf
#     after we know the platform is happy). ---
mkdir -p /dev/hugepages
mountpoint -q /dev/hugepages || mount -t hugetlbfs none /dev/hugepages
echo "$HUGEPAGES" > /proc/sys/vm/nr_hugepages
HP_OK=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
if [ "$HP_OK" -lt "$HUGEPAGES" ]; then
    echo "WARN: only $HP_OK hugepages allocated, requested $HUGEPAGES" >&2
    echo "      (CMA fragmentation? Free RAM = $(awk '/^MemFree:/ {print $2}' /proc/meminfo) kB)" >&2
fi

# --- Bind i225 to uio_pci_generic ---
CURR_DRV=$(basename "$(readlink -f /sys/bus/pci/devices/$NIC_PCI_ADDR/driver 2>/dev/null)" 2>/dev/null || echo none)
if [ "$CURR_DRV" != "$DPDK_DRIVER" ]; then
    KERNEL_NAME=$(ls /sys/bus/pci/devices/$NIC_PCI_ADDR/net 2>/dev/null | head -1)
    [ -n "$KERNEL_NAME" ] && ip link set "$KERNEL_NAME" down 2>/dev/null || true
    "$SCRIPT_DIR/dpdk-devbind.py" --bind="$DPDK_DRIVER" "$NIC_PCI_ADDR"
fi

# --- Host iface for af_packet PMD: promisc + allmulti so GOOSE/SV
#     multicast frames are not filtered by the kernel ---
ip link set "$HOST_IFACE" up
ip link set "$HOST_IFACE" promisc on
ip link set "$HOST_IFACE" allmulticast on

# --- Sentinel: cleared on reboot (tmpfs) ---
touch /var/run/pbus_rt

# --- Status report ---
echo "OK: setup_platform.sh applied."
echo "  hugepages         : $HP_OK × 2 MiB ($((HP_OK * 2)) MiB)"
echo "  u-dma-buf         : udmabuf0 size=$(cat /sys/class/u-dma-buf/udmabuf0/size) sync_mode=$ACTUAL_SYNC"
echo "  $NIC_PCI_ADDR (i225) : $DPDK_DRIVER"
echo "  $HOST_IFACE         : $(cat /sys/class/net/$HOST_IFACE/operstate) promisc=$(cat /sys/class/net/$HOST_IFACE/flags)"
