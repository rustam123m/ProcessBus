# Intel Atom (Qotom production)

set(PLATFORM_NAME atom)
set(TARGET_ARCH_FLAGS "-march=atom -msse3 -msse4")
set(DPDK_PMD_LIBS
    -lrte_net_e1000
    -lrte_net_igc
    -lrte_net_ixgbe
    -lrte_net_i40e
    -lrte_net_af_xdp
    -lrte_net_af_packet
    -lrte_net_virtio
    -lrte_net_ring
    -lrte_net_tap
    -lrte_net_vhost
)

add_compile_definitions(PLATFORM_ATOM)
