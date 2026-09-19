# Flight software for the Raspberry Pi Zero 2 W. Linux only: AF_UNIX, epoll,
# spidev and the GPIO character device. The ESP32 builds only common/.

add_library(ttec_ipc STATIC libipc/ipc.c)
target_include_directories(ttec_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libipc)
# accept4() and SOCK_NONBLOCK are GNU/Linux extensions.
target_compile_definitions(ttec_ipc PRIVATE _GNU_SOURCE)

# The ttcd core: link protocol, telecommand dispatch, telemetry scheduling.
# No system calls (ADR-0005), so the tests link it directly and drive it with
# a simulated clock and a simulated radio.
add_library(ttcd_core STATIC
    ttcd/link.c
    ttcd/tc_dispatch.c
    ttcd/tm_sched.c
)
target_include_directories(ttcd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ttcd)
target_link_libraries(ttcd_core PUBLIC gama_common)

# SX1278 register-level driver. Talks to an abstract bus, so the register
# logic is tested against an emulated chip on the development host.
add_library(ttec_sx1278 STATIC radio/sx1278.c)
target_include_directories(ttec_sx1278 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/radio)
target_link_libraries(ttec_sx1278 PUBLIC gama_common)

# The Linux bus for the driver: spidev and the GPIO character device (ADR-0012).
# Builds anywhere with kernel headers >= 5.10; runs only with the hardware.
add_library(ttec_sx1278_linux STATIC radio/sx1278_linux.c)
target_link_libraries(ttec_sx1278_linux PUBLIC ttec_sx1278)
target_compile_definitions(ttec_sx1278_linux PRIVATE _GNU_SOURCE)
