# Examples CMakeLists.txt
find_package(Threads REQUIRED)

# ble_test_server: native GATT test server for hardware tests with nRF Connect (docs/TESTING.md).
# TestServerApp.cpp is plain C ABI client code; tests/ compiles the same file against the fake backend.
add_executable(ble_test_server
    ble_test_server/main.cpp
    ble_test_server/TestServerApp.cpp
    ble_test_server/TestServerApp.h
    ble_test_server/TestProfile.h
)
target_link_libraries(ble_test_server PRIVATE BleServer Threads::Threads)
target_include_directories(ble_test_server PRIVATE ${CMAKE_SOURCE_DIR}/include ble_test_server)
if(NOT WIN32)
    target_compile_options(ble_test_server PRIVATE -Wall -Wextra)
endif()
if(APPLE)
    # A command-line tool needs an embedded Info.plist with NSBluetoothAlwaysUsageDescription, otherwise
    # macOS cannot show/record the Bluetooth permission for it (docs/TESTING.md, macOS section).
    target_link_options(ble_test_server PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/ble_test_server/Info.plist")
    set_property(TARGET ble_test_server APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ble_test_server/Info.plist)
endif()

# On Windows the executable needs BleServer.dll next to it.
if(WIN32)
    add_custom_command(TARGET ble_test_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:BleServer> $<TARGET_FILE_DIR:ble_test_server>)
endif()

install(TARGETS ble_test_server
    RUNTIME DESTINATION bin/examples
)
