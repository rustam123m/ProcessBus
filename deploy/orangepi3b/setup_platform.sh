#!/bin/bash
# OPI3B / RK3566 host setup for DPDK on the i225. Run as root after each boot.
# u-dma-buf sync_mode=2 (Normal-NC) is required by non-coherent RK3566 PCIe.

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"

# --- Tunables (env-overridable) ---
HUGEPAGES_32M="${HUGEPAGES_32M:-32}"   # 32 × 32 MiB = 1 GiB. A55 dTLB has 32 entries.
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

# Pin to performance: ondemand ramps cold cores from 408 MHz, adding ms latency on wake-up.
for g in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null || true
done

# Disable THP: DPDK uses explicit hugetlbfs; THP only adds khugepaged jitter.
THP_DIR=/sys/kernel/mm/transparent_hugepage
if [ -w "$THP_DIR/enabled" ]; then echo never > "$THP_DIR/enabled"; fi
if [ -w "$THP_DIR/defrag" ];  then echo never > "$THP_DIR/defrag";  fi

# Uncap SCHED_FIFO: default throttle preempts RT threads 50 ms / s.
sysctl -wq kernel.sched_rt_runtime_us=-1

# --- Hugepages: 32 MiB pages — arm64 PMD-contiguous, single A55 dTLB entry per page.
#     1 GiB attempts failed at boot (CMA fragments low-address region). ---
echo "$HUGEPAGES_32M" > /sys/kernel/mm/hugepages/hugepages-32768kB/nr_hugepages
HP_OK=$(cat /sys/kernel/mm/hugepages/hugepages-32768kB/nr_hugepages)
if [ "$HP_OK" -lt "$HUGEPAGES_32M" ]; then
    echo "ERROR: only $HP_OK of $HUGEPAGES_32M × 32 MiB pages allocated." >&2
    echo "       Free RAM = $(awk '/^MemFree:/ {print $2}' /proc/meminfo) kB" >&2
    exit 1
fi
# Remount /dev/hugepages with pagesize=32M. systemd auto-mounts it with the
# kernel's default_hugepagesz, which may be different.
mountpoint -q /dev/hugepages && umount /dev/hugepages
mkdir -p /dev/hugepages
mount -t hugetlbfs -o pagesize=32M none /dev/hugepages

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
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)
THP=$(grep -oE '\[[a-z]+\]' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo n/a)
RT=$(sysctl -n kernel.sched_rt_runtime_us 2>/dev/null || echo n/a)
echo "OK: setup_platform.sh applied."
echo "  cpu governor      : $GOV"
echo "  thp               : $THP"
echo "  sched_rt_runtime  : $RT"
echo "  hugepages         : $HP_OK × 32 MiB ($((HP_OK * 32)) MiB)"
echo "  u-dma-buf         : udmabuf0 size=$(cat /sys/class/u-dma-buf/udmabuf0/size) sync_mode=$ACTUAL_SYNC"
echo "  $NIC_PCI_ADDR (i225) : $DPDK_DRIVER"
echo "  $HOST_IFACE         : $(cat /sys/class/net/$HOST_IFACE/operstate) promisc=$(cat /sys/class/net/$HOST_IFACE/flags)"
