# Orange Pi 3B (Rockchip RK3566 / Cortex-A55) — ARM64 functional bring-up

set(PLATFORM_NAME "orangepi3b (Cortex-A55)")
set(TARGET_ARCH_FLAGS "-mcpu=cortex-a55")
set(DPDK_PMD_LIBS
    -lrte_net_igc
    -lrte_net_af_packet
    -lrte_net_tap
    -lrte_net_ring
)

add_compile_definitions(PLATFORM_ORANGEPI3B)
