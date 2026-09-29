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

add_executable(test_sx1278 test_sx1278.c)
target_link_libraries(test_sx1278 PRIVATE ttec_sx1278)
target_include_directories(test_sx1278 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_sx1278 COMMAND test_sx1278)

add_executable(test_config test_config.c)
target_link_libraries(test_config PRIVATE ttcd_config)
target_include_directories(test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_config COMMAND test_config)

# adsbd: the SBS parser and the record line, the track table, and the core on
# a virtual clock.
add_executable(test_sbs test_sbs.c)
target_link_libraries(test_sbs PRIVATE adsbd_core)
target_include_directories(test_sbs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_sbs COMMAND test_sbs)

add_executable(test_tracks test_tracks.c)
target_link_libraries(test_tracks PRIVATE adsbd_core)
target_include_directories(test_tracks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_tracks COMMAND test_tracks)

add_executable(test_adsbd_core test_adsbd_core.c)
target_link_libraries(test_adsbd_core PRIVATE adsbd_core)
target_include_directories(test_adsbd_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_adsbd_core COMMAND test_adsbd_core)

add_executable(test_adsbd_config test_adsbd_config.c)
target_link_libraries(test_adsbd_config PRIVATE adsbd_config)
target_include_directories(test_adsbd_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_adsbd_config COMMAND test_adsbd_config)

# The real ttcd binary, end to end, over the UDP radio.
add_executable(test_ttcd_integration test_ttcd_integration.c)
target_link_libraries(test_ttcd_integration PRIVATE ttec_radio ttec_ipc gama_common)
target_include_directories(test_ttcd_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_ttcd_integration COMMAND test_ttcd_integration $<TARGET_FILE:ttcd>)
set_tests_properties(test_ttcd_integration PROPERTIES TIMEOUT 60)

# The real adsbd binary: dump1090 and ttcd played by the test.
add_executable(test_adsbd_integration test_adsbd_integration.c)
target_link_libraries(test_adsbd_integration PRIVATE ttec_ipc gama_common)
target_include_directories(test_adsbd_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_adsbd_integration COMMAND test_adsbd_integration $<TARGET_FILE:adsbd>)
set_tests_properties(test_adsbd_integration PROPERTIES TIMEOUT 60)

# The chain end to end: SBS -> adsbd -> ttcd -> UDP radio -> ground. Also
# shows the radio surviving the payload, and ttcd re-anchoring a restarted
# adsbd.
add_executable(test_adsb_chain test_adsb_chain.c)
target_link_libraries(test_adsb_chain PRIVATE ttec_radio gama_common)
target_include_directories(test_adsb_chain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_adsb_chain
         COMMAND test_adsb_chain $<TARGET_FILE:ttcd> $<TARGET_FILE:adsbd>)
set_tests_properties(test_adsb_chain PROPERTIES TIMEOUT 90)

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
    # The ground's ADS-B estimator, with its own tests.
    add_test(NAME ground_aeronaves
             COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/ground/host/ground-aeronaves)
endif()
