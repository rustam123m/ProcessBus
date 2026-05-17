#!/bin/bash
# Building generator and processor for ProcessBus

set -e          # Exit on error
set -o pipefail # Don't let a successful tail/grep mask an upstream failure

SCRIPT_PATH="$(dirname "$(realpath "$0")")"
REPO_DIR="$SCRIPT_PATH/../"
DPDK_DIR="$REPO_DIR/3rdparty/dpdk/"
# DPDK_BUILD / INSTALL / PKGCONFIG are per-platform — set in configure_platform().

PLATFORM=atom
OPT_UPDATE_SRC=1
OPT_BUILD_DPDK=1
OPT_BUILD_PBUS=1
OPT_REBUILD=0
ACTION=""

# Per-platform settings
configure_platform() {
    DPDK_CROSS_FILE=""
    CMAKE_TOOLCHAIN=""
    DOCKERFILE="$SCRIPT_PATH/Dockerfile.debian"
    BUILDER_IMAGE="pbus_builder"
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
        orangepi3b)
            TARGET_PROCESSOR=generic   # overridden by --cross-file
            DPDK_DRIVERS="net_igc,net_af_packet,net_tap,net_ring"
            BUILD_DIR="$REPO_DIR/build-orangepi3b/"
            INSTALL_DIR="$REPO_DIR/install-orangepi3b/"
            DPDK_CROSS_FILE="$REPO_DIR/cmake/platforms/orangepi3b-dpdk.cross"
            CMAKE_TOOLCHAIN="$REPO_DIR/cmake/platforms/orangepi3b-toolchain.cmake"
            DOCKERFILE="$SCRIPT_PATH/Dockerfile.debian-arm64"
            BUILDER_IMAGE="pbus_builder_arm64"
            ;;
        *)
            echo "Unknown platform: $PLATFORM"
            exit 1
            ;;
    esac

    # Per-platform DPDK build/install — keeps x86 and arm64 outputs separate.
    DPDK_BUILD="$DPDK_DIR/build-$PLATFORM/"
    DPDK_INSTALL="$DPDK_BUILD/install/"
    DPDK_PKGCONFIG="$DPDK_INSTALL/lib/pkgconfig/"
}

function usage()
{
    echo "Usage: $0 [--platform=atom/qemu/orangepi3b/all] [--update=0/1] [--dpdk=0/1] [--pbus=0/1] [--rebuild] [--clean] [--check] [--setup] [--shell]"
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

    # Patches grouped per submodule under ci/patches/<name>/.
    apply_patches "$REPO_DIR/3rdparty/libiec61850" "$SCRIPT_PATH/patches/libiec61850"
    apply_patches "$REPO_DIR/3rdparty/dpdk"        "$SCRIPT_PATH/patches/dpdk"
}

function apply_patches()
{
    local target_dir="$1"
    local patch_dir="$2"
    [ -d "$patch_dir" ] || return 0
    for patch in "$patch_dir"/*.patch; do
        [ -f "$patch" ] || continue
        if git -C "$target_dir" apply --check "$patch" 2>/dev/null; then
            echo "Applying $(basename $patch) to $(basename $target_dir)"
            git -C "$target_dir" apply "$patch"
        else
            echo "Already applied: $(basename $patch)"
        fi
    done
}

function build_dpdk()
{
    cd "$DPDK_DIR"
    rm -rf "$DPDK_BUILD"

    local meson_args=(
        --prefix="$DPDK_INSTALL"
        -Dlibdir=lib
        -Ddefault_library=static
        -Dbuildtype=release
        -Dmax_numa_nodes=1
        -Ddisable_drivers=all
        -Denable_drivers="$DPDK_DRIVERS"
    )
    if [[ -n "$DPDK_CROSS_FILE" ]]; then
        # Cross-build: cross file owns -Dmachine and toolchain.
        meson_args+=(--cross-file="$DPDK_CROSS_FILE")
    else
        meson_args+=(-Dmachine="$TARGET_PROCESSOR")
    fi
    # RK3566 PCIe is not cache-coherent; patched igc PMD emits dc cvac/civac.
    if [[ "$PLATFORM" == "orangepi3b" ]]; then
        meson_args+=(-Dnoncoherent_dma=true)
    fi

    meson setup "$DPDK_BUILD" "${meson_args[@]}"

    # Install
    ninja -C "$DPDK_BUILD"
    ninja -C "$DPDK_BUILD" install

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

    local cmake_args=(
        -S ./
        -B "$BUILD_DIR"
        -DPLATFORM="$PLATFORM"
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
        -DBUILD_TESTS=ON
        -DBUILD_SAMPLES=OFF
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O3 -g -DNDEBUG"
        -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O3 -g -DNDEBUG"
    )
    if [[ -n "$CMAKE_TOOLCHAIN" ]]; then
        cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN")
        # When cross-building, also disable tests by default — they target host arch.
        cmake_args+=(-DBUILD_TESTS=OFF)
    fi

    cmake "${cmake_args[@]}"

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
        --clean|--check|--setup|--shell)
            ACTION="${1#--}"
            ;;
        *) usage;;
    esac
    shift
done

# Dispatch deferred actions after parsing all options (so --platform= is honoured).
case "$ACTION" in
    clean)
        configure_platform
        echo "Cleaning build artifacts for platform: $PLATFORM"
        rm -rf "$BUILD_DIR" "$INSTALL_DIR" "$DPDK_BUILD"
        echo "Re-initializing submodules..."
        git submodule deinit -f --all
        git submodule update --init --recursive
        echo "Clean done"
        exit 0
        ;;
    check)
        configure_platform
        check_code
        exit 0
        ;;
    setup)
        configure_platform
        podman build -f "$DOCKERFILE" --tag "$BUILDER_IMAGE" "$SCRIPT_PATH/.."
        exit 0
        ;;
    shell)
        configure_platform
        podman run -it --rm --cap-add=NET_RAW \
            -v "$(realpath "$REPO_DIR"):/ProcessBus/:Z" \
            --userns=keep-id --name "$BUILDER_IMAGE" "$BUILDER_IMAGE" /bin/bash
        exit 0
        ;;
esac

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

