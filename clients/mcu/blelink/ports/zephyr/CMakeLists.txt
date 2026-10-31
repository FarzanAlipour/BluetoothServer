# BleLink demo for Zephyr / nRF Connect SDK (GATT central). Build from a west workspace:
#   west build -b nrf52840dk/nrf52840 clients/mcu/blelink/ports/zephyr      (Zephyr >= 3.7 / NCS >= 2.7)
#   west build -b nrf52840dk_nrf52840 clients/mcu/blelink/ports/zephyr      (older board name)
#   west flash
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blelink_zephyr_demo LANGUAGES C)

set(BLELINK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_sources(app PRIVATE
    src/main.c
    ${BLELINK_DIR}/src/blelink.c
    ${BLELINK_DIR}/src/blelink_codec.c
    ${BLELINK_DIR}/examples/common/demo_app.c)
target_include_directories(app PRIVATE ${BLELINK_DIR}/include ${BLELINK_DIR}/examples/common)
# Library sizing (include/blelink.h): window 1, 2 send slots; nibble-table CRC (64 bytes).
target_compile_definitions(app PRIVATE "DEMO_BOARD_NAME=\"nrf52840dk\"" BLELINK_CRC32_IMPL=1)
