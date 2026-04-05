# Using QEMU for running DPDK apps

## Building apps with a Docker container

1. Building a Docker image with podman/docker:

    **podman build -f ci/Dockerfile.debian --tag pbus_builder**

2. Running the container

    **podman run -it --rm -v ~/ProcessBus/:/ProcessBus/:Z --userns=keep-id --name pbus_builder pbus_builder /bin/bash**

3. Inside the continer all apps are built with **ci/build.sh**

    - cd /ProcessBus/ci/
    - build.sh 

4. Deploy to a QEMU VM(it uploads all binaries and scripts to run into **/home/user/pbus/**):

    /ProcessBus/deploy/qemu/deploy.sh 

## Requirements for QEMU environment

1. Ubuntu Linux 24.04
2. CPU with more than 4 cores, 4GB RAM and 4GB space on a harddrive

## Create QEMU VM with Debian 

**There is a special deploy/qemu/Makefile with targets:**

1. **fetch** - installs QEMU packages and to download the Debian iso image.
2. **hard** - create a harddrive for the VM with QEMU.
3. **interfaces** - create 4 TAP interfaces on the host system: vm_tapX

    vm_tap0 is assigned the IP: **192.168.12.1/24** and is used for SSH connections

4. **install** - runs the QEMU VM with the prepared harddrive and iso image with Debian.

    A developer must install Debian **manually**.

    The user **user** is needed to deploy software. The IP in the VM must be **192.168.12.2/24** on the first interface.

5. **run** - runs QEMU VM with installed Debian and attached TAP interfaces

## Running packet generator or processor

1. make run # runs Debian VM using deploy/qemu/Makefile
2. Inside the VM, some preparations are needed:
    - Detach NIC interfaces from the Linux kernel: **deploy/qemu/scripts/setup_dpdk_env.sh**
    - Tune RT with a special script: **deploy/tune_linux.sh**
    - Run the generator with script: **deploy/qemu/scripts/run_generator.sh**
    - Run the processor with script: **deploy/qemu/scripts/run_processor.sh**

