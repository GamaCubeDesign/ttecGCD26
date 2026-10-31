# The ESP32 ground station (ADR-0018). Built in the ESP-IDF Docker image by
# tools/esp32/idf.sh; `make esp32-build` from the repository root.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gama_ground)
