# IEC 61850 Process Bus on Linux

This repository contains a proof of concept that combines five key ideas:

1. Using DPDK for interrupt-free Ethernet packet handling in Linux user space.

2. Dedicating one or more CPU cores to high-rate Process Bus and Station Bus
   traffic.

3. Running Linux with a fully preemptible PREEMPT_RT kernel.

4. Separating time-critical IEC 61850 traffic from general IP traffic in CPAC,
   vPAC and digital fault recorder systems.

5. Monitoring and measuring Process Bus and Station Bus traffic, including
   congestion detection.

[Alternative Perspective on the Process Bus](docs/Article_ProcessBus.pdf)

## Applications

1. **bus_generator:** A frame generator for GOOSE, SV, R-GOOSE and R-SV. The
   routable protocols support `none`, HMAC-SHA256-128 and AES-128-GCM security
   modes.

2. **bus_processor:** An example receiver for processing GOOSE, SV, R-GOOSE and
   R-SV frames, including authentication, decryption and protocol validation.

3. **delay_meter:** An application that measures Ethernet delay by sending
   dedicated test frames.

4. **pkt_redirect:** A simple example that redirects packets between Ethernet
   ports.

5. **rtspin:** A tool that consumes all available CPU time by running a thread
   at maximum priority.

6. **rx_counter:** A pure RX-path counter (`rx_burst` → `free_bulk`, without
   parsing). Use it as a baseline against `bus_processor` to determine whether
   the bottleneck is the application or the PMD/NIC/PCIe path.

## Platforms

- **QEMU VM** for functional validation — see [deploy/qemu/README.md](deploy/qemu/README.md)
- **Qotom Intel Atom** servers, bare metal with PREEMPT_RT — see [deploy/qotom/README.md](deploy/qotom/README.md)
- **Orange Pi 3B** (Rockchip RK3566, AArch64) with PREEMPT_RT — see [deploy/orangepi3b/README.md](deploy/orangepi3b/README.md)

## How to Build Applications

Builds run inside a Debian container so the host's toolchain does not matter. Run `ci/build.sh`; `--platform=` selects the container and toolchain:

- `--platform=atom` → `ci/Dockerfile.debian` (x86_64, native)
- `--platform=qemu` → `ci/Dockerfile.debian` (x86_64, generic ISA for VMs)
- `--platform=orangepi3b` → `ci/Dockerfile.debian-arm64` (aarch64 cross-build, Cortex-A55)

```
ci/build.sh --platform=atom --setup     # build the builder image (once)
ci/build.sh --platform=atom             # full build
ci/build.sh --platform=atom --rebuild   # recompile only, no reconfigure
ci/build.sh --platform=atom --shell     # interactive shell in the builder
```

The build is split into two stages, which matters if you invoke it in another
way:

| Script | Runs | Responsibility |
|---|---|---|
| `ci/build.sh` | on the host | picks and starts the builder container, then calls the one below |
| `ci/build_internal.sh` | inside the container | sources, DPDK, CMake, install |
| `ci/platforms.sh` | sourced by both | the per-platform table, so the two cannot disagree |

Anything already running inside a container calls `ci/build_internal.sh`
directly. That is what `.github/workflows/docker-build.yml` does: GitHub Actions
supplies its own container, and using `ci/build.sh` would nest a second one.

Per-platform build/install directories (`build-atom/`, `build-qemu/`, `build-orangepi3b/` and the matching `install-*/`) let multiple platforms coexist on the same host without stomping each other.

Configuring by hand with `cmake -S . -B build-atom` is not equivalent:
`build_internal.sh` passes `-DCMAKE_INSTALL_PREFIX`, `-DPLATFORM` and the
RelWithDebInfo flags, while a bare `cmake` invocation uses different defaults.
Use the presets in `CMakePresets.json` if you need a build tree outside the
script.

The script builds:

1. DPDK, a submodule in `3rdparty/dpdk`. It is statically linked and built in
   `release` mode (without DWARF).

2. libiec61850, a submodule in `3rdparty/libiec61850`.

3. `bus_processor`, `bus_generator`, `unit_tests` and supporting tools. They are
   built in `RelWithDebInfo` mode (`-O3 -g -DNDEBUG`); DWARF remains in the
   binary, with no split-debug step.

## How to Run

Each platform has its own deploy directory under `deploy/<platform>/` containing `setup_platform.sh`, `run_generator.sh`, `run_processor.sh`, and a platform README that covers the host setup (hugepages, NIC binding, RT tuning, etc.). The app command-line below is the same on every platform; only the wrapper scripts differ.

- QEMU walkthrough: [docs/Running_with_QEMU.md](docs/Running_with_QEMU.md)
- Per-platform notes: [deploy/qemu/README.md](deploy/qemu/README.md), [deploy/qotom/README.md](deploy/qotom/README.md), [deploy/orangepi3b/README.md](deploy/orangepi3b/README.md)

For example, generating packets:

1. `./run_generator.sh --sv80 500`
   Generate 500 SV80 streams.

2. `./run_generator.sh --sv256 500`
   Generate 500 SV256 streams.

3. `./run_generator.sh --goose 100,1000`
   Generate 100 independent GOOSE publishers with 1,000 changes per second.

Processing packets:

1. `./run_processor.sh --sv80 100`
   Expect 100 SV80 streams from a generator.

2. `./run_processor.sh --sv256 100`
   Expect 100 SV256 streams from a generator.

3. `./run_processor.sh --goose 100`
   Expect 100 independent GOOSE publishers from a generator.

### Routable GOOSE and SV (IEC 61850-90-5)

`--rgoose N,F`, `--rsv80 N` and `--rsv256 N` mirror their L2 counterparts but
wrap the same APDU in an IPv4/UDP session envelope on port 102 (protocol version
2).
`--r-mode` selects the security applied to it:

| Mode | Meaning |
|------|---------|
| `none` (default) | no authentication, no encryption |
| `hmac` | HMAC-SHA256-128 over the whole session PDU |
| `gcm` | AES-128-GCM: payload header and APDU encrypted, session header authenticated |

The cryptographic key is a hardcoded lab constant. Key management is out of
scope, so there is no key option or key file.

`--dst-ip` (default `239.192.1.1`) sets the multicast group and must match at
both ends; the receiver derives the expected destination MAC address from it.
`--src-ip` applies only to the generator.

```
./run_generator.sh --rgoose 100,1000 --r-mode gcm
./run_processor.sh --rgoose 100 --r-mode gcm
```

L2 and routable streams share the APPID-indexed lookup, so a single run must
not mix L2 and routable streams that carry the same APPID.

#### Isolating the cost of security

Comparing the four transport and security modes isolates three costs:

| Comparison | What the delta is |
|---|---|
| `--goose` → `--rgoose --r-mode none` | the routable envelope: IPv4/UDP + session parse. **No crypto runs in `none` mode at all** |
| `--rgoose none` → `--rgoose hmac` | authentication only — the APDU stays in clear |
| `--rgoose none` → `--rgoose gcm` | encryption + authentication |

`hmac` adds 18 bytes per frame and `gcm` adds 30 bytes, so compare them at a
fixed packet rate and read `Load %` per core rather than Mb/s.

AES is accelerated on both platforms. With Mbed TLS 3.6, SHA-256 is accelerated
only on the Cortex-A55 because this version has no x86 SHA-NI path. The HMAC
figures are therefore not directly comparable between the devices, while the
AES-GCM figures are.

## Performance results

[R-GOOSE and R-SV performance on x86 and ARM](docs/R-messages_Linkedin_post.md)

[Results in non-RT mode](docs/Results_in_nonRT.md)

## AI assistance

Parts of this project are developed with the help of AI coding agents, mainly
Claude Code and Codex. They are used for implementation, refactoring and test
and measurement tooling; the design decisions, the review and the published
results remain mine.

## License

GPL-3.0
