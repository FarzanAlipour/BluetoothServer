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
target_compile_definitions(BleServerTestLib PUBLIC BLE_BUILD_PLATFORM="fake")
target_link_libraries(BleServerTestLib PUBLIC Threads::Threads)

add_executable(ble_tests test_abi.cpp)
target_link_libraries(ble_tests PRIVATE BleServerTestLib)
add_test(NAME abi COMMAND ble_tests)
