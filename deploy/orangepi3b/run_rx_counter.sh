#!/bin/bash
# Pure RX-path counter on the i225 (DPDK uio_pci_generic). Same port shape as
# bus_processor, so its PPS is the baseline the processor is measured against.
# One optional argument: the run duration in seconds.

# Own our working directory so the caller needs only "<script> <seconds>".
cd "$(dirname "$(realpath "$0")")" || exit 1

NIC_PCI_ADDR="${NIC_PCI_ADDR:-0000:01:00.0}"
LCORES="${LCORES:-2}"
RUN_TIME="${1:-60}"

# FLR clears the SW/FW semaphore that a SIGKILLed prior run leaves locked.
echo 1 > "/sys/bus/pci/devices/$NIC_PCI_ADDR/reset"

exec bin/rx_counter -l "$LCORES" -n 1 \
    -a "$NIC_PCI_ADDR" \
    --file-prefix=bus_proc \
    -- --time "$RUN_TIME"
