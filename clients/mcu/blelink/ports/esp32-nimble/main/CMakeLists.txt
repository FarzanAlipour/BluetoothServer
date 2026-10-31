# The library (../../../src, ../../../include) and the shared demo are compiled straight into the
# main component; nothing is copied.
set(BLELINK_DIR ${CMAKE_CURRENT_LIST_DIR}/../../..)
idf_component_register(
    SRCS "main.c"
         "${BLELINK_DIR}/src/blelink.c"
         "${BLELINK_DIR}/src/blelink_codec.c"
         "${BLELINK_DIR}/examples/common/demo_app.c"
    INCLUDE_DIRS "." "${BLELINK_DIR}/include" "${BLELINK_DIR}/examples/common"
    PRIV_REQUIRES bt nvs_flash esp_timer)
# Library sizing (include/blelink.h): window 1, 2 send slots; nibble-table CRC (64 bytes).
target_compile_definitions(${COMPONENT_LIB} PRIVATE "DEMO_BOARD_NAME=\"esp32\"" BLELINK_CRC32_IMPL=1)
