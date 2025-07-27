#!/bin/bash

# Variable to store all device mappings
DEVICE_MAPPINGS="--device=/dev/vfio"

# Loop through each device in /dev/vfio and add to DEVICE_MAPPINGS
for vfio_device in /dev/vfio/*; do
    DEVICE_MAPPINGS+=" --device=$vfio_device"
done

# Docker / podman
podman run --rm -it \
    --privileged \
    --cap-add=ALL \
    $DEVICE_MAPPINGS \
    -v /dev:/dev \
    -v /dev/hugepages:/dev/hugepages \
    -v /sys/bus/pci:/sys/bus/pci \
    -v /sys/devices:/sys/devices \
    -v /lib/modules:/lib/modules \
    -v /mnt/bus_gen:/mnt/bus_gen \
    -v /mnt/bus_proc:/mnt/bus_proc \
    -v $(pwd):/app \
    --entrypoint /bin/bash \
    runner_img

