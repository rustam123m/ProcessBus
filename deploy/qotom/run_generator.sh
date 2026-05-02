#!/bin/bash

NIC_PCI_ADDR="0B:00.0"
#NIC_PCI_ADDR="08:00.0"

exec bin/bus_generator -l 1 \
    -a $NIC_PCI_ADDR \
    --huge-dir=/mnt/bus_gen/ \
    --file-prefix=bus_gen \
    -- $@

