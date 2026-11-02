# The portable test suites on the ESP32 (TTC-16): the same files as on the
# Pi and the PC, compiled by the ESP32's compiler and run on its CPU — on the
# DevKit, or in QEMU (tools/esp32/run_tests_qemu.sh). `make esp32-tests`.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../esp32/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gama_ground_tests)
