#!/bin/bash
# Building generator and processor for ProcessBus

set -e # Exit on error

SCRIPT_PATH="$(dirname "$(realpath "$0")")"
REPO_DIR="$SCRIPT_PATH/../"
DPDK_DIR="$REPO_DIR/3rdparty/dpdk/"
DPDK_INSTALL="$DPDK_DIR/build/install/"
DPDK_PKGCONFIG="$DPDK_INSTALL/lib/pkgconfig/"

PLATFORM=atom
OPT_UPDATE_SRC=1
OPT_BUILD_DPDK=1
OPT_BUILD_PBUS=1
OPT_REBUILD=0

# Per-platform settings
configure_platform() {
    case "$PLATFORM" in
        atom)
            TARGET_PROCESSOR=atom
            DPDK_DRIVERS="net_e1000,net_igc,net_ixgbe,net_i40e,net_af_xdp,net_af_packet,net_tap,net_virtio,net_ring,net_vhost"
            BUILD_DIR="$REPO_DIR/build/"
            INSTALL_DIR="$REPO_DIR/install/"
            ;;
        qemu)
            TARGET_PROCESSOR=generic
            DPDK_DRIVERS="net_virtio,net_tap,net_ring"
            BUILD_DIR="$REPO_DIR/build-qemu/"
            INSTALL_DIR="$REPO_DIR/install-qemu/"
            ;;
        arm64)
            TARGET_PROCESSOR=generic
            DPDK_DRIVERS="net_af_xdp,net_af_packet,net_tap,net_ring"
            BUILD_DIR="$REPO_DIR/build-arm64/"
            INSTALL_DIR="$REPO_DIR/install-arm64/"
            ;;
        *)
            echo "Unknown platform: $PLATFORM"
            exit 1
            ;;
    esac
}

function usage()
{
    echo "Usage: $0 [--platform=atom/qemu/arm64/all] [--update=0/1] [--dpdk=0/1] [--pbus=0/1] [--rebuild] [--clean] [--check] [--setup] [--shell]"
    exit 1
}

function prepare_sources()
{
    # Ensure submodules are updated
    git submodule update --init --recursive

    # Download mbedtls for libiec61850
    MBEDTLS_DIR="$REPO_DIR/3rdparty/libiec61850/third_party/mbedtls/"
    wget -q -O "$MBEDTLS_DIR/mbedtls.tar.gz" https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.0.tar.gz
    tar xzf "$MBEDTLS_DIR/mbedtls.tar.gz" -C "$MBEDTLS_DIR"
    rm "$MBEDTLS_DIR/mbedtls.tar.gz"

    # Apply patches to libiec61850
    local lib_dir="$REPO_DIR/3rdparty/libiec61850"
    local patch_dir="$SCRIPT_PATH/patches"
    if [ -d "$patch_dir" ]; then
        for patch in "$patch_dir"/*.patch; do
            [ -f "$patch" ] || continue
            if git -C "$lib_dir" apply --check "$patch" 2>/dev/null; then
                echo "Applying: $(basename $patch)"
                git -C "$lib_dir" apply "$patch"
            else
                echo "Already applied: $(basename $patch)"
            fi
        done
    fi
}

function build_dpdk()
{
    cd $DPDK_DIR
    rm -rf build/

    # Building DPDK with platform-specific settings
    meson setup build \
        --prefix="$DPDK_DIR/build/install/" \
        -Dlibdir=lib \
        -Dmachine=$TARGET_PROCESSOR \
        -Ddefault_library=static \
        -Dbuildtype=release \
        -Dmax_numa_nodes=1 \
        -Ddisable_drivers=all \
        -Denable_drivers=$DPDK_DRIVERS

    # Install
    ninja -C build
    ninja -C build install

    # Hack to force static linking
    rm -rf "$DPDK_INSTALL"/lib/*.so
    rm -rf "$DPDK_INSTALL"/lib/*.so.*

    # Check it
    export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$DPDK_PKGCONFIG"
    if ! pkg-config --list-all | grep dpdk > /dev/null ; then
        echo "Can't find static libs for DPDK!"
        exit 1
    fi
}

function rebuild_and_install()
{
    cmake --build "$BUILD_DIR"
    cmake --install "$BUILD_DIR"

    # DPDK's stuff
    cp "$REPO_DIR"/3rdparty/dpdk/usertools/dpdk-devbind.py "$INSTALL_DIR"
}

function build_apps()
{
    cd "$REPO_DIR"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"

    export PKG_CONFIG_PATH="$DPDK_PKGCONFIG:$PKG_CONFIG_PATH"

    cmake -S ./ -B "$BUILD_DIR" \
        -DPLATFORM=$PLATFORM \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=ON \
        -DBUILD_SAMPLES=OFF \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_CXX_FLAGS_RELEASE="-O3" \
        -DCMAKE_C_FLAGS_RELEASE="-O3"

    rebuild_and_install
}

function check_code()
{
    mkdir -p $INSTALL_DIR

    echo "Check the source code with CppCheck....."
    cppcheck --enable=all --std=c++20 \
            --suppress=missingIncludeSystem --suppress=missingInclude \
            --check-level=exhaustive \
            --suppress=cstyleCast \
            --inconclusive --force --quiet \
            -i3rdparty -ibuild -iplayground \
            -I$REPO_DIR/src/ \
            --output-file=$INSTALL_DIR/cppcheck_results.txt \
            $REPO_DIR
    echo "Done"

    echo "Check the source code with clang-tidy....."
    find $REPO_DIR/src/ -name '*.cpp' -or -name '*.hpp' > $BUILD_DIR/tidy_files.txt
    clang-tidy -p $BUILD_DIR -quiet \
        $(< $BUILD_DIR/tidy_files.txt) \
        > $BUILD_DIR/clang-tidy.log 2>&1
    echo "Done"
}

# Options
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --platform=*)
            PLATFORM="${1#*=}"
            ;;
        --platform)
            PLATFORM="$2"
            shift
            ;;
        --update=*)
            OPT_UPDATE_SRC="${1#*=}"
            ;;
        --dpdk=*)
            OPT_BUILD_DPDK="${1#*=}"
            ;;
        --pbus=*)
            OPT_BUILD_PBUS="${1#*=}"
            ;;
        --rebuild)
            OPT_UPDATE_SRC=0
            OPT_BUILD_DPDK=0
            OPT_BUILD_PBUS=0
            OPT_REBUILD=1
            ;;
        --clean)
            echo "Cleaning build artifacts..."
            rm -rf "$BUILD_DIR" "$INSTALL_DIR" "$DPDK_DIR/build/"
            echo "Re-initializing submodules..."
            git submodule deinit -f --all
            git submodule update --init --recursive
            echo "Clean done"
            exit 0
            ;;
        --check)
            check_code
            exit 0
            ;;
        --setup)
            podman build -f "$SCRIPT_PATH/Dockerfile.debian" --tag pbus_builder
            exit 0
            ;;
        --shell)
            podman run -it --rm --cap-add=NET_RAW \
                -v "$(realpath "$REPO_DIR"):/ProcessBus/:Z" \
                --userns=keep-id --name pbus_builder pbus_builder /bin/bash
            exit 0
            ;;
        *) usage;;
    esac
    shift
done

ALL_PLATFORMS="atom qemu"

# Source update runs once regardless of platform
if [[ "$OPT_UPDATE_SRC" -eq 1 ]]; then
    echo "Performing source update..."
    prepare_sources
fi

build_for_platform() {
    configure_platform
    echo "=== Platform: $PLATFORM ==="

    if [[ "$OPT_BUILD_DPDK" -eq 1 ]]; then
        echo "Building DPDK..."
        build_dpdk
    fi

    if [[ "$OPT_BUILD_PBUS" -eq 1 ]]; then
        echo "Building PBUS..."
        build_apps
    fi

    if [[ "$OPT_REBUILD" -eq 1 ]]; then
        echo "Rebuild apps without cleaning..."
        rebuild_and_install
    fi
}

if [[ "$PLATFORM" == "all" ]]; then
    if [[ "$OPT_BUILD_DPDK" -eq 1 ]]; then
        echo "Error: --platform=all cannot build DPDK (shared build dir). Build DPDK per-platform."
        exit 1
    fi
    for PLATFORM in $ALL_PLATFORMS; do
        build_for_platform
    done
else
    build_for_platform
fi

