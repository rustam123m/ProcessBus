#!/bin/bash
# Processor on the i225 (DPDK uio_pci_generic). Pass app args via "--".
# Example:  sudo bash run_processor.sh --goose 1
#
# By default the i225 is the input port (so loopback-cable tests have the
# generator on a peer / on end0). Set USE_AF_PACKET=1 to switch the input
# to end0 via the af_packet vdev instead — useful when the generator runs
# on the same machine bound to the i225.

NIC_PCI_ADDR="${NIC_PCI_ADDR:-0000:01:00.0}"
HOST_IFACE="${HOST_IFACE:-end0}"
LCORES="${LCORES:-2}"

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
