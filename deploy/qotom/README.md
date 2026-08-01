# Qotom Server (blackbox) — Onboarding

Hardware: Intel Atom C3808 (Denverton), 12 cores, no HT, single NUMA.
OS: Debian 13 (trixie) with `linux-image-rt-amd64` (PREEMPT_RT 6.12).

CPU split: **CPU 0 = housekeeping**, **CPUs 1–11 = isolated RT**.

---

## 1. One-time manual setup (on the target)

### 1.1 Packages

RT kernel and tools:

```bash
sudo apt install -y \
    linux-image-rt-amd64 \
    linux-cpupower \
    rt-tests
```

Keep `linux-image-amd64` installed as a recovery option.

DPDK runtime dependencies (libs the binaries link against, plus helpers
used by `setup_platform.sh` and `dpdk-devbind.py`):

```bash
sudo apt install -y --no-install-recommends \
    pciutils libatomic1 elfutils \
    libbpf1 libpcap0.8 libxdp1 libnuma1 \
    openssh-server psmisc kmod iproute2
```

### 1.2 User account and SSH key

The deploy flow expects user `jarvis` with passwordless sudo. On a fresh
box, as root:

```bash
useradd -m -s /bin/bash jarvis
echo "jarvis ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/jarvis
chmod 440 /etc/sudoers.d/jarvis
```

Then from the dev box (so `deploy.sh` can SSH in passwordless):

```bash
ssh-copy-id jarvis@blackbox
```

### 1.3 BIOS

| Setting | Value |
|---|---|
| C-states | Off (or limit C1) |
| SpeedStep / EIST | Off |
| Turbo Boost | Off (or pinned) |
| Hyper-Threading | Off |
| HPET | On |
| TSC | Locked |
| USB legacy / fan-ctrl SMI | Off |

### 1.4 Kernel command line

Edit `/etc/default/grub`, replace `GRUB_CMDLINE_LINUX` with:

```
GRUB_CMDLINE_LINUX="intel_iommu=on iommu=pt isolcpus=domain,managed_irq,1-11 nohz_full=1-11 rcu_nocbs=1-11 rcu_nocb_poll rcutree.kthread_prio=95 irqaffinity=0 processor.max_cstate=1 intel_idle.max_cstate=0 intel_pstate=disable nosoftlockup nowatchdog nmi_watchdog=0 audit=0 mce=ignore_ce skew_tick=1 tsc=reliable transparent_hugepage=never default_hugepagesz=2M hugepages=4096 mitigations=off"
```

Apply and reboot:

```bash
sudo update-grub
sudo reboot
```

After reboot, verify:

```bash
cat /proc/cmdline
grep ^Huge /proc/meminfo      # expect HugePages_Total: 4096, Hugepagesize: 2048 kB
```

Per-flag details: `Documentation/admin-guide/kernel-parameters.txt` in
the kernel source tree.

---

## 2. Routine use (from the dev box)

Single entry point: `deploy/qotom/deploy.sh`.

```bash
# Push files only (no rebuild, no scenarios)
./deploy/qotom/deploy.sh

# Push and run a single scenario
./deploy/qotom/deploy.sh --scenario goose_100x10k

# Push and run every scenario
./deploy/qotom/deploy.sh --all

# Rebuild first, then push and run
./deploy/qotom/deploy.sh --rebuild --all

# Override target host
./deploy/qotom/deploy.sh --scenario sv80_500 --host 192.168.100.103
```

What it does in order:
1. (Optional) `ci/build.sh --rebuild`.
2. Bundle `install-atom/*` + common scripts + this directory's scripts and
   `scenarios.conf` into `deploy/qotom/pbus/`, scp to
   `$USER@$HOST:/home/jarvis/pbus/`.
3. Check the sentinel `/var/run/pbus_rt` on the target. If absent,
   run `setup_platform.sh` (governor, IRQ pin, kthread RT priorities,
   hugetlbfs mounts, NIC bind to `vfio-pci`). If present, skip.
   The sentinel lives on tmpfs, so it clears on every reboot.
4. For each requested scenario: run `target_runner.sh` over SSH,
   fetch logs, parse to JSON under `results/<YYYY-MM-DD>/<name>/`.
5. Exit code = number of failed scenarios.

Available scenarios are listed by running `deploy.sh` with no flags
and reading `scenarios.conf`.

---

## 3. Verification

Quick spot-check after first deploy:

```bash
ssh jarvis@blackbox "test -e /var/run/pbus_rt && echo 'tuned' || echo 'NOT tuned'"
ssh jarvis@blackbox "grep ^Huge /proc/meminfo"
ssh jarvis@blackbox "cat /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor"
```

Latency baseline (run on the target):

```bash
sudo cyclictest -m -p 99 -t 11 -i 200 -l 100000000 -a 1-11
sudo rtla osnoise top -c 1-11 -d 1h
sudo rtla timerlat hist -c 1-11 -d 1h
sudo hwlatdetect --duration=600
```

Targets on Atom C3808:

* `cyclictest` max: < 30 µs over 24 h
* `cyclictest` p99: < 10 µs
* `hwlatdetect`: 0 hits in 10 min

---

## 4. Rollback

1. At GRUB menu → "Advanced options" → non-RT 6.12 kernel.
2. Restore `GRUB_CMDLINE_LINUX="intel_iommu=on iommu=pt"` in `/etc/default/grub`.
3. `sudo update-grub && sudo reboot`.

To re-trigger setup on the target without a reboot:

```bash
ssh jarvis@blackbox "sudo rm -f /var/run/pbus_rt"
./deploy/qotom/deploy.sh
```

---

## 5. References

* OSADL latency monitoring: <https://www.osadl.org/Continuous-latency-monitoring.qa-farm-latency-monitoring.0.html>
* Linux Foundation RT wiki: <https://wiki.linuxfoundation.org/realtime/start>
* Debian RealTime: <https://wiki.debian.org/RealTime>
* Kernel: `Documentation/admin-guide/kernel-parameters.txt`, `tools/tracing/rtla/`
