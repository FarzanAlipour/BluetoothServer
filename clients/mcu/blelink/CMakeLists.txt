# BleLink v1 MCU client library (portable C99) and its host unit tests.
#
#   cmake -S clients/mcu/blelink -B build-blelink && cmake --build build-blelink && ctest --test-dir build-blelink
#
# Standalone project (the repository root is a C++ project for the server). Firmware builds do not use
# this file: ESP-IDF and Zephyr add src/*.c and include/ through their own build systems (see ports/).
cmake_minimum_required(VERSION 3.16)
project(blelink VERSION 1.0.0 LANGUAGES C)

option(BLELINK_BUILD_TESTS "Build the host unit tests" ON)
option(BLELINK_SANITIZERS "Build the tests with ASan + UBSan" OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(BLELINK_SOURCES src/blelink_codec.c src/blelink.c)

# Library with the default configuration (window 1, 2 send slots, bitwise CRC).
add_library(blelink STATIC ${BLELINK_SOURCES})
target_include_directories(blelink PUBLIC include)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(BLELINK_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)
endif()
target_compile_options(blelink PRIVATE ${BLELINK_WARNINGS})

if(BLELINK_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(VECTORS_JSON ${CMAKE_CURRENT_SOURCE_DIR}/../../../docs/protocol-test-vectors.json)
    set(VECTORS_H ${CMAKE_CURRENT_BINARY_DIR}/gen/blelink_test_vectors.h)
    add_custom_command(
        OUTPUT ${VECTORS_H}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_vectors_h.py ${VECTORS_JSON} ${VECTORS_H}
        DEPENDS ${VECTORS_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_vectors_h.py
        COMMENT "Generating blelink_test_vectors.h from docs/protocol-test-vectors.json")
    add_custom_target(blelink_vectors DEPENDS ${VECTORS_H})

    # One test binary per compile-time configuration: the MCU default (window 1, packetSize 20 cases
    # included), a multi-message configuration, and each CRC-32 variant.
    function(blelink_test name defs)
        add_executable(${name} tests/test_blelink.c ${BLELINK_SOURCES})
        add_dependencies(${name} blelink_vectors)
        target_include_directories(${name} PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/gen)
        target_compile_definitions(${name} PRIVATE ${defs})
        target_compile_options(${name} PRIVATE ${BLELINK_WARNINGS})
        if(BLELINK_SANITIZERS)
            target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
            target_link_options(${name} PRIVATE -fsanitize=address,undefined)
        endif()
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    blelink_test(blelink_test_default "BLELINK_CRC32_IMPL=0")
    blelink_test(blelink_test_window4 "BLELINK_MAX_RX_WINDOW=4;BLELINK_MAX_TX=4;BLELINK_CRC32_IMPL=1")
    blelink_test(blelink_test_crctable "BLELINK_MAX_RX_WINDOW=2;BLELINK_MAX_TX=3;BLELINK_CRC32_IMPL=2;BLELINK_CTRL_QUEUE=8")

    # The example application shared by the ESP32 and Zephyr ports, against a scripted fake transport.
    add_executable(blelink_test_demo tests/test_demo.c examples/common/demo_app.c ${BLELINK_SOURCES})
    target_include_directories(blelink_test_demo PRIVATE include examples/common)
    target_compile_options(blelink_test_demo PRIVATE ${BLELINK_WARNINGS})
    if(BLELINK_SANITIZERS)
        target_compile_options(blelink_test_demo PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(blelink_test_demo PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME blelink_test_demo COMMAND blelink_test_demo)
endif()
