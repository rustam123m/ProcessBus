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

# Crypto microbenchmark. Deliberately not a build_tool_app: it needs neither
# DPDK nor libiec61850, only the crypto libraries under test.
#
# OpenSSL is optional so this cannot break a build on a host without
# libssl-dev — the tool then reports the mbedtls variants alone.
add_executable(crypto_bench crypto_bench.cpp)
setup_mbedcrypto(crypto_bench)

find_package(OpenSSL)
if (OPENSSL_FOUND)
    target_compile_definitions(crypto_bench PRIVATE HAVE_OPENSSL)
    target_link_libraries(crypto_bench PRIVATE OpenSSL::Crypto)
    message(STATUS "crypto_bench: OpenSSL ${OPENSSL_VERSION}")
else()
    message(STATUS "crypto_bench: OpenSSL not found - mbedtls variants only")
endif()

install(TARGETS crypto_bench DESTINATION bin)

