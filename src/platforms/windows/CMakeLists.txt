# Windows-specific sources
set(PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/BleServerWindows.cpp
)

# Add to parent scope
set(PLATFORM_SOURCES ${PLATFORM_SOURCES} PARENT_SCOPE)

# Windows-specific settings
if(MSVC)
    add_compile_options(/await)
endif()