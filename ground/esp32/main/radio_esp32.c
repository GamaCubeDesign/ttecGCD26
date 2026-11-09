#include "radio_esp32.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "gama_tc.h"
#include "sdkconfig.h"

static spi_device_handle_t s_spi;
static TaskHandle_t        s_task;

/* DMA moves the FIFO bursts (up to 1 + 255 bytes) in one transaction with
 * CS held, which the driver requires. The buffers must be DMA-capable and
 * word-aligned; the driver's own, on the stack, are neither. */
static WORD_ALIGNED_ATTR DRAM_ATTR uint8_t s_tx[260];
static WORD_ALIGNED_ATTR DRAM_ATTR uint8_t s_rx[260];

static int spi_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    if (len == 0 || len > sizeof(s_tx)) {
        return -1;
    }
    memcpy(s_tx, tx, len);
    spi_transaction_t t = { .length = len * 8u, .rxlength = len * 8u,
                            .tx_buffer = s_tx, .rx_buffer = s_rx };
    /* Polling, with a bound: a stuck bus is an error, not a hang. IDF 5.3
     * takes only portMAX_DELAY on the start (it waits for the bus semaphore,
     * which this one task never holds); the bound is on the end. */
    if (spi_device_polling_start(s_spi, &t, portMAX_DELAY) != ESP_OK ||
        spi_device_polling_end(s_spi, pdMS_TO_TICKS(10)) != ESP_OK) {
        return -1;
    }
    memcpy(rx, s_rx, len);
    return 0;
}

/* Datasheet §7.2.2: NRESET low for more than 100 us, then 5 ms. Runs once,
 * before the loop. */
static int pulse_reset(void *ctx)
{
    (void)ctx;
    gpio_set_level(CONFIG_GAMA_GS_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(CONFIG_GAMA_GS_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

static void IRAM_ATTR dio0_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

int radio_esp32_open(sx1278_t *dev, TaskHandle_t task, const char **why)
{
    s_task = task;

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_GAMA_GS_PIN_MOSI,
        .miso_io_num = CONFIG_GAMA_GS_PIN_MISO,
        .sclk_io_num = CONFIG_GAMA_GS_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_tx),
    };
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_GAMA_GS_SPI_HZ,
        .spics_io_num = CONFIG_GAMA_GS_PIN_NSS,
        .queue_size = 1,
    };
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK ||
        spi_bus_add_device(SPI3_HOST, &devcfg, &s_spi) != ESP_OK) {
        *why = "SPI setup failed";
        return -1;
    }

    /* RESET high before it becomes an output: no glitch that resets the chip
     * by accident. */
    gpio_set_level(CONFIG_GAMA_GS_PIN_RESET, 1);
    gpio_config_t rst = { .pin_bit_mask = 1ull << CONFIG_GAMA_GS_PIN_RESET,
                          .mode = GPIO_MODE_OUTPUT };
    gpio_config_t dio0 = { .pin_bit_mask = 1ull << CONFIG_GAMA_GS_PIN_DIO0,
                           .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE,
                           .intr_type = GPIO_INTR_POSEDGE };
    esp_err_t isr = gpio_install_isr_service(0);
    if (gpio_config(&rst) != ESP_OK || gpio_config(&dio0) != ESP_OK ||
        (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) ||
        gpio_isr_handler_add(CONFIG_GAMA_GS_PIN_DIO0, dio0_isr, NULL) != ESP_OK) {
        *why = "GPIO setup failed";
        return -1;
    }

    sx1278_bus_t b = { .ctx = NULL, .xfer = spi_xfer, .reset = pulse_reset };
#if CONFIG_GAMA_GS_LBT_PREAMBLE
    sx1278_lbt_t lbt = SX_LBT_PREAMBLE;
#else
    sx1278_lbt_t lbt = SX_LBT_HEADER;
#endif
    return sx1278_init(dev, &b, GAMA_RATE_NOMINAL, (int8_t)CONFIG_GAMA_GS_TX_POWER_DBM, lbt, why);
}
