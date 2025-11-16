cmake_minimum_required(VERSION 3.15)
project(BleServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Common source files
set(COMMON_SOURCES
    src/common/BleServer.cpp
    src/common/BleMessageChunker.cpp
)

set(COMMON_HEADERS
    include/IBleServer.h
    include/BleMessageChunker.h
    src/common/BleUtilities.h
)

# Platform-specific sources
if(WIN32)
    set(PLATFORM_SOURCES src/platforms/windows/BleServerWindows.cpp)
    set(PLATFORM_LIBS windowsapp.lib)
    set(PLATFORM_INCLUDES "")
elseif(APPLE)
    # macOS implementation not yet available
    message(FATAL_ERROR "macOS platform is not yet supported. Windows and Linux only.")
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
    )
    target_compile_options(BleServer PRIVATE /await)
elseif(UNIX AND NOT APPLE)
    target_include_directories(BleServer PRIVATE ${PLATFORM_INCLUDES})
    target_compile_definitions(BleServer PRIVATE BLE_SERVER_EXPORTS)
    # Enable warnings
    target_compile_options(BleServer PRIVATE -Wall -Wextra)
endif()

# Link libraries
target_link_libraries(BleServer PRIVATE ${PLATFORM_LIBS})

# Set output properties
set_target_properties(BleServer PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON
)

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