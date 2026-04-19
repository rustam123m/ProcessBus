# ARM64 RockChip (placeholder)

set(PLATFORM_NAME arm64)
set(TARGET_ARCH_FLAGS "-march=armv8-a")
set(DPDK_PMD_LIBS
    -lrte_net_af_xdp
    -lrte_net_af_packet
    -lrte_net_tap
    -lrte_net_ring
)

add_compile_definitions(PLATFORM_ARM64)
