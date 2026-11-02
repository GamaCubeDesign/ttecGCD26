/*
 * Runs every portable suite on the ESP32 and prints, last, the sentinel line
 * tools/esp32/run_tests_qemu.sh and the bench look for:
 *
 *   ESP32 SUITES: <run> run, <failed> failed
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "suites.h"

void app_main(void)
{
    int run = 0, failed = 0;
    printf("ESP32 SUITES: start\n");
#define X(suite)                                         \
    do {                                                 \
        printf("== %s\n", #suite);                       \
        run++;                                           \
        if (suite##_main() != 0) {                       \
            failed++;                                    \
        }                                                \
    } while (0);
    TTEC_SUITES(X)
#undef X
    printf("ESP32 SUITES: %d run, %d failed\n", run, failed);
    fflush(stdout);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
