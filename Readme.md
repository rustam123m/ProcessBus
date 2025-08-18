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

## Platforms

- QEMU scripts are provided for testing purposes.
- Embedded systems based on Intel Atom (e.g. Qotom servers)
- ARM64 is planned (e.g. RockChip)

## How to Build Applications

`(by using special Docker container ci/Dockerfile.debian)`

The script `ci/build.sh` builds:

1. DPDK, which is a submodule in `3rdparty/dpdk`.

2. libiec61850, which is a submodule in `3rdparty/libiec61850`.

3. `bus_processor`, `bus_generator`, `unit_tests` and tools.

## How to Run

[Running with Qemu](docs/Running_with_QEMU.md)

There are special scripts(qemu): `run_generator.sh` and `run_processor.sh`.

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

