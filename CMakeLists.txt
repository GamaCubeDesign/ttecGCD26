cmake_minimum_required(VERSION 3.16)
project(ttec C)

# C11 across the whole project, matching the obc repository so that shared
# headers and the OBC-side IPC reference implementation compile identically
# in both. See docs/adr/0002-flight-software-language.md.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# -Wconversion is deliberate and load-bearing here: almost every bug class
# this project can afford least -- a truncated length field, a sign lost on a
# latitude -- shows up first as an implicit narrowing conversion.
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wstrict-prototypes -Wmissing-prototypes
    -Wpointer-arith -Wvla
)

option(TTEC_SANITIZE "Build with the sanitizers listed in TTEC_SANITIZERS" OFF)
# ASan cannot run on the Pi Zero 2 W: GCC's arm64 ASan reserves its heap at
# 0x500000000000 and the Pi kernel gives each process a 39-bit address space
# (bench notes, docs/vv/bancada/2026-10-07). The Makefile passes "undefined"
# there; ASan keeps running on the PC.
set(TTEC_SANITIZERS "address,undefined" CACHE STRING
    "Comma-separated -fsanitize= list used when TTEC_SANITIZE is ON")
if(TTEC_SANITIZE)
    add_compile_options(-fsanitize=${TTEC_SANITIZERS} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${TTEC_SANITIZERS})
endif()

add_subdirectory(common)
add_subdirectory(flight)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Developer tools. Not part of the flight software.
add_subdirectory(tools/gen_vectors)
add_subdirectory(tools/gs_cli)
