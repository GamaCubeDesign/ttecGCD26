/*
 * gs_main.c — the ESP32 ground station (ADR-0018).
 *
 * The ground station's console (ground/console/gs_console.h) on the RA-02,
 * spoken over UART0 — the DevKit's USB port — in the same text commands and
 * the same JSON lines as the bench tool tools/gs_cli. The host side is
 * ground/host/gs_serial.py, which makes this board look like gs_cli to every
 * bench script.
 *
 * At boot: one `boot` line, then `set` lines from the host (the wall clock
 * first: this board has none), then `begin`. `quit` prints the summary and
 * restarts the board, so every session starts fresh, like a gs_cli process.
 *
 * One task, one loop, no blocking but the wait for the next deadline: DIO0
 * rising edges and the deadline wake it; the radio is also polled every
 * 20 ms, which covers a missed edge. The UART is written through the
 * driver's ring buffer, never waited on: a line that does not fit is
 * counted and dropped, because the ground has 20 ms to answer the satellite
 * (ADR-0007) and must not spend them on a serial port.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gama_lora.h"
#include "gama_tc.h"
#include "gs_console.h"
#include "radio_esp32.h"
#include "sdkconfig.h"

#define POLL_MS  20u
#define UART     UART_NUM_0

static gs_console_t C;          /* static: its line buffers are large */
static sx1278_t     R;
static uint32_t     s_dropped;  /* lines the UART ring could not take */

static uint64_t mono_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ---- the console's operations ---- */

static int c_tx(void *c, const uint8_t *f, size_t n)
{
    (void)c;
    return sx1278_try_transmit(&R, f, (uint8_t)n);   /* SX_TX_* are GS_TX_* */
}

static int c_prof(void *c, uint8_t p) { (void)c; return sx1278_set_profile(&R, p); }
static int c_power(void *c, int8_t dbm) { (void)c; return sx1278_set_power(&R, dbm); }

static int c_lbt(void *c, bool preamble)
{
    (void)c;
    R.lbt = preamble ? SX_LBT_PREAMBLE : SX_LBT_HEADER;
    return 0;
}

static void c_write(void *c, const char *line, size_t len)
{
    (void)c;
    size_t room = 0;
    if (uart_get_tx_buffer_free_size(UART, &room) != ESP_OK || room < len) {
        s_dropped++;
        return;
    }
    uart_write_bytes(UART, line, len);   /* copies into the ring and returns */
}

/* ---- input: lines from the host, taken only when the console wants one ---- */

static char   s_in[2048];
static size_t s_have;

static void read_input(void)
{
    while (gs_console_wants_line(&C)) {
        char *end = NULL;
        for (size_t i = 0; i < s_have; i++) {
            if (s_in[i] == '\n' || s_in[i] == '\r') {
                end = s_in + i;
                break;
            }
        }
        if (end != NULL) {
            char line[GS_CONSOLE_INPUT_MAX];
            size_t len = (size_t)(end - s_in);
            if (len >= sizeof(line)) {
                len = sizeof(line) - 1u;
            }
            memcpy(line, s_in, len);
            line[len] = '\0';
            size_t used = (size_t)(end - s_in) + 1u;
            memmove(s_in, s_in + used, s_have - used);
            s_have -= used;
            if (len > 0) {
                gs_console_line(&C, mono_ms(), line);
            }
            continue;
        }
        if (s_have == sizeof(s_in)) {
            s_have = 0;                  /* a line longer than the buffer: dropped */
        }
        int n = uart_read_bytes(UART, s_in + s_have, (uint32_t)(sizeof(s_in) - s_have), 0);
        if (n <= 0) {
            return;
        }
        s_have += (size_t)n;
    }
}

/* ---- the radio ---- */

static void service_radio(void)
{
    for (;;) {
        sx1278_event_t ev = SX_EVENT_NONE;
        uint8_t buf[GAMA_LORA_MAX_PAYLOAD];
        uint8_t len = 0;
        int16_t rssi = 0;
        int8_t snr = 0;
        if (sx1278_service(&R, &ev, buf, &len, &rssi, &snr) != 0 || ev == SX_EVENT_NONE) {
            return;
        }
        uint64_t now = mono_ms();
        if (ev == SX_EVENT_RX) {
            gs_console_on_rx(&C, now, buf, len, rssi, snr);
        } else if (ev == SX_EVENT_RX_CRC_ERROR) {
            gs_console_on_rx_error(&C, now);
        } else if (ev == SX_EVENT_TX_DONE) {
            gs_console_on_tx_done(&C, now);
        }
    }
}

static void boot_line(const char *radio, const char *why)
{
    char f[320];
    snprintf(f, sizeof(f),
             "\"fw\":\"%s\",\"idf\":\"%s\",\"reset\":%d,\"radio\":\"%s\",\"why\":\"%s\","
             "\"power_dbm\":%d,\"pins\":\"sck=%d miso=%d mosi=%d nss=%d reset=%d dio0=%d\"",
             esp_app_get_description()->version, esp_get_idf_version(), (int)esp_reset_reason(),
             radio, why, CONFIG_GAMA_GS_TX_POWER_DBM, CONFIG_GAMA_GS_PIN_SCK,
             CONFIG_GAMA_GS_PIN_MISO, CONFIG_GAMA_GS_PIN_MOSI, CONFIG_GAMA_GS_PIN_NSS,
             CONFIG_GAMA_GS_PIN_RESET, CONFIG_GAMA_GS_PIN_DIO0);
    gs_console_event(&C, mono_ms(), "boot", f);
}

void app_main(void)
{
    uart_driver_install(UART, 2048, 16384, 0, NULL, 0);
    uart_vfs_dev_use_driver(UART);       /* stray ESP_LOG lines go the same way */

    gs_console_ops_t ops = { .radio_tx = c_tx, .radio_set_profile = c_prof,
                             .radio_set_power = c_power, .radio_set_lbt = c_lbt,
                             .write_line = c_write };
    gs_console_init(&C, &ops, "esp32");

    const char *why = "";
    if (radio_esp32_open(&R, xTaskGetCurrentTaskHandle(), &why) != 0) {
        /* No radio, no ground station: say so until someone looks. */
        for (;;) {
            boot_line("error", why);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    boot_line("ok", "");

    for (;;) {
        read_input();
        service_radio();
        gs_console_on_tick(&C, mono_ms());
        if (gs_console_finished(&C)) {
            if (s_dropped > 0) {
                char f[48];
                snprintf(f, sizeof(f), "\"lines_dropped\":%" PRIu32, s_dropped);
                gs_console_event(&C, mono_ms(), "uart", f);
            }
            gs_console_summary(&C, mono_ms());
            uart_wait_tx_done(UART, pdMS_TO_TICKS(500));
            esp_restart();
        }
        uint64_t now = mono_ms();
        uint64_t d = gs_console_next_deadline(&C);
        uint64_t wait = d > now ? d - now : 0;
        if (wait > POLL_MS) {
            wait = POLL_MS;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)wait));
    }
}
