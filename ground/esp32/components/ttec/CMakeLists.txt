# The code the ground station shares with the rest of the repository, by
# path, never copied: the codec and the link's ground side (common/), the
# SX1278 driver (flight/radio/sx1278.c) and the console (ground/console/).
# Built with the project's own warnings, as on the Pi (AGENTS.md).
get_filename_component(TTEC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
include("${TTEC_ROOT}/common/sources.cmake")
list(TRANSFORM GAMA_COMMON_SOURCES PREPEND "${TTEC_ROOT}/common/")

idf_component_register(
    SRCS ${GAMA_COMMON_SOURCES}
         "${TTEC_ROOT}/flight/radio/sx1278.c"
         "${TTEC_ROOT}/ground/console/gs_console.c"
    INCLUDE_DIRS "${TTEC_ROOT}/common" "${TTEC_ROOT}/flight/radio" "${TTEC_ROOT}/ground/console")

# newlib's platform headers use #include_next, which -Wpedantic reports in
# every file that includes string.h unless they are system headers.
idf_build_get_property(idf_path IDF_PATH)
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE "${idf_path}/components/newlib/platform_include")
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=c11>
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wstrict-prototypes
    -Wmissing-prototypes -Wsign-compare -Wunused-parameter -Wenum-conversion -Werror)
