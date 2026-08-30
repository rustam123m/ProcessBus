# Cost of a productive DPDK RX burst

The RX loop was measured twice with one RX core. The only code moved into the
timed region was `rte_eth_rx_burst()`:

- **Parse only:** accounting starts immediately after `rte_eth_rx_burst()`.
- **Parse + RX burst:** `--rx-account-burst` starts accounting immediately
  before the call.
- **RX-burst delta:** `(corrected CPU - parse-only CPU) / RX pps`.

Empty polls are excluded in both variants. Each point is an independent
60-second run.

## Results

The comparison uses 42 matching scenarios that passed on both platforms. A pass requires cumulative TX = RX,
zero GOOSE/SV sequence gaps, and zero NIC, queue, parser, authentication and
generator errors.

| Platform + NIC | RX descriptor memory | Parse only | Parse + RX burst | RX-burst delta |
|---|---|---:|---:|---:|
| Orange Pi 3B + I225-V | Normal-NC | 1,785 ns/frame | 2,601 ns/frame | **702 ns/frame** |
| Atom C3808 + X553 | Cacheable | 1,245 ns/frame | 1,294 ns/frame | **48 ns/frame** |

The first two numeric columns are medians of their respective measurements.
The delta is the median of the 42 per-scenario differences, so it is not the
difference between the two displayed medians.

### Representative measurements

All values are ns/frame.

| Traffic | Load | Orange Pi parse | Orange Pi total | Orange Pi delta | Atom parse | Atom total | Atom delta |
|---|---:|---:|---:|---:|---:|---:|---:|
| SV80 L2 | 200 streams / 800 kpps | 304 | 965 | **661** | 283 | 329 | 46 |
| R-SV80 UDP | 200 streams / 800 kpps | 421 | 1,106 | **685** | 349 | 393 | 44 |
| GOOSE-64 L2 | 300 IEDs × 1 kHz | 1,261 | 1,951 | **690** | 667 | 708 | 41 |
| R-GOOSE UDP | 300 IEDs × 1 kHz | 1,537 | 2,239 | **703** | 727 | 771 | 44 |
| R-GOOSE HMAC (OpenSSL) | 200 IEDs × 1 kHz | 3,014 | 3,608 | **594** | 1,927 | 1,972 | 44 |
| R-GOOSE GCM (OpenSSL) | 200 IEDs × 1 kHz | 3,417 | 4,145 | **729** | 1,711 | 1,742 | 31 |
| SV256 L2 | 200 streams / 320 kpps | 1,565 | 2,452 | **886** | 1,110 | 1,152 | 42 |
| R-SV256 GCM (Mbed TLS) | 25 streams / 40 kpps | 7,272 | 7,952 | **680** | 5,778 | 5,876 | 98 |

## Interpretation

- **Orange Pi:** the productive RX-burst call adds about 0.7 µs/frame. For
  SV80 L2 at 800 kpps, it is 68% of the measured 965 ns/frame total.
- **Atom:** the median delta is 48 ns/frame and remains small compared with the
  protocol-processing cost.
- **Scope:** this is a platform comparison, not a controlled memory-only A/B
  test. CPU, NIC and PMD also differ. The result is consistent with the cost of
  the Orange Pi's Normal-NC RX descriptor ring, but it does not isolate that
  ring as the only cause.

The Orange Pi non-coherent-DMA design is described in
[DPDK with an external PCIe NIC on Orange Pi 3B](DPDK_on_OrangePi3B.md).
