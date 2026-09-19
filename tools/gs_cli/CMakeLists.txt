# Bench ground station: the real gs_link on a real (or UDP) radio. Linux only.
add_executable(gs_cli gs_cli.c)
target_link_libraries(gs_cli PRIVATE ttec_radio gama_common)
