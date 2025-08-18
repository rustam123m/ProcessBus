#!/bin/bash

HUGEPATH_SIZE=2048M
HUGEPAGE_DIR=/mnt/bus_proc
NIC_ADDR=0000:0C:00.0
QEMU_HARDDRIVE=vm.qcow2

qemu-system-x86_64 \
    -enable-kvm \
    -m 2048 \
    -smp 2 \
    -cpu Nehalem,+sse3 \
    -machine q35 \
    -hda ${QEMU_HARDDRIVE} \
    -device intel-iommu,intremap=off,caching-mode=on \
    -object memory-backend-file,id=mem,size=$HUGEPATH_SIZE,mem-path=$HUGEPAGE_DIR,share=on \
    -numa node,memdev=mem \
    -nographic \
    -serial mon:stdio \
    -device vfio-pci,host=$NIC_ADDR,rombar=0

