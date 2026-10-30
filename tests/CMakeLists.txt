# Tests CMakeLists.txt
find_package(Threads REQUIRED)

# Pure chunker test: no server, no backend.
add_executable(test_chunker
    test_chunker.cpp
    ${CMAKE_SOURCE_DIR}/src/common/BleMessageChunker.cpp
)
target_include_directories(test_chunker PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME chunker COMMAND test_chunker)

# C-ABI tests: the real src/common logic against the in-memory fake backend.
# No platform SDK and no Bluetooth hardware needed.
add_library(BleServerTestLib STATIC
    ${CMAKE_SOURCE_DIR}/src/common/BleServer.cpp
    ${CMAKE_SOURCE_DIR}/src/common/BleMessageChunker.cpp
    fake/FakeBleServer.cpp
)
target_include_directories(BleServerTestLib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/fake
)
# BLE_SERVER_EXPORTS: static lib, so BLE_API must not become __declspec(dllimport) on MSVC.
target_compile_definitions(BleServerTestLib PUBLIC BLE_BUILD_PLATFORM="fake" BLE_SERVER_EXPORTS)
target_link_libraries(BleServerTestLib PUBLIC Threads::Threads)

add_executable(ble_tests test_abi.cpp)
target_link_libraries(ble_tests PRIVATE BleServerTestLib)
add_test(NAME abi COMMAND ble_tests)

# GATT test-profile scenario: compiles the ble_test_server logic (examples/ble_test_server) against the fake.
# Independent of BUILD_EXAMPLES: the sources are referenced by path.
add_executable(ble_scenario_tests
    test_scenario_gatt.cpp
    ${CMAKE_SOURCE_DIR}/examples/ble_test_server/TestServerApp.cpp
)
target_link_libraries(ble_scenario_tests PRIVATE BleServerTestLib)
target_include_directories(ble_scenario_tests PRIVATE ${CMAKE_SOURCE_DIR}/examples/ble_test_server)
add_test(NAME scenario_gatt COMMAND ble_scenario_tests)

# Long (prepared) writes: shared LongWriteAssembler rules + end to end through the C ABI and the fake.
add_executable(ble_long_write_tests test_long_write.cpp)
target_link_libraries(ble_long_write_tests PRIVATE BleServerTestLib)
add_test(NAME long_write COMMAND ble_long_write_tests)

add_executable(ble_chunker_tests test_chunker_abi.cpp)
target_link_libraries(ble_chunker_tests PRIVATE BleServerTestLib)
target_include_directories(ble_chunker_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
add_test(NAME chunker_abi COMMAND ble_chunker_tests)
