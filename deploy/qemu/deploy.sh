#!/bin/bash
# ssh-keygen -t rsa -b 4096 -C "deploy@test.com"
# ssh-copy-id "${USER}@${HOST}"
# scp -r "$INSTALL_DIR/bin/" "${USER}@${HOST}:${REMOTE_DIR}"

set -e

SCRIPT_PATH="$(dirname "$(realpath "$0")")"
REPO_DIR="$SCRIPT_PATH/../../"
BUILD_DIR="$REPO_DIR/build/"
INSTALL_DIR="$REPO_DIR/install/"
PBUS_FILES="$SCRIPT_PATH/pbus/"

USER=user
HOST=192.168.21.2
REMOTE_DIR=/home/user/

# Rebuild (skip with --no-build)
if [ "${1}" != "--no-build" ]; then
    $REPO_DIR/ci/build.sh --rebuild
fi

# Create a folder with all files
rm -rf $PBUS_FILES
mkdir $PBUS_FILES
# Collect
DIRECTORIES=(
    "$REPO_DIR/install/*"
    "$REPO_DIR/deploy/common/tune_linux.sh"
    "$REPO_DIR/deploy/common/hw_test_runner.sh"
    "$REPO_DIR/deploy/qemu/scripts/*"
)
for dir in "${DIRECTORIES[@]}"; do
    cp -rp $dir $PBUS_FILES
done

# Send all files to VM (requires ssh key: ssh-copy-id user@host)
scp -o StrictHostKeyChecking=no -rp "$PBUS_FILES/" "${USER}@${HOST}:${REMOTE_DIR}"

