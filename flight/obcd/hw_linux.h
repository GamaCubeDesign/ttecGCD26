/*
 * hw_linux.h — obcd's hardware through the kernel's interfaces (ADR-0012):
 * i2c-dev for the INA219, the GPIO character device for the burn-wire and
 * the heaters. No wiringPi, no /sys/class/gpio, no root.
 *
 * Builds anywhere with kernel headers >= 5.10; runs only with the hardware.
 * The pin numbers and the bus come only from the configuration (AGENTS.md:
 * the boards as fabricated cannot run together, and the new OBC board's
 * pins are not known yet).
 */

#ifndef OBCD_HW_LINUX_H
#define OBCD_HW_LINUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ina219.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An i2c-dev bus, and the INA219's address on it. Every transfer is one
 * I2C_RDWR transaction naming its address (ADR-0017): a register read is a
 * write and a read joined by a repeated start, so another master cannot
 * take the bus between them, and one descriptor reaches every device. */
typedef struct {
    int     fd;
    uint8_t ina219_addr;
} hw_i2c_t;

/* Opens path (e.g. /dev/i2c-1) and fills *bus with the INA219 operations at
 * ina219_addr. 0, or -1 with errno set. */
int  hw_i2c_open(hw_i2c_t *h, const char *path, uint8_t ina219_addr, ina219_bus_t *bus);
void hw_i2c_close(hw_i2c_t *h);

/* One write, or one read, to a 7-bit address. 0, or -errno: -EREMOTEIO or
 * -ENXIO when the address did not acknowledge (nobody there). */
int  hw_i2c_write(int fd, uint8_t addr, const uint8_t *buf, size_t len);
int  hw_i2c_read(int fd, uint8_t addr, uint8_t *buf, size_t len);

/* One output line of a GPIO chip, requested low. The kernel keeps the
 * request for as long as the file descriptor is open. -1 if it failed. */
int  hw_gpio_output(const char *chip, uint32_t line, const char *consumer);
int  hw_gpio_set(int fd, bool high);

#ifdef __cplusplus
}
#endif

#endif /* OBCD_HW_LINUX_H */
