# The shared codec. Compiled here for the Raspberry Pi flight software and
# the host-side tools; the ESP32 ground station compiles the same sources as
# an ESP-IDF component (ground/esp32/components/ttec/CMakeLists.txt). The list
# is in sources.cmake, shared by both.
#
# Nothing in this directory may include a platform header or depend on libm,
# pthreads or any OS facility -- that portability is what makes one wire
# format impossible to get wrong on only one side of the link.
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)
add_library(gama_common STATIC ${GAMA_COMMON_SOURCES})

target_include_directories(gama_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
