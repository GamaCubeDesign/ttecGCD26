# The ground station's console: the link's ground side, its commands and its
# JSON lines. Portable C11 — the same file builds into tools/gs_cli on Linux
# and into the ESP32 ground station (ground/esp32/components/ttec).
add_library(gs_console STATIC gs_console.c)
target_include_directories(gs_console PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gs_console PUBLIC gama_common)
