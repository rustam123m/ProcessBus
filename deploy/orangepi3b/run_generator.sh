#!/bin/bash
# Generator on the i225 (DPDK uio_pci_generic). Pass app args via "--".
# Example:  sudo bash run_generator.sh --goose 1,500000

NIC_PCI_ADDR="${NIC_PCI_ADDR:-0000:01:00.0}"
LCORES="${LCORES:-1}"

# FLR clears the SW/FW semaphore that a SIGKILLed prior run leaves locked.
echo 1 > "/sys/bus/pci/devices/$NIC_PCI_ADDR/reset"

exec bin/bus_generator -l "$LCORES" -n 1 \
    -a "$NIC_PCI_ADDR" \
    --file-prefix=bus_gen \
    -- "$@"
