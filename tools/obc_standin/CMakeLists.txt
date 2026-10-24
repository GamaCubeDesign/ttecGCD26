# Stand-in for the OBC until flight/obcd/ exists. Linux only; not flight software.
add_executable(obc_standin obc_standin.c)
target_link_libraries(obc_standin PRIVATE ttec_ipc gama_common)
