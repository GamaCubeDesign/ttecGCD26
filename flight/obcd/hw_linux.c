#define _GNU_SOURCE
#include "hw_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int i2c_write_reg16(void *ctx, uint8_t reg, uint16_t value)
{
    hw_i2c_t *h = ctx;
    uint8_t b[3] = { reg, (uint8_t)(value >> 8), (uint8_t)value };
    return write(h->fd, b, sizeof(b)) == (ssize_t)sizeof(b) ? 0 : -1;
}

/* A register pointer write, then a two-byte read, as the INA219 expects. */
static int i2c_read_reg16(void *ctx, uint8_t reg, uint16_t *value)
{
    hw_i2c_t *h = ctx;
    uint8_t b[2];
    if (write(h->fd, &reg, 1) != 1 || read(h->fd, b, 2) != 2) {
        return -1;
    }
    *value = (uint16_t)((uint16_t)b[0] << 8 | b[1]);
    return 0;
}

int hw_i2c_open(hw_i2c_t *h, const char *path, uint8_t addr, ina219_bus_t *bus)
{
    h->fd = open(path, O_RDWR | O_CLOEXEC);
    if (h->fd < 0) {
        return -1;
    }
    if (ioctl(h->fd, I2C_SLAVE, (unsigned long)addr) < 0) {
        int e = errno;
        close(h->fd);
        h->fd = -1;
        errno = e;
        return -1;
    }
    bus->ctx = h;
    bus->write_reg16 = i2c_write_reg16;
    bus->read_reg16 = i2c_read_reg16;
    return 0;
}

void hw_i2c_close(hw_i2c_t *h)
{
    if (h->fd >= 0) {
        close(h->fd);
        h->fd = -1;
    }
}

int hw_gpio_output(const char *chip, uint32_t line, const char *consumer)
{
    int chip_fd = open(chip, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) {
        return -1;
    }
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = line;
    req.num_lines = 1;
    strncpy(req.consumer, consumer, sizeof(req.consumer) - 1);
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 0u;          /* requested low: off */
    req.config.attrs[0].mask = 1u;
    int r = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
    int e = errno;
    close(chip_fd);
    errno = e;
    return r < 0 ? -1 : req.fd;
}

int hw_gpio_set(int fd, bool high)
{
    if (fd < 0) {
        return -1;
    }
    struct gpio_v2_line_values v;
    memset(&v, 0, sizeof(v));
    v.bits = high ? 1u : 0u;
    v.mask = 1u;
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0 ? -1 : 0;
}
