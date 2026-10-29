# BleLink demo for ESP32 (ESP-IDF v5.x, NimBLE central).
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32      # or esp32c3 / esp32s3
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(blelink_esp32_demo)
