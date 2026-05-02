#!/bin/bash

NIC_PCI_ADDR="0000:00:05.0"

exec bin/bus_processor -l 2          \
    -a $NIC_PCI_ADDR            \
    --huge-dir=/mnt/bus_proc/   \
    --file-prefix=bus_proc      \
    -- $@

