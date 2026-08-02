#!/bin/bash
# Apply RT tuning + mount hugetlbfs + bind NICs to DPDK driver.
# Run on the target as root after each boot. Idempotent.
#
# Hugepages must be reserved at boot via GRUB cmdline. See README.md.

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"

# --- Platform values (qotom / blackbox) ---
EXPECTED_HUGEPAGES=4096
HUGEPAGE_MOUNTS=(/mnt/bus_gen /mnt/bus_proc /mnt/delay_meter /mnt/redirect1 /mnt/redirect2)
DPDK_DRIVER="vfio-pci"
NIC_PCI_ADDRS=(
    0000:0c:00.0
    0000:0b:00.0
    0000:04:00.0
    0000:05:00.0
    0000:06:00.0
    0000:07:00.0
)

# --- Sanity: hugepages reserved at boot? ---
HP_TOTAL=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
if [ "${HP_TOTAL:-0}" -lt "$EXPECTED_HUGEPAGES" ]; then
    echo "ERROR: HugePages_Total=$HP_TOTAL, expected >= $EXPECTED_HUGEPAGES" >&2
    echo "Add 'default_hugepagesz=2M hugepages=$EXPECTED_HUGEPAGES' to" >&2
    echo "GRUB_CMDLINE_LINUX in /etc/default/grub, then update-grub + reboot." >&2
    echo "See README.md." >&2
    exit 1
fi

# Best-effort tuning: some IRQs / kthreads / cpufreq nodes legitimately
# reject writes (per-CPU timers, managed IRQs, missing threads). Disable
# strict mode for the loops, re-enable for critical operations after.
set +e

# --- Scheduler ---
echo -1 > /proc/sys/kernel/sched_rt_runtime_us
echo  4 > /proc/sys/kernel/sched_rr_timeslice_ms

# --- Workqueues to CPU0 ---
for q in /sys/devices/virtual/workqueue/*/cpumask; do
    echo 1 > "$q" 2>/dev/null
done

# --- All movable IRQs to CPU0 (housekeeping) ---
for i in /proc/irq/*/smp_affinity; do
    echo 1 > "$i" 2>/dev/null
done
echo 1 > /proc/irq/default_smp_affinity 2>/dev/null

# --- Performance governor on RT cores (CPU0 = housekeeping) ---
NCPU=$(nproc)
for C in $(seq 1 $((NCPU - 1))); do
    GOV="/sys/devices/system/cpu/cpu$C/cpufreq/scaling_governor"
    [ -e "$GOV" ] && echo performance > "$GOV" 2>/dev/null
done

# --- Disable lockup detectors (cmdline already sets nowatchdog/nmi_watchdog=0) ---
echo 0 > /proc/sys/kernel/watchdog 2>/dev/null
echo 0 > /proc/sys/kernel/nmi_watchdog 2>/dev/null
echo 0 > /proc/sys/kernel/softlockup_panic 2>/dev/null
echo 0 > /proc/sys/kernel/softlockup_all_cpu_backtrace 2>/dev/null

# --- RT priorities for kernel threads (silent if a thread doesn't exist) ---
declare -A KTHREAD_PRIO=(
    ["ktimers"]="99 fifo"
    ["irq_work"]="98 fifo"
    ["rcuog"]="95 rr"
    ["rcuop"]="95 rr"
    ["rcuc"]="95 rr"
)
for tname in "${!KTHREAD_PRIO[@]}"; do
    pp=(${KTHREAD_PRIO[$tname]})
    for pid in $(pgrep -f "$tname"); do
        chrt --${pp[1]} -p ${pp[0]} "$pid" 2>/dev/null
    done
done

# Back to strict mode — the next steps must succeed.
set -e

# --- Mount hugetlbfs (boot reservation comes from GRUB cmdline) ---
for mp in "${HUGEPAGE_MOUNTS[@]}"; do
    mkdir -p "$mp"
    if ! mountpoint -q "$mp"; then
        mount -t hugetlbfs nodev "$mp"
    fi
done

# --- Bind NICs to DPDK driver ---
modprobe vfio
modprobe "$DPDK_DRIVER"
"$SCRIPT_DIR/dpdk-devbind.py" --bind="$DPDK_DRIVER" "${NIC_PCI_ADDRS[@]}"

# --- Sentinel: cleared on reboot (tmpfs) ---
touch /var/run/pbus_rt

echo "OK: setup_platform.sh applied."
