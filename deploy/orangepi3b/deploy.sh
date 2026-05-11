#!/bin/bash
# Deploy ProcessBus to the OrangePi 3B target. Pushes files; runs
# setup_platform.sh on the first deploy after each boot.

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
REPO_DIR="$SCRIPT_DIR/../.."
INSTALL_DIR="$REPO_DIR/install-orangepi3b"
COMMON_DIR="$REPO_DIR/deploy/common"
UDMABUF_KO="$REPO_DIR/3rdparty/udmabuf/u-dma-buf.ko"
DPDK_DEVBIND="$REPO_DIR/3rdparty/dpdk/usertools/dpdk-devbind.py"
BUNDLE="$SCRIPT_DIR/pbus"
SENTINEL="/var/run/pbus_rt"

HOST="${HOST:-192.168.100.105}"
USER="${USER_:-jarvis}"
REMOTE_DIR="/home/jarvis/pbus"

DO_REBUILD=0

usage() {
    cat <<EOF
Usage: $0 [--rebuild] [--host <addr>]

  --rebuild   Run ci/build.sh --platform=orangepi3b --rebuild before pushing
  --host      Override target host (default: $HOST, env HOST=)
  -h, --help  Show this help
EOF
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --rebuild)  DO_REBUILD=1 ;;
        --host)     HOST="$2"; shift ;;
        -h|--help)  usage; exit 0 ;;
        *)          echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

SSH="ssh -o StrictHostKeyChecking=no $USER@$HOST"
SCP="scp -o StrictHostKeyChecking=no"

# --- 1. Optional rebuild (apps + DPDK if needed) ---
if [ "$DO_REBUILD" -eq 1 ]; then
    echo "=== Rebuilding for orangepi3b ==="
    "$REPO_DIR/ci/build.sh" --platform=orangepi3b --rebuild
fi

# --- 2. Bundle artifacts ---
if [ ! -d "$INSTALL_DIR" ]; then
    echo "ERROR: $INSTALL_DIR missing — build first (--rebuild or ci/build.sh)" >&2
    exit 1
fi
if [ ! -f "$UDMABUF_KO" ]; then
    echo "ERROR: $UDMABUF_KO missing — see README §1.5 (cross-build u-dma-buf)" >&2
    exit 1
fi

echo "=== Bundling for $USER@$HOST:$REMOTE_DIR ==="
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
cp -rp "$INSTALL_DIR/"*           "$BUNDLE/"
cp -p  "$UDMABUF_KO"              "$BUNDLE/"
cp -p  "$DPDK_DEVBIND"            "$BUNDLE/"
cp -p  "$SCRIPT_DIR/setup_platform.sh" "$BUNDLE/"
cp -p  "$SCRIPT_DIR/run_"*.sh          "$BUNDLE/"
[ -f "$COMMON_DIR/target_runner.sh" ] && cp -p "$COMMON_DIR/target_runner.sh" "$BUNDLE/" || true

# --- 3. Push ---
$SSH "mkdir -p $REMOTE_DIR"
$SCP -rp "$BUNDLE/"* "$USER@$HOST:$REMOTE_DIR/"

# --- 4. Run setup_platform.sh once per boot ---
if $SSH "test -e $SENTINEL" 2>/dev/null; then
    echo "=== Setup already applied ($SENTINEL present) — skipping ==="
else
    echo "=== Applying platform setup (modules, hugepages, NIC bind) ==="
    $SSH "cd $REMOTE_DIR && sudo bash setup_platform.sh"
fi

echo "=== Done ==="
