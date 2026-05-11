function(build_tool_app NAME)
    add_executable(${NAME}
        ${NAME}.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/utils.cpp
    )

    setup_dpdk(${NAME})
    setup_libiec61850(${NAME})

    install(TARGETS ${NAME} DESTINATION bin)
endfunction()

build_tool_app(rtspin)
build_tool_app(delay_meter)
build_tool_app(pkt_redirect)
build_tool_app(rx_counter)

