cmake_minimum_required(VERSION 3.15)

option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# Universal binary for releases. Sanitizer builds stay single-arch (host): the x86_64 slice of an
# ASan binary hangs at startup under Rosetta, and a sanitized build is never shipped.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES AND NOT ENABLE_SANITIZERS)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

project(BleServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BLE_SANITIZER_KINDS "address,undefined" CACHE STRING "Sanitizers used when ENABLE_SANITIZERS=ON (e.g. address,undefined | thread)")
if(ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "ENABLE_SANITIZERS is not supported with MSVC in this project")
    endif()
    add_compile_options(-fsanitize=${BLE_SANITIZER_KINDS} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=${BLE_SANITIZER_KINDS})
endif()

# Common source files
set(COMMON_SOURCES
    src/common/BleServer.cpp
    src/common/BleMessageChunker.cpp
    src/common/BleLinkCodec.cpp
    src/common/BleLinkEngine.cpp
)

set(COMMON_HEADERS
    include/IBleServer.h
    include/BleMessageChunker.h
    src/common/BleUtilities.h
    src/common/BleLinkCodec.h
    src/common/BleLinkEngine.h
    src/common/LongWriteAssembler.h
)

# Platform-specific sources
if(WIN32)
    set(PLATFORM_SOURCES src/platforms/windows/BleServerWindows.cpp)
    set(PLATFORM_LIBS windowsapp.lib)
    set(PLATFORM_INCLUDES "")
elseif(APPLE)
    enable_language(OBJCXX)
    set(PLATFORM_SOURCES src/platforms/macos/BleServerMacOS.mm)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(COREBLUETOOTH_FRAMEWORK CoreBluetooth REQUIRED)
    set(PLATFORM_LIBS
        ${FOUNDATION_FRAMEWORK}
        ${COREBLUETOOTH_FRAMEWORK}
    )
    set(PLATFORM_INCLUDES "")
elseif(UNIX AND NOT APPLE)
    # Linux-specific configuration
    set(PLATFORM_SOURCES src/platforms/linux/BleServerLinux.cpp)
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)

    # Required dependencies for Linux BlueZ/D-Bus implementation
    pkg_check_modules(BLUEZ REQUIRED bluez)
    pkg_check_modules(DBUS REQUIRED dbus-1)
    pkg_check_modules(GIO REQUIRED gio-2.0)
    pkg_check_modules(GLIB REQUIRED glib-2.0)

    set(PLATFORM_LIBS
        ${BLUEZ_LIBRARIES}
        ${DBUS_LIBRARIES}
        ${GIO_LIBRARIES}
        ${GLIB_LIBRARIES}
        Threads::Threads
    )

    set(PLATFORM_INCLUDES
        ${BLUEZ_INCLUDE_DIRS}
        ${DBUS_INCLUDE_DIRS}
        ${GIO_INCLUDE_DIRS}
        ${GLIB_INCLUDE_DIRS}
    )
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# Create shared library
add_library(BleServer SHARED
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
    ${PLATFORM_SOURCES}
)

# Include directories
target_include_directories(BleServer
    PUBLIC include
    PRIVATE src/common
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(BleServer PRIVATE
        BLE_SERVER_EXPORTS
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        # The backend uses co_await under /await (C++17); current MSVC STL turns the
        # <experimental/coroutine> deprecation into a hard error without this.
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
    )
    target_compile_options(BleServer PRIVATE /await)
elseif(UNIX AND NOT APPLE)
    target_include_directories(BleServer PRIVATE ${PLATFORM_INCLUDES})
    target_compile_definitions(BleServer PRIVATE BLE_SERVER_EXPORTS)
    # Enable warnings
    target_compile_options(BleServer PRIVATE -Wall -Wextra)
elseif(APPLE)
    target_compile_definitions(BleServer PRIVATE BLE_SERVER_EXPORTS)
    target_compile_options(BleServer PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>
        $<$<COMPILE_LANGUAGE:OBJCXX>:-Wall -Wextra>
    )
endif()

if(WIN32)
    target_compile_definitions(BleServer PRIVATE BLE_BUILD_PLATFORM="windows")
elseif(APPLE)
    target_compile_definitions(BleServer PRIVATE BLE_BUILD_PLATFORM="macos")
else()
    target_compile_definitions(BleServer PRIVATE BLE_BUILD_PLATFORM="linux")
endif()

# Link libraries
target_link_libraries(BleServer PRIVATE ${PLATFORM_LIBS})

# Set output properties. Unity loads macOS native plugins more reliably when the
# plugin is a real unversioned dylib instead of a CMake symlink chain.
set_target_properties(BleServer PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

if(NOT APPLE)
    set_target_properties(BleServer PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )
endif()

# Copy to Unity project if specified
if(UNITY_PROJECT_PATH)
    if(WIN32)
        set(UNITY_PLUGIN_DIR "${UNITY_PROJECT_PATH}/Assets/Plugins/Windows/x86_64")
    elseif(APPLE)
        set(UNITY_PLUGIN_DIR "${UNITY_PROJECT_PATH}/Assets/Plugins/macOS")
    else()
        set(UNITY_PLUGIN_DIR "${UNITY_PROJECT_PATH}/Assets/Plugins/Linux/x86_64")
    endif()

    add_custom_command(TARGET BleServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${UNITY_PLUGIN_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:BleServer> ${UNITY_PLUGIN_DIR}
    )
endif()

# Build examples (optional)
option(BUILD_EXAMPLES "Build example applications" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Build tests (optional)
option(BUILD_TESTING "Build test applications" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS BleServer
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES
    include/IBleServer.h
    include/BleMessageChunker.h
    DESTINATION include
)

install(FILES
    unity/BleServer.cs
    DESTINATION unity
)