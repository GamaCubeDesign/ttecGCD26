# Host-side unit tests for the shared codec. No external framework, so these
# can be cross-compiled and run on the ESP32 to prove both ends of the link
# really do produce identical bytes.
set(TTEC_TESTS
    test_crc16
    test_frame
    test_tm
    test_tc
    test_vectors
    test_lora
    test_ipc_codec
    test_gs_link
)

foreach(t IN LISTS TTEC_TESTS)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE gama_common)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# Flight-software tests: Linux only, linked against the flight libraries.
add_executable(test_libipc test_libipc.c)
target_link_libraries(test_libipc PRIVATE ttec_ipc gama_common)
target_include_directories(test_libipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_libipc COMMAND test_libipc)

add_executable(test_ttcd_core test_ttcd_core.c)
target_link_libraries(test_ttcd_core PRIVATE ttcd_core)
target_include_directories(test_ttcd_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_ttcd_core COMMAND test_ttcd_core)

add_executable(test_link_sim test_link_sim.c)
target_link_libraries(test_link_sim PRIVATE ttcd_core)
target_include_directories(test_link_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_link_sim COMMAND test_link_sim)

# test_tm exercises NaN handling and quantisation bounds.
target_link_libraries(test_tm PRIVATE m)

# The ADRs quote figures produced by tools/analysis/lora_budget.py. This test
# fails if the script and the documentation ever drift apart, so a change to
# the protocol's record sizes cannot silently invalidate the data budget that
# justifies the architecture.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME budget_matches_adrs
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tools/analysis/lora_budget.py --check)
endif()
