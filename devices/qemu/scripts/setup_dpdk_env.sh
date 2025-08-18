#!/bin/sh
# ./run_generator.sh --goose 10,100000
# ./run_processor.sh --goose 10
#

# 1024 * 2MB
echo 1024 > /proc/sys/vm/nr_hugepages

mount_points=(/mnt/bus_gen /mnt/bus_proc /mnt/delay_meter /mnt/redirect1 /mnt/redirect2)

# Create directories and mount each
for mount_point in "${mount_points[@]}"; do
    mkdir -p "$mount_point"
    mount -t hugetlbfs nodev "$mount_point"
done

modprobe uio
modprobe uio_pci_generic

cd /home/user/pbus/
./dpdk-devbind.py --bind=uio_pci_generic 0000:00:04.0 0000:00:05.0
./dpdk-devbind.py --status

