set(TARGET_NAME bus_processor)

add_executable(${TARGET_NAME}
    ../common/utils.hpp
    ../common/utils.cpp

    ../common/r_frame_builder.hpp
    ../common/r_frame_builder.cpp

    ../common/r_session.hpp
    ../common/r_session.cpp
    ${RSESS_CRYPTO_SOURCES}

    process_bus_parser.hpp
    process_bus_parser.cpp

    rx_application.hpp
    rx_application.cpp

    main.cpp
)

setup_dpdk(${TARGET_NAME})
# Crypto only — the processor must stay libiec61850-free (no setup_libiec61850).
setup_crypto(${TARGET_NAME})
hotpath_disasm(${TARGET_NAME})

install(TARGETS ${TARGET_NAME} DESTINATION bin)

