/*
 * sx1278_linux.h — the SX1278 bus on Linux: spidev for SPI, the GPIO
 * character device (uAPI v2) for RESET and DIO0 (ADR-0012).
 *
 * The DIO0 line is requested with rising-edge detection, and the kernel hands
 * back a file descriptor that becomes readable on each edge. That descriptor
 * goes straight into the caller's epoll set: an interrupt from the radio is
 * one more readable fd, with no library thread, no daemon and no callback.
 *
 * Needs read-write access to the SPI and GPIO device nodes (groups spi and
 * gpio on Raspberry Pi OS), not root.
 */

#ifndef TTEC_SX1278_LINUX_H
#define TTEC_SX1278_LINUX_H

#include "sx1278.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *spi_dev;      /* e.g. /dev/spidev0.0 (CE0)                   */
    uint32_t    spi_hz;       /* 1 MHz is ample and tolerant of long wires   */
    const char *gpio_chip;    /* e.g. /dev/gpiochip0                         */
    uint32_t    reset_line;   /* line offset = BCM number on gpiochip0       */
    uint32_t    dio0_line;
} sx1278_linux_cfg_t;

typedef struct {
    int      spi_fd;
    int      reset_fd;        /* line request, output                        */
    int      dio0_fd;         /* line request, input, rising edge; pollable  */
    uint32_t spi_hz;
} sx1278_linux_t;

/* Opens the devices and fills *bus for sx1278_init(). Returns 0, or -1 with
 * the reason in *why. */
int  sx1278_linux_open(sx1278_linux_t *h, const sx1278_linux_cfg_t *cfg,
                       sx1278_bus_t *bus, const char **why);

/* The DIO0 descriptor, for epoll. */
int  sx1278_linux_irq_fd(const sx1278_linux_t *h);

/* Consumes the pending edge events so the descriptor stops being readable. */
void sx1278_linux_irq_drain(sx1278_linux_t *h);

void sx1278_linux_close(sx1278_linux_t *h);

#ifdef __cplusplus
}
#endif

#endif /* TTEC_SX1278_LINUX_H */
