# Bench ground station: the ground console (ground/console) on a real or UDP
# radio. Linux only.
add_executable(gs_cli gs_cli.c)
target_link_libraries(gs_cli PRIVATE gs_console ttec_radio gama_common)
