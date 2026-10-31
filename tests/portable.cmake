# The test suites that are portable C: no OS, no files, just the shared code.
# One list for the host build (tests/CMakeLists.txt) and the ESP32 run
# (ground/esp32-tests), which together show that both ends of the link
# compute the same bytes (TTC-16, ADR-0002).
#
# TTEC_CODEC_TESTS need only common/; the others also link the SX1278 driver
# or the ground console.
set(TTEC_CODEC_TESTS
    test_crc16
    test_frame
    test_tm
    test_tc
    test_vectors
    test_lora
    test_ipc_codec
    test_gs_link
    test_aocs_codec
)
set(TTEC_PORTABLE_TESTS ${TTEC_CODEC_TESTS} test_sx1278 test_gs_console)
