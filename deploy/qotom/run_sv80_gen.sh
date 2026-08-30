#!/bin/bash
# SV80 generator for the rx_counter baseline. One argument: the stream count.
# Fixed 40 s run so the tool and the generator overlap without host signalling.

cd "$(dirname "$(realpath "$0")")" || exit 1

NIC_PCI_ADDR="${NIC_PCI_ADDR:-0000:07:00.0}"
LCORES="${LCORES:-1,2,3,4}"
STREAMS="${1:-250}"

exec bin/bus_generator -l "$LCORES" \
    -a "$NIC_PCI_ADDR" \
    --huge-dir=/mnt/bus_gen/ \
    --file-prefix=bus_gen \
    -- --sv80 "$STREAMS" --time 40
