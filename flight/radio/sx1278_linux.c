#include "sx1278_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int spi_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    sx1278_linux_t *h = ctx;
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = (uint32_t)len;
    tr.speed_hz = h->spi_hz;
    tr.bits_per_word = 8;
    return ioctl(h->spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0 ? -1 : 0;
}

static int set_line(int fd, int value)
{
    struct gpio_v2_line_values v;
    memset(&v, 0, sizeof(v));
    v.bits = value ? 1u : 0u;
    v.mask = 1u;
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0 ? -1 : 0;
}

static void sleep_ms(long ms)
{
    struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/* Datasheet §7.2.2: hold NRESET low for more than 100 us, then wait 5 ms.
 * The only blocking wait in the radio code, and it runs before any event
 * loop exists. */
static int pulse_reset(void *ctx)
{
    sx1278_linux_t *h = ctx;
    if (set_line(h->reset_fd, 0) != 0) {
        return -1;
    }
    sleep_ms(1);
    if (set_line(h->reset_fd, 1) != 0) {
        return -1;
    }
    sleep_ms(10);
    return 0;
}

static int request_line(int chip_fd, uint32_t offset, const char *consumer,
                        uint64_t flags, int initial_high)
{
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = offset;
    req.num_lines = 1;
    strncpy(req.consumer, consumer, sizeof(req.consumer) - 1);
    req.config.flags = flags;
    if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
        req.config.num_attrs = 1;
        req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = initial_high ? 1u : 0u;
        req.config.attrs[0].mask = 1u;
    }
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        return -1;
    }
    return req.fd;
}

int sx1278_linux_open(sx1278_linux_t *h, const sx1278_linux_cfg_t *cfg,
                      sx1278_bus_t *bus, const char **why)
{
    h->spi_fd = h->reset_fd = h->dio0_fd = -1;
    h->spi_hz = cfg->spi_hz;

    h->spi_fd = open(cfg->spi_dev, O_RDWR | O_CLOEXEC);
    if (h->spi_fd < 0) {
        *why = "cannot open SPI device";
        return -1;
    }
    uint8_t mode = SPI_MODE_0, bits = 8;
    if (ioctl(h->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(h->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(h->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &h->spi_hz) < 0) {
        *why = "cannot configure SPI";
        sx1278_linux_close(h);
        return -1;
    }

    int chip = open(cfg->gpio_chip, O_RDWR | O_CLOEXEC);
    if (chip < 0) {
        *why = "cannot open GPIO chip";
        sx1278_linux_close(h);
        return -1;
    }
    /* NRESET idles high: the chip runs. */
    h->reset_fd = request_line(chip, cfg->reset_line, "ttec-sx1278-reset",
                               GPIO_V2_LINE_FLAG_OUTPUT, 1);
    h->dio0_fd = request_line(chip, cfg->dio0_line, "ttec-sx1278-dio0",
                              GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING, 0);
    close(chip);                 /* the line requests outlive the chip fd */
    if (h->reset_fd < 0 || h->dio0_fd < 0) {
        *why = "cannot request GPIO lines (in use, or wrong offsets)";
        sx1278_linux_close(h);
        return -1;
    }
    fcntl(h->dio0_fd, F_SETFL, fcntl(h->dio0_fd, F_GETFL) | O_NONBLOCK);

    bus->ctx = h;
    bus->xfer = spi_xfer;
    bus->reset = pulse_reset;
    return 0;
}

int sx1278_linux_irq_fd(const sx1278_linux_t *h)
{
    return h->dio0_fd;
}

void sx1278_linux_irq_drain(sx1278_linux_t *h)
{
    struct gpio_v2_line_event ev[8];
    while (read(h->dio0_fd, ev, sizeof(ev)) > 0) {
    }
}

void sx1278_linux_close(sx1278_linux_t *h)
{
    if (h->dio0_fd >= 0)  { close(h->dio0_fd); }
    if (h->reset_fd >= 0) { close(h->reset_fd); }
    if (h->spi_fd >= 0)   { close(h->spi_fd); }
    h->spi_fd = h->reset_fd = h->dio0_fd = -1;
}
