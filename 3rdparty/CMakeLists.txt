# mbedtls crypto only: libiec61850.a leaves mbedtls_* undefined.
set(MBEDTLS_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/libiec61850/third_party/mbedtls/mbedtls-3.6.0)

file(GLOB MBEDCRYPTO_SOURCES ${MBEDTLS_SRC_DIR}/library/*.c)
# Upstream's src_x509 + src_tls lists (mbedtls-3.6.0/library/CMakeLists.txt).
list(FILTER MBEDCRYPTO_SOURCES EXCLUDE REGEX
     "/(pkcs7|x509[^/]*|debug|mps_[^/]*|net_sockets|ssl_[^/]*)\\.c$")

add_library(mbedcrypto STATIC ${MBEDCRYPTO_SOURCES})
target_include_directories(mbedcrypto
    PUBLIC  ${MBEDTLS_SRC_DIR}/include
    PRIVATE ${MBEDTLS_SRC_DIR}/library
)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_definitions(mbedcrypto
        PRIVATE MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT)
endif()

# Crypto only: no TLS, no X.509. Used by bus_processor, which must stay
# libiec61850-free.
function(setup_mbedcrypto TARGET_NAME)
    target_link_libraries(${TARGET_NAME}
        PRIVATE
        mbedcrypto
    )
endfunction()

# The sources of RSess::Crypto: the neutral half plus exactly one backend.
# mbedcrypto stays in every build regardless — libiec61850 is built
# WITH_MBEDTLS3 and the interop tests use it as the oracle.
set(RSESS_CRYPTO_SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/r_session_crypto.hpp
    ${CMAKE_SOURCE_DIR}/src/common/r_session_crypto_common.cpp
    ${CMAKE_SOURCE_DIR}/src/common/r_session_crypto_${CRYPTO_BACKEND}.cpp
    CACHE INTERNAL "RSess::Crypto sources for the selected backend"
)

# Links whichever backend CRYPTO_BACKEND selected. Every target that compiles
# RSESS_CRYPTO_SOURCES must call this.
function(setup_crypto TARGET_NAME)
    if (CRYPTO_BACKEND STREQUAL "openssl")
        target_link_libraries(${TARGET_NAME} PRIVATE PkgConfig::LIBCRYPTO)
    else()
        target_link_libraries(${TARGET_NAME} PRIVATE mbedcrypto)
    endif()
endfunction()

# Configure libiec61850 submodule
set(WITH_MBEDTLS3                   ON CACHE BOOL "on")
set(CONFIG_INCLUDE_GOOSE_SUPPORT    ON CACHE BOOL "on")
set(CONFIG_IEC61850_R_SMV           ON CACHE BOOL "on")
set(CONFIG_IEC61850_L2_SMV          ON CACHE BOOL "on")
set(CONFIG_IEC61850_R_GOOSE         ON CACHE BOOL "on")
set(CONFIG_IEC61850_L2_GOOSE        ON CACHE BOOL "on")
set(BUILD_EXAMPLES                  OFF CACHE BOOL "off")
set(CONFIG_IEC61850_SNTP_CLIENT     OFF CACHE BOOL "off")
# set(DEBUG_GOOSE_SUBSCRIBER TRUE)
# set(DEBUG_GOOSE_PUBLISHER TRUE)
add_subdirectory(libiec61850 EXCLUDE_FROM_ALL)

function(setup_libiec61850 TARGET_NAME)
    target_include_directories(${TARGET_NAME}
        PRIVATE
        ${CMAKE_SOURCE_DIR}/3rdparty/libiec61850/src/common/inc/
        ${CMAKE_SOURCE_DIR}/3rdparty/libiec61850/src/iec61850/inc/
        ${CMAKE_SOURCE_DIR}/3rdparty/libiec61850/src/mms/inc/
        ${CMAKE_SOURCE_DIR}/3rdparty/libiec61850/src/logging/
        ${CMAKE_SOURCE_DIR}/3rdparty/libiec61850/src/goose/
        ${CMAKE_SOURCE_DIR}/3rdparty/libiec61850/src/r_session/
        ${CMAKE_SOURCE_DIR}/3rdparty/libiec61850/src/sampled_values/
        ${CMAKE_SOURCE_DIR}/3rdparty/libiec61850/hal/inc/
    )
    # message(WARNING "setup_libiec61850 include: ${CMAKE_SOURCE_DIR}")

    target_link_libraries(${TARGET_NAME}
        PUBLIC
        iec61850
    )

    # r_session_crypto_mbedtls.c leaves mbedtls_* unresolved in libiec61850.a
    setup_mbedcrypto(${TARGET_NAME})
endfunction()

