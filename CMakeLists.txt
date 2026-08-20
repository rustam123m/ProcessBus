cmake_minimum_required(VERSION 3.10)
project(pbus LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_SOURCE_DIR}/cmake/hotpath.cmake)

# Platform selection (default: atom)
if (NOT DEFINED PLATFORM)
    set(PLATFORM atom)
endif()
include(${CMAKE_SOURCE_DIR}/cmake/platforms/${PLATFORM}.cmake)
message(STATUS "Building for platform: ${PLATFORM_NAME}")

# R-session crypto backend. mbedtls is the default so existing builds and CI
# are unchanged; openssl is opt-in and must fail loudly if libssl-dev is absent.
set(CRYPTO_BACKEND "mbedtls" CACHE STRING "R-session crypto backend: mbedtls or openssl")
set_property(CACHE CRYPTO_BACKEND PROPERTY STRINGS mbedtls openssl)
if (NOT CRYPTO_BACKEND STREQUAL "mbedtls" AND NOT CRYPTO_BACKEND STREQUAL "openssl")
    message(FATAL_ERROR "CRYPTO_BACKEND must be 'mbedtls' or 'openssl', got '${CRYPTO_BACKEND}'")
endif()
message(STATUS "R-session crypto backend: ${CRYPTO_BACKEND}")
find_package(PkgConfig REQUIRED)

# OpenSSL via pkg-config, not find_package. The aarch64 cross-build is Debian
# multiarch: libcrypto lives in /usr/lib/aarch64-linux-gnu and its headers in
# /usr/include, both outside the toolchain's CMAKE_FIND_ROOT_PATH, which is
# searched in ONLY mode. find_package therefore cannot see it, while
# pkg-config already resolves arm64 through PKG_CONFIG_LIBDIR. This is also how
# DPDK is found. Must stay at top level so the imported target is visible in src/.
if (CRYPTO_BACKEND STREQUAL "openssl")
    pkg_check_modules(LIBCRYPTO REQUIRED IMPORTED_TARGET libcrypto)
    message(STATUS "R-session crypto: OpenSSL ${LIBCRYPTO_VERSION}")
endif()

# Static DPDK via pkg-config
pkg_check_modules(DPDK_STATIC REQUIRED IMPORTED_TARGET libdpdk)

if (DPDK_STATIC_FOUND)
    # message(WARNING "DPDK libraries: ${DPDK_STATIC_LIBRARIES}")
    # message(WARNING "DPDK include dirs: ${DPDK_STATIC_INCLUDE_DIRS}")
    # message(WARNING "DPDK library dirs: ${DPDK_STATIC_LIBRARY_DIRS}")
    # message(WARNING "DPDK cflags: ${DPDK_STATIC_CFLAGS_OTHER}")
else()
    message(FATAL_ERROR "DPDK package not found!")
endif()
link_directories(${DPDK_STATIC_LIBRARY_DIRS})

# Platform-specific compiler flags. -fno-omit-frame-pointer for perf record -g.
#
# These must be set BEFORE add_subdirectory(3rdparty/): CMAKE_C_FLAGS is captured
# when a subdirectory is processed, so setting them afterwards left every
# third-party target — libiec61850, googletest and anything added later — built
# for a generic CPU while only our own sources got -mcpu/-march.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_ARCH_FLAGS} -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TARGET_ARCH_FLAGS} -fno-strict-aliasing -Wno-unused-result -Wno-volatile -fno-omit-frame-pointer")

# libiec61850: GOOSE/R-GOOSE/SV/R-SV
add_subdirectory(3rdparty/)

# DPDK's related functions
function(setup_dpdk TARGET_NAME)
    target_include_directories(${TARGET_NAME}
        PRIVATE
        ${DPDK_STATIC_INCLUDE_DIRS}
	)

    target_compile_options(${TARGET_NAME}
        PRIVATE
        ${DPDK_STATIC_CFLAGS}
    )

    target_link_libraries(${TARGET_NAME}
        PRIVATE
        -Wl,--whole-archive
        ${DPDK_STATIC_LIBRARIES}
        -lrte_mempool_ring
        -lrte_bus_pci
        -lrte_bus_vdev
        ${DPDK_PMD_LIBS}
        -Wl,--no-whole-archive
        pcap
        elf
        bpf
        xdp
        numa
    )
endfunction()

# Unit tests with Googletest
option(BUILD_TESTS "Build unit tests" ON)
if (BUILD_TESTS)
    add_subdirectory(3rdparty/googletest EXCLUDE_FROM_ALL)

    include_directories(
        3rdparty/googletest/googletest/include
        3rdparty/googletest/googlemock/include
        3rdparty/cxxopts/include
    )

    include(CTest)
    enable_testing()
endif()

# Apps
add_subdirectory(src/)

