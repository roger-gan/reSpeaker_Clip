# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(clip)

# Include directories
target_include_directories(app PRIVATE include)

# Source files
target_sources(app PRIVATE
    src/main.c
    src/clip_event.c
    src/config.c
    src/ble.c
    src/transport.c
    src/transport_ble.c
    src/at_server.c
    src/at_commands.c
    src/audio.c
    src/storage.c
    src/transfer.c
    src/haptic.c
    src/button.c
    src/display.c
    src/icons.c
    src/wifi_udp.c
    src/transport_udp.c
    src/battery.c
    src/usb_cdc.c
)

if(CONFIG_CLIP_HTTPS_POC)
    # NCS nrf-config.h does not expose Zephyr's HAVE_TIME_DATE Kconfig option.
    # Apply to the entire application image including its TLS/X.509 libraries.
    zephyr_compile_definitions(MBEDTLS_HAVE_TIME MBEDTLS_HAVE_TIME_DATE MBEDTLS_PLATFORM_MS_TIME_ALT)
    # Hostap selects MBEDTLS_ENABLE_HEAP unconditionally in this NCS version.
    # Replace only this image's initializer, at the SDK's original init priority;
    # leave the shared SDK and its public calloc/free API unchanged.
    get_target_property(_tls_sources mbedtls_zephyr SOURCES)
    set(_tls_initializers ${_tls_sources})
    list(FILTER _tls_initializers INCLUDE REGEX "(^|/)mbedtls_heap[.]c$")
    list(LENGTH _tls_initializers _tls_count)
    if(NOT _tls_count EQUAL 1)
        message(FATAL_ERROR "NCS heap initializer changed: review HTTPS allocator integration")
    endif()
    list(GET _tls_initializers 0 _tls_original)
    list(REMOVE_ITEM _tls_sources "${_tls_original}")
    list(APPEND _tls_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/tls_allocator.c")
    set_property(TARGET mbedtls_zephyr PROPERTY SOURCES "${_tls_sources}")
    if(NOT CLIP_HTTPS_CA_FILE)
        set(CLIP_HTTPS_CA_FILE "$ENV{CLIP_HTTPS_CA_FILE}")
    endif()
    if(NOT CLIP_HTTPS_CA_FILE OR NOT EXISTS "${CLIP_HTTPS_CA_FILE}")
        message(FATAL_ERROR "Set CLIP_HTTPS_CA_FILE to the trusted test CA PEM (not a private key)")
    endif()
    file(READ "${CLIP_HTTPS_CA_FILE}" _ca_pem)
    if(NOT _ca_pem MATCHES "BEGIN CERTIFICATE" OR _ca_pem MATCHES "PRIVATE KEY")
        message(FATAL_ERROR "CLIP_HTTPS_CA_FILE must contain public CA certificate(s), not keys")
    endif()
    file(READ "${CLIP_HTTPS_CA_FILE}" _ca_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _ca_bytes "${_ca_hex}")
    file(WRITE "${PROJECT_BINARY_DIR}/https_ca.h"
        "static const unsigned char https_ca[] = {${_ca_bytes}0};\n")
    target_include_directories(app PRIVATE "${PROJECT_BINARY_DIR}")
    target_sources(app PRIVATE src/wifi_station.c src/https_poc.c)
else()
    target_sources(app PRIVATE src/wifi.c)
endif()
