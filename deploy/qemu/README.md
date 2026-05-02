# QEMU VM — Onboarding

Functional validation target. A small Debian 13 VM with virtio NICs that
runs the same DPDK binaries as the bare-metal target. Not a performance
target — there is no PREEMPT_RT, no CPU isolation, no IRQ pinning.

VM layout: 4 vCPUs, 4 GB RAM, 4 virtio NICs (one for management, two for
DPDK gen/proc, one spare). 1 GB of hugepages reserved at runtime.

---

## 1. One-time manual setup

### 1.1 On the host (dev box)

Bring up the VM with the helper Makefile in this directory:

```bash
cd deploy/qemu
make fetch        # install qemu-system-x86, fetch Debian ISO
make hard         # create qcow2 disk image
make interfaces   # create vm_tap0..3 + assign 192.168.21.1 to vm_tap0
make internet     # NAT vm_tap0 → uplink so the VM can reach the internet
make install      # boot from ISO, install Debian manually
make run          # boot the installed VM with all 4 tap NICs attached
```

`make install` is interactive Debian installer. After install completes,
shut down the VM and use `make run` from then on.

`make internet` assumes the host's uplink is `wlp3s0`. Edit the rule if
yours differs.

### 1.2 On the VM (one-time)

```bash
sudo apt install -y --no-install-recommends \
    pciutils libatomic1 elfutils \
    libbpf1 libpcap0.8 libxdp1 libnuma1 \
    openssh-server psmisc kmod iproute2 python3
```

DPDK runtime libs the binaries link against, plus `python3` for
`dpdk-devbind.py`, `pciutils` for `lspci`, `kmod` for `modprobe`.

### 1.3 SSH key

From the dev box:

```bash
ssh-copy-id jarvis@192.168.21.2
```

`jarvis` must exist with passwordless sudo.

### 1.4 Kernel command line (optional)

The default Debian VM kernel is fine. If you want hugepages reserved at
boot instead of at runtime (which is what `setup_platform.sh` does
today), add to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`:

```
default_hugepagesz=2M hugepages=1024
```

then `sudo update-grub && sudo reboot`. Not required.

---

## 2. Routine use (from the dev box)

Single entry point: `deploy/qemu/deploy.sh`.

```bash
# Push files only (no rebuild, no scenarios)
./deploy/qemu/deploy.sh

# Push and run a single scenario
./deploy/qemu/deploy.sh --scenario goose_10x10

# Push and run every scenario
./deploy/qemu/deploy.sh --all

# Rebuild first, then push and run
./deploy/qemu/deploy.sh --rebuild --all

# Override target host
./deploy/qemu/deploy.sh --scenario goose_1x1 --host 10.0.0.5
```

What it does in order:
1. (Optional) `ci/build.sh --platform=qemu --rebuild`.
2. Bundle `install-qemu/*` + common scripts + this directory's scripts
   and `scenarios.conf` into `deploy/qemu/pbus/`, scp to
   `$USER@$HOST:/home/jarvis/pbus/`.
3. Check the sentinel `/var/run/pbus_qemu` on the VM. If absent, run
   `setup_platform.sh` (reserve 1024 hugepages, mount hugetlbfs, bind
   the two virtio NICs to `uio_pci_generic`). If present, skip. The
   sentinel lives on tmpfs, so it clears on every reboot.
4. For each requested scenario: run `target_runner.sh` over SSH,
   fetch logs, parse to JSON under `results/<YYYY-MM-DD>/<name>/`.
5. Exit code = number of failed scenarios.

Available scenarios are listed in `scenarios.conf`.

---

## 3. Verification

```bash
ssh jarvis@192.168.21.2 "test -e /var/run/pbus_qemu && echo 'set up' || echo 'NOT set up'"
ssh jarvis@192.168.21.2 "grep ^Huge /proc/meminfo"
ssh jarvis@192.168.21.2 "lspci -k -s 0000:00:04.0"   # expect Kernel driver in use: uio_pci_generic
```

Scenarios in `scenarios.conf` are functional checks: low rate, short
duration, `max_latency_us=0` (latency is not asserted on a VM).

---

## 4. Rollback

The VM has no persistent tuning to roll back. `setup_platform.sh` only
touches runtime state (sysctl + mounts + driver bind), all of which clear
on reboot.

To re-trigger setup without a reboot:

```bash
ssh jarvis@192.168.21.2 "sudo rm -f /var/run/pbus_qemu"
./deploy/qemu/deploy.sh
```

---

## 5. Differences vs `qotom`

| Aspect | qemu | qotom |
|---|---|---|
| Purpose | functional validation | performance / latency |
| Kernel | stock Debian | `linux-image-rt-amd64` (PREEMPT_RT) |
| CPU isolation | none | `isolcpus=1-11`, `nohz_full`, `rcu_nocbs` |
| IRQ pinning | none | all to CPU0 |
| DPDK driver | `uio_pci_generic` | `vfio-pci` |
| Hugepages | 1024 × 2M (runtime) | 4096 × 2M (GRUB cmdline) |
| NICs | 2 virtio | 7 physical |
| Sentinel | `/var/run/pbus_qemu` | `/var/run/pbus_rt` |
