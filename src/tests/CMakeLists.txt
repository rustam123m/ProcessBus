# Common sources shared by both test binaries
set(TEST_COMMON_SOURCES
    ../common/r_frame_builder.hpp
    ../common/r_frame_builder.cpp

    ../common/r_session.hpp
    ../common/r_session.cpp
    ../common/r_session_crypto.hpp
    ../common/r_session_crypto.cpp

    ../bus_generator/goose_traffic_gen.hpp
    ../bus_generator/goose_traffic_gen.cpp
    ../bus_generator/r_goose_traffic_gen.hpp
    ../bus_generator/r_goose_traffic_gen.cpp
    ../bus_generator/r_sv_traffic_gen.hpp
    ../bus_generator/r_sv_traffic_gen.cpp
    ../bus_generator/sv_traffic_gen.hpp
    ../bus_generator/sv_traffic_gen.cpp

    ../bus_processor/process_bus_parser.hpp
    ../bus_processor/process_bus_parser.cpp
)

# Unit tests (no AF_PACKET required, run in CI)
set(UNIT_TEST unit_tests)
add_executable(${UNIT_TEST}
    ${TEST_COMMON_SOURCES}
    goose_traffic_test.cpp
    burst_plan_test.cpp
    appid_container_test.cpp
    pipeline_test.cpp
    mac_addr_test.cpp
    goose_container_test.cpp
    sv_container_test.cpp
    parser_error_test.cpp
    r_session_test.cpp
    r_routing_test.cpp
    r_goose_interop_test.cpp
    main.cpp
)
target_link_libraries(${UNIT_TEST} PRIVATE gtest gmock gtest_main)
setup_dpdk(${UNIT_TEST})
setup_libiec61850(${UNIT_TEST})
add_test(NAME ${UNIT_TEST} COMMAND ${UNIT_TEST})
install(TARGETS ${UNIT_TEST} DESTINATION bin)

# Integration tests (require AF_PACKET / sudo)
set(INTEGRATION_TEST integration_tests)
add_executable(${INTEGRATION_TEST}
    ${TEST_COMMON_SOURCES}
    goose_integration_test.cpp
    sv_integration_test.cpp
    # Needs AF_PACKET: libiec61850's SV publisher has no buffer-only
    # constructor, so building an SV skeleton opens a raw socket.
    r_sv_interop_test.cpp
    main.cpp
)
target_link_libraries(${INTEGRATION_TEST} PRIVATE gtest gmock gtest_main)
setup_dpdk(${INTEGRATION_TEST})
setup_libiec61850(${INTEGRATION_TEST})
add_test(NAME ${INTEGRATION_TEST} COMMAND ${INTEGRATION_TEST})
install(TARGETS ${INTEGRATION_TEST} DESTINATION bin)
