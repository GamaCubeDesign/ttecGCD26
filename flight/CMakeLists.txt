# Flight software for the Raspberry Pi Zero 2 W. Linux only: AF_UNIX, epoll,
# spidev and the GPIO character device. The ESP32 builds only common/.

add_library(ttec_ipc STATIC libipc/ipc.c)
target_include_directories(ttec_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libipc)
# accept4() and SOCK_NONBLOCK are GNU/Linux extensions.
target_compile_definitions(ttec_ipc PRIVATE _GNU_SOURCE)
