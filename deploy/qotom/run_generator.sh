#!/bin/bash

NIC_PCI_ADDR="${NIC_PCI_ADDR:-0B:00.0}"
LCORES="${LCORES:-1}"

exec bin/bus_generator -l "$LCORES" \
    -a "$NIC_PCI_ADDR" \
    --huge-dir=/mnt/bus_gen/ \
    --file-prefix=bus_gen \
    -- $@

