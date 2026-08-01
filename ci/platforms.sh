#!/bin/bash
# Per-platform build settings, sourced by ci/build.sh and ci/build_internal.sh.

ALL_PLATFORMS="atom qemu"

function configure_platform()
{
    DPDK_CROSS_FILE=""
    CMAKE_TOOLCHAIN=""
    DOCKERFILE="$CI_DIR/Dockerfile.debian"
    BUILDER_IMAGE="pbus_builder"
    case "$PLATFORM" in
        atom)
            TARGET_PROCESSOR=atom
            DPDK_DRIVERS="net_e1000,net_igc,net_ixgbe,net_i40e,net_af_xdp,net_af_packet,net_tap,net_virtio,net_ring,net_vhost"
            BUILD_DIR="$REPO_DIR/build-atom/"
            INSTALL_DIR="$REPO_DIR/install-atom/"
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
            DOCKERFILE="$CI_DIR/Dockerfile.debian-arm64"
            BUILDER_IMAGE="pbus_builder_arm64"
            ;;
        *)
            echo "Unknown platform: $PLATFORM" >&2
            exit 1
            ;;
    esac

    # Per-platform DPDK build/install — keeps x86 and arm64 outputs separate.
    DPDK_BUILD="$REPO_DIR/3rdparty/dpdk/build-$PLATFORM/"
    DPDK_INSTALL="$DPDK_BUILD/install/"
    DPDK_PKGCONFIG="$DPDK_INSTALL/lib/pkgconfig/"
}
