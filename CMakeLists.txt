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

option(TTEC_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(TTEC_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(common)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Developer tools. Not part of the flight software.
add_subdirectory(tools/gen_vectors)
