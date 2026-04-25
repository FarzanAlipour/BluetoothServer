# Tests CMakeLists.txt

add_executable(test_chunker
    test_chunker.cpp
    ${CMAKE_SOURCE_DIR}/src/common/BleMessageChunker.cpp
)
target_include_directories(test_chunker PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_test(NAME chunker COMMAND test_chunker)
