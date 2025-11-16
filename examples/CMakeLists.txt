# Examples CMakeLists.txt

# Simple server example
add_executable(simple_server simple_server.cpp)
target_link_libraries(simple_server PRIVATE BleServer)
target_include_directories(simple_server PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Install examples
install(TARGETS simple_server
    RUNTIME DESTINATION bin/examples
)
