#!/bin/bash
# Mount hugetlbfs + bind virtio NICs to DPDK driver inside the QEMU VM.
# Run on the VM as root after each boot. Idempotent.
#
# This is a functional VM (virtio NICs, no real-time). No RT tuning here —
# see deploy/qotom/setup_platform.sh for the bare-metal version.

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"

# --- Platform values (qemu VM) ---
EXPECTED_HUGEPAGES=1024
HUGEPAGE_MOUNTS=(/mnt/bus_gen /mnt/bus_proc /mnt/delay_meter /mnt/redirect1 /mnt/redirect2)
DPDK_DRIVER="uio_pci_generic"
NIC_PCI_ADDRS=(
    0000:00:04.0
    0000:00:05.0
)

# --- Reserve hugepages at runtime (VM is throwaway; no GRUB edit) ---
HP_TOTAL=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
if [ "${HP_TOTAL:-0}" -lt "$EXPECTED_HUGEPAGES" ]; then
    echo "Reserving $EXPECTED_HUGEPAGES x 2M hugepages..."
    echo "$EXPECTED_HUGEPAGES" > /proc/sys/vm/nr_hugepages
    HP_TOTAL=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
    if [ "${HP_TOTAL:-0}" -lt "$EXPECTED_HUGEPAGES" ]; then
        echo "ERROR: HugePages_Total=$HP_TOTAL after request (asked for $EXPECTED_HUGEPAGES)." >&2
        echo "VM is likely too fragmented; reboot and retry." >&2
        exit 1
    fi
fi

# --- Mount hugetlbfs ---
for mp in "${HUGEPAGE_MOUNTS[@]}"; do
    mkdir -p "$mp"
    if ! mountpoint -q "$mp"; then
        mount -t hugetlbfs nodev "$mp"
    fi
done

# --- Bind NICs to DPDK driver ---
modprobe uio
modprobe "$DPDK_DRIVER"
"$SCRIPT_DIR/dpdk-devbind.py" --bind="$DPDK_DRIVER" "${NIC_PCI_ADDRS[@]}"

# --- Sentinel: cleared on reboot (tmpfs) ---
touch /var/run/pbus_qemu

echo "OK: setup_platform.sh applied."
