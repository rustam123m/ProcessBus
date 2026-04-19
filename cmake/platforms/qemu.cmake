# QEMU VM (functional testing)

set(PLATFORM_NAME qemu)
set(TARGET_ARCH_FLAGS "")
set(DPDK_PMD_LIBS
    -lrte_net_virtio
    -lrte_net_tap
    -lrte_net_ring
)

add_compile_definitions(PLATFORM_QEMU)
