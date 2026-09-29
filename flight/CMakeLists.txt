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

# The radio as the shells see it: sx1278 hardware or a UDP simulation.
add_library(ttec_radio STATIC radio/radio_sx1278.c radio/radio_udp.c)
target_link_libraries(ttec_radio PUBLIC ttec_sx1278_linux)
target_compile_definitions(ttec_radio PRIVATE _GNU_SOURCE)

# ttcd configuration parsing, kept out of main.c so it can be unit-tested.
add_library(ttcd_config STATIC ttcd/config.c)
target_link_libraries(ttcd_config PUBLIC ttcd_core)

# The ttcd daemon. The build records `git describe` so every log says which
# build produced it.
find_package(Git QUIET)
set(TTEC_VERSION "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE TTEC_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
add_executable(ttcd ttcd/main.c)
target_link_libraries(ttcd PRIVATE ttcd_config ttcd_core ttec_ipc ttec_radio)
target_compile_definitions(ttcd PRIVATE TTEC_VERSION="${TTEC_VERSION}")

# The adsbd core: SBS parsing, the track table, the onboard record and the
# snapshots for ttcd. No system calls, like ttcd's core, so the tests drive
# it with a virtual clock and recorded SBS lines.
add_library(adsbd_core STATIC
    adsbd/sbs.c
    adsbd/tracks.c
    adsbd/ndjson.c
    adsbd/core.c
)
target_include_directories(adsbd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/adsbd)
target_link_libraries(adsbd_core PUBLIC gama_common)

# adsbd configuration parsing, kept out of main.c so it can be unit-tested.
add_library(adsbd_config STATIC adsbd/config.c)
target_link_libraries(adsbd_config PUBLIC adsbd_core)
# strtok_r is POSIX, not C11.
target_compile_definitions(adsbd_config PRIVATE _POSIX_C_SOURCE=200809L)

# The payload daemon: supervises dump1090-fa, reads its SBS output, records
# every message and sends track snapshots to ttcd (ADR-0008).
add_executable(adsbd adsbd/main.c)
target_link_libraries(adsbd PRIVATE adsbd_config adsbd_core ttec_ipc)
target_compile_definitions(adsbd PRIVATE TTEC_VERSION="${TTEC_VERSION}")
