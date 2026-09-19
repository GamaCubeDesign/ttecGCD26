# The shared codec. Compiled here for the Raspberry Pi flight software and
# the host-side tools; the ESP32 firmware pulls the same sources in as an
# ESP-IDF component (see ground/esp32/main/CMakeLists.txt).
#
# Nothing in this directory may include a platform header or depend on libm,
# pthreads or any OS facility -- that portability is what makes one wire
# format impossible to get wrong on only one side of the link.
add_library(gama_common STATIC
    crc16.c
    gama_frame.c
    gama_tm.c
    gama_tc.c
    gama_lora.c
)

target_include_directories(gama_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
