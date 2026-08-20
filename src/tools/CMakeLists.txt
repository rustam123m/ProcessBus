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

# pkg-config rather than find_package: on the aarch64 cross-build the arm64
# libcrypto sits outside CMAKE_FIND_ROOT_PATH, so find_package misses it and the
# tool would silently benchmark mbedtls alone on the very platform we want to
# compare. Still optional — a host without libssl-dev builds the mbedtls variants.
pkg_check_modules(BENCH_LIBCRYPTO IMPORTED_TARGET libcrypto)
if (BENCH_LIBCRYPTO_FOUND)
    target_compile_definitions(crypto_bench PRIVATE HAVE_OPENSSL)
    target_link_libraries(crypto_bench PRIVATE PkgConfig::BENCH_LIBCRYPTO)
    message(STATUS "crypto_bench: OpenSSL ${BENCH_LIBCRYPTO_VERSION}")
else()
    message(STATUS "crypto_bench: OpenSSL not found - mbedtls variants only")
endif()

install(TARGETS crypto_bench DESTINATION bin)

