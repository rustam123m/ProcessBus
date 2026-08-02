#!/bin/bash
# Building generator and processor for ProcessBus — CONTAINER SIDE.
#
# This script assumes it is already running inside a builder container and does
# no podman/docker work of its own. Two callers reach it:
#
#   ci/build.sh                   starts the right container, then runs this
#   .github/workflows/*.yml       already inside its own container, runs this
#                                 directly — going through build.sh would nest
#                                 a container inside a container

set -e          # Exit on error
set -o pipefail # Don't let a successful tail/grep mask an upstream failure

CI_DIR="$(dirname "$(realpath "$0")")"
REPO_DIR="$CI_DIR/../"
DPDK_DIR="$REPO_DIR/3rdparty/dpdk/"

# BUILD_DIR / INSTALL_DIR / DPDK_* and the toolchain are per-platform.
source "$CI_DIR/platforms.sh"

PLATFORM=atom
OPT_UPDATE_SRC=1
OPT_BUILD_DPDK=1
OPT_BUILD_PBUS=1
OPT_REBUILD=0
OPT_RUN_TESTS=0
ACTION=""

function usage()
{
    echo "Usage: $0 [--platform=atom/qemu/orangepi3b/all] [--update=0/1] [--dpdk=0/1] [--pbus=0/1] [--rebuild] [--test] [--clean] [--check]"
    echo ""
    echo "Container-side script. For --setup and --shell, use ci/build.sh."
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
    apply_patches "$REPO_DIR/3rdparty/libiec61850" "$CI_DIR/patches/libiec61850"
    apply_patches "$REPO_DIR/3rdparty/dpdk"        "$CI_DIR/patches/dpdk"
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

function run_tests()
{
    if [[ -n "$CMAKE_TOOLCHAIN" ]]; then
        echo "Skipping tests: $PLATFORM is a cross-build"
        return 0
    fi

    "$BUILD_DIR/src/tests/unit_tests"
    "$BUILD_DIR/src/tests/integration_tests"
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
        --test)
            OPT_UPDATE_SRC=0
            OPT_BUILD_DPDK=0
            OPT_BUILD_PBUS=0
            OPT_RUN_TESTS=1
            ;;
        --clean|--check)
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
esac

# Source update runs once regardless of platform
if [[ "$OPT_UPDATE_SRC" -eq 1 ]]; then
    echo "Performing source update..."
    prepare_sources
fi

function build_for_platform()
{
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

    if [[ "$OPT_RUN_TESTS" -eq 1 ]]; then
        echo "Running tests..."
        run_tests
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
