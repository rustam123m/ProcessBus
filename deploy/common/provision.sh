#!/bin/bash
# Provision a fresh Debian target device for ProcessBus.
# Run as root on the target machine:
#   curl/scp this script to the device, then:
#   sudo bash provision.sh [--user jarvis] [--pubkey "ssh-ed25519 AAAA..."]
#
# What it does:
#   1. Creates the user with passwordless sudo
#   2. Installs DPDK runtime dependencies
#   3. Sets up SSH key authentication
#   4. Loads required kernel modules

set -e

USERNAME=jarvis
PUBKEY=""

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --user)    USERNAME="$2"; shift ;;
        --pubkey)  PUBKEY="$2"; shift ;;
        *)         echo "Usage: $0 [--user NAME] [--pubkey \"ssh-...\"]"; exit 1 ;;
    esac
    shift
done

echo "=== Provisioning target device ==="
echo "User: $USERNAME"

# 1. Create user with passwordless sudo
if id "$USERNAME" &>/dev/null; then
    echo "User $USERNAME already exists, skipping creation"
else
    useradd -m -s /bin/bash "$USERNAME"
    echo "Created user: $USERNAME"
fi

# Passwordless sudo for DPDK operations (hugepages, vfio, RT priorities)
echo "$USERNAME ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/$USERNAME
chmod 440 /etc/sudoers.d/$USERNAME
echo "Configured passwordless sudo"

# 2. Install runtime dependencies
apt-get update
apt-get install -y --no-install-recommends \
    pciutils libatomic1 elfutils \
    libbpf1 libpcap0.8 libxdp1 libnuma1 \
    openssh-server psmisc \
    kmod iproute2
apt-get clean
rm -rf /var/lib/apt/lists/*
echo "Installed runtime dependencies"

# 3. SSH key setup
USER_HOME="/home/$USERNAME"
SSH_DIR="$USER_HOME/.ssh"
mkdir -p "$SSH_DIR"
chmod 700 "$SSH_DIR"

if [ -n "$PUBKEY" ]; then
    echo "$PUBKEY" >> "$SSH_DIR/authorized_keys"
    chmod 600 "$SSH_DIR/authorized_keys"
    echo "Added SSH public key"
else
    echo "No --pubkey provided. Add manually:"
    echo "  ssh-copy-id $USERNAME@<this-host>"
fi

chown -R "$USERNAME:$USERNAME" "$SSH_DIR"

# Ensure sshd allows key auth
sed -i 's/^#*PubkeyAuthentication.*/PubkeyAuthentication yes/' /etc/ssh/sshd_config
systemctl restart sshd 2>/dev/null || true

# 4. Hugepages — reserve on boot (2MB pages)
HUGEPAGES_CONF="/etc/sysctl.d/90-hugepages.conf"
echo "vm.nr_hugepages = 4096" > "$HUGEPAGES_CONF"
sysctl -p "$HUGEPAGES_CONF"
echo "Reserved 4096 hugepages (8 GB)"

echo ""
echo "=== Done ==="
echo "From your dev machine:"
echo "  ssh-copy-id $USERNAME@\$(hostname -I | awk '{print \$1}')"
echo "  deploy/qotom_server/deploy.sh"
