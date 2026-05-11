#!/bin/bash
# Processor on the i225 (DPDK uio_pci_generic). Pass app args via "--".
# USE_AF_PACKET=1 switches input to end0 via the af_packet vdev.

NIC_PCI_ADDR="${NIC_PCI_ADDR:-0000:01:00.0}"
HOST_IFACE="${HOST_IFACE:-end0}"
LCORES="${LCORES:-2}"

# FLR clears the SW/FW semaphore that a SIGKILLed prior run leaves locked.
if [ "${USE_AF_PACKET:-0}" -ne 1 ]; then
    echo 1 > "/sys/bus/pci/devices/$NIC_PCI_ADDR/reset"
fi

if [ "${USE_AF_PACKET:-0}" -eq 1 ]; then
    exec bin/bus_processor -l "$LCORES" -n 1 \
        --no-pci \
        --vdev=eth_af_packet0,iface="$HOST_IFACE" \
        --file-prefix=bus_proc \
        -- "$@"
else
    exec bin/bus_processor -l "$LCORES" -n 1 \
        -a "$NIC_PCI_ADDR" \
        --file-prefix=bus_proc \
        -- "$@"
fi
