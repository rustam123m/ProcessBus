#!/bin/bash

NIC_PCI_ADDR="${NIC_PCI_ADDR:-0C:00.0}"
LCORES="${LCORES:-4}"

exec bin/bus_processor -l "$LCORES" \
    -a "$NIC_PCI_ADDR" \
    --huge-dir=/mnt/bus_proc/ \
    --file-prefix=bus_proc \
    -- $@

