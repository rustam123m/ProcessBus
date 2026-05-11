## IEC 61850 ProcessBus in Linux

This repository contains a proof-of-concept that combines five key ideas:

1. Using DPDK for IRQ-free Ethernet packet handling in Linux userspace.

2. Dedicating a core or multiple cores to handle huge ProcessBus/StationBus traffic.

3. Running Linux with a fully preemptible kernel (RT).

4. Splitting real-time-dependent traffic, such as IEC 61850 GOOSE & SV, from IP traffic for use in CPAC, vPAC, or Digital Fault Recorder systems.

5. Monitoring and measuring the state of ProcessBus/StationBuses, including signaling congestion.

[Alternative Perspective on the Process Bus](docs/Article_ProcessBus.pdf)

## Applications

1. **bus_generator:** A frame generator for GOOSE & SV protocols.
    (support for R-GOOSE & R-SV are planned)

2. **bus_processor:** An example application for processing GOOSE & SV frames.

3. **delay_meter:** An application for measuring delays in Ethernet by sending special frames.

4. **pkt_redirect:** A simple example of redirecting packets between Ethernet ports.

5. **rtspin:** A tool to consume the entire CPU time by running a thread with maximum priority.

6. **rx_counter:** A pure RX-path counter (`rx_burst` → `free_bulk`, no parsing). Use as a baseline against `bus_processor` to tell whether the bottleneck is the app or the PMD/NIC/PCIe.

## Platforms

- **QEMU VM** for functional validation — see [deploy/qemu/README.md](deploy/qemu/README.md)
- **Qotom Intel Atom** servers, bare metal with PREEMPT_RT — see [deploy/qotom/README.md](deploy/qotom/README.md)
- **OrangePi 3B** (RockChip RK3566, aarch64) with PREEMPT_RT — see [deploy/orangepi3b/README.md](deploy/orangepi3b/README.md)

## How to Build Applications

Builds run inside a Debian container so the host's toolchain does not matter. The script `ci/build.sh` selects the right container and toolchain per platform via `--platform=`:

- `--platform=atom` → `ci/Dockerfile.debian` (x86_64, native)
- `--platform=qemu` → `ci/Dockerfile.debian` (x86_64, generic ISA for VMs)
- `--platform=orangepi3b` → `ci/Dockerfile.debian-arm64` (aarch64 cross-build, Cortex-A55)

Per-platform build/install directories (e.g. `build-orangepi3b/`, `install-orangepi3b/`) let multiple platforms coexist on the same host without stomping each other.

The script builds:

1. DPDK, submodule in `3rdparty/dpdk`. Statically linked. Built `release` (no DWARF).

2. libiec61850, submodule in `3rdparty/libiec61850`.

3. `bus_processor`, `bus_generator`, `unit_tests` and tools. Built `RelWithDebInfo` (`-O3 -g -DNDEBUG`) — DWARF stays in the binary, no split-debug step.

## How to Run

Each platform has its own deploy directory under `deploy/<platform>/` containing `setup_platform.sh`, `run_generator.sh`, `run_processor.sh`, and a platform README that covers the host setup (hugepages, NIC binding, RT tuning, etc.). The app command-line below is the same on every platform; only the wrapper scripts differ.

- QEMU walkthrough: [docs/Running_with_QEMU.md](docs/Running_with_QEMU.md)
- Per-platform notes: [deploy/qemu/README.md](deploy/qemu/README.md), [deploy/qotom/README.md](deploy/qotom/README.md), [deploy/orangepi3b/README.md](deploy/orangepi3b/README.md)

For example, generating packets:

1. `./run_generator.sh --sv80 500`
   Generate 500 SV protocol according to 9.2LE 80 points.

2. `./run_generator.sh --sv256 500`
   Generate 500 SV protocol according to 9.2LE 256 points.

3. `./run_generator.sh --goose 100,1000`
   Generate 100 unique GOOSE messages with 1000 changes per second.

Processing packets:

1. `./run_processor.sh --sv80 100`
   Expect 100 SV streams from a generator.

2. `./run_processor.sh --sv256 100`
   Expect 100 SV streams from a generator.

3. `./run_processor.sh --goose 100`
   Expect 100 GOOSE messages from a generator.

## Performance metrics

[Results in nonRT mode](docs/Results_in_nonRT.md)

## License

GPL-3.0

