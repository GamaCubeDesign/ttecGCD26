# The shared codec's sources: one list for every build that compiles them —
# common/CMakeLists.txt (the Pi and the PC) and
# ground/esp32/components/ttec/CMakeLists.txt (the ESP32 ground station).
set(GAMA_COMMON_SOURCES
    crc16.c
    gama_frame.c
    gama_tm.c
    gama_tc.c
    gama_lora.c
    gama_ipc.c
    gama_gs_link.c
    gama_aocs.c
)
