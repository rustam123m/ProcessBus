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
HOST=192.168.13.3
REMOTE_DIR=/home/user/

# Rebuild
$REPO_DIR/ci/build.sh --rebuild

# Create a folder with all files
rm -rf $PBUS_FILES
mkdir $PBUS_FILES
# Collect
DIRECTORIES=(
    "$REPO_DIR/install/*"
    "$REPO_DIR/devices/common/*"
    "$REPO_DIR/devices/qotom_server/scripts/*"
)
for dir in "${DIRECTORIES[@]}"; do
    cp -rp $dir $PBUS_FILES
done

# Send all files to VM
sshpass -p 123 scp -o StrictHostKeyChecking=no -rp "$PBUS_FILES/" "${USER}@${HOST}:${REMOTE_DIR}"

