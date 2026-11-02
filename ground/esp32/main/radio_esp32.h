/*
 * radio_esp32.h — the RA-02 on the ground station's ESP32: the SX1278 driver
 * (flight/radio/sx1278.c, the satellite's own) over ESP-IDF's SPI master and
 * GPIO. Pins come from menuconfig ("Gama ground station").
 */

#ifndef RADIO_ESP32_H
#define RADIO_ESP32_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sx1278.h"

/* Resets and configures the chip, then listens. DIO0 rising edges notify
 * `task`. Returns 0, or -1 with *why set. */
int radio_esp32_open(sx1278_t *dev, TaskHandle_t task, const char **why);

#endif /* RADIO_ESP32_H */
