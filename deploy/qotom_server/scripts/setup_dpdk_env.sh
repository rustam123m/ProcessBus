#!/bin/bash
# ./run_generator.sh --goose 10,100000
# ./run_processor.sh --goose 10
#

echo 4096 > /proc/sys/vm/nr_hugepages

mount_points=(/mnt/bus_gen /mnt/bus_proc /mnt/delay_meter /mnt/redirect1 /mnt/redirect2)

# Create directories and mount each
for mount_point in "${mount_points[@]}"; do
    mkdir -p "$mount_point"
    mount -t hugetlbfs nodev "$mount_point"
done

modprobe vfio
modprobe vfio-pci

cd /home/jarvis/pbus/
./dpdk-devbind.py --bind=vfio-pci 0000:0c:00.0 0000:0b:00.0 0000:04:00.0 0000:05:00.0 0000:06:00.0 0000:07:00.0 0000:08:00.0
./dpdk-devbind.py --status

