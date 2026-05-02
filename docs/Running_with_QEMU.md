# Using QEMU for running DPDK apps

## Building apps with a Docker container

1. Building a Docker image with podman/docker:

    **podman build -f ci/Dockerfile.debian --tag pbus_builder**

    Or: `./ci/build.sh --setup`

2. Running the container

    **podman run -it --rm -v ~/ProcessBus/:/ProcessBus/:Z --userns=keep-id --name pbus_builder pbus_builder /bin/bash**

    Or: `./ci/build.sh --shell`

3. Inside the container all apps are built with **ci/build.sh**

    - cd /ProcessBus/ci/
    - ./build.sh

4. Deploy to a QEMU VM (uploads all binaries and scripts to **/home/user/pbus/**):

    ./deploy/qemu/deploy.sh

## Requirements for QEMU environment

1. Ubuntu Linux 24.04 (or similar with KVM support)
2. CPU with more than 4 cores, 4GB RAM and 4GB free disk space

## Create QEMU VM with Debian

**There is a special `deploy/qemu/Makefile` with targets:**

1. **fetch** — installs QEMU packages and downloads the Debian ISO image.
2. **hard** — creates a 4GB QCOW2 harddrive for the VM.
3. **interfaces** — creates 4 TAP interfaces on the host system: `vm_tap0..3`

    `vm_tap0` is assigned the IP: **192.168.21.1/24** and is used for SSH connections.

4. **install** — boots the QEMU VM with the ISO for manual Debian installation.
5. **run** — boots the installed VM with all 4 TAP interfaces attached.

## Installing Debian in the VM

During `make install`, install Debian manually with the following settings:

- Create user **user** with password **123**
- Use the first network interface for connectivity

### Post-install setup (inside the VM)

After first boot with `make run`, configure the VM:

1. **Configure apt sources** (`/etc/apt/sources.list`):

    Comment out the `cdrom:` line and add:
    ```
    deb http://deb.debian.org/debian trixie main contrib non-free non-free-firmware
    deb http://deb.debian.org/debian trixie-updates main contrib non-free non-free-firmware
    deb http://security.debian.org/debian-security trixie-security main contrib non-free non-free-firmware
    ```

2. **Configure DNS** (needed before apt can resolve hostnames):

    ```bash
    echo "nameserver 8.8.8.8" > /etc/resolv.conf
    ```

3. **Configure static IP on the first interface** (`/etc/network/interfaces`):

    ```
    auto enp0s2
    iface enp0s2 inet static
        address 192.168.21.2
        netmask 255.255.255.0
        gateway 192.168.21.1
        dns-nameservers 8.8.8.8
    ```

    Then restart networking: `sudo systemctl restart networking`

3. **Enable internet access from the host** (so the VM can `apt install`):

    ```bash
    # On the host machine (from deploy/qemu/)
    make internet
    ```

    This enables IP forwarding and sets up NAT masquerade through `wlp3s0`.
    If your host uses a different interface for internet, edit the Makefile.
    Rules are lost on reboot — run again after restart.

4. **Install required packages:**

    ```bash
    apt update
    apt install -y sudo openssh-server rsync mc \
        pciutils libatomic1 elfutils \
        libbpf1 libpcap0.8 libxdp1 libnuma1
    ```

    The first line is system tools, the second is DPDK runtime dependencies.

5. **Enable sudo for user:**

    ```bash
    usermod -aG sudo user
    echo "user ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/user
    ```

6. **Configure SSH** (edit `/etc/ssh/sshd_config`):

    ```
    PermitRootLogin yes
    PasswordAuthentication yes
    ```

    Then: `systemctl restart sshd`

7. **Set up SSH key access from the host:**

    Generate an SSH key pair on the host (if you don't have one). This creates
    a private key (`~/.ssh/id_ed25519`) and a public key (`~/.ssh/id_ed25519.pub`):
    ```bash
    ssh-keygen -t ed25519 -N ""
    ```

    Copy the public key to the VM. This appends it to `~/.ssh/authorized_keys`
    on the VM so future SSH connections don't require a password:
    ```bash
    ssh-copy-id user@192.168.21.2
    ```

    Verify password-free access:
    ```bash
    ssh user@192.168.21.2
    ```

8. **Allocate hugepages** (add to `/etc/default/grub`):

    ```
    GRUB_CMDLINE_LINUX="default_hugepagesz=2M hugepagesz=2M hugepages=1024 intel_iommu=on iommu=pt"
    ```

    Then: `sudo update-grub && sudo reboot`

## Connecting generator and processor NICs

The generator sends packets on one virtual NIC (`vm_tap1`) and the processor
receives on another (`vm_tap2`). To connect them (like a loopback cable), create
a bridge on the host:

```bash
# From deploy/qemu/
make bridge
```

This creates a Linux bridge `br_pbus` linking `vm_tap1` and `vm_tap2`.
Run this after `make interfaces` and before testing.

## Running packet generator or processor

1. `make run` — boots the VM using `deploy/qemu/Makefile`
2. From the dev machine, push files and apply hugepages + NIC bind:
    - `./deploy/qemu/deploy.sh`
3. Inside the VM (manual run, optional):
    - `./run_generator.sh --goose 10,1000`
    - `./run_processor.sh --goose 10`

## Automated testing

From the dev machine (VM must be running and set up):

```bash
# Push files + run a single scenario (auto-tunes if needed)
./deploy/qemu/deploy.sh --scenario goose_10x10

# Run all QEMU scenarios
./deploy/qemu/deploy.sh --all
```

Results are saved to `deploy/qemu/results/<date>/<scenario>/`.

See `deploy/qemu/README.md` for the full onboarding flow.
