/*
 * sx1278.h — register-level driver for the Semtech SX1278 in LoRa mode.
 *
 * Ported from ultima_missao/satellite/LoRa.c, which is kept as reference.
 * What changed, and why:
 *
 *   - The register logic talks to an abstract bus (sx1278_bus_t), not to
 *     pigpio. It can then be tested against an emulated register file on the
 *     development host (tests/test_sx1278.c), and the same driver runs on the
 *     flight Pi, on the bench Pi and in any future target.
 *   - Rate changes go through sx1278_set_profile(), which writes all three
 *     modem configuration registers together — including LowDataRateOptimize,
 *     which the legacy driver only rewrote inside LoRa_send()/LoRa_receive(),
 *     leaving it stale after a configuration change until the next packet.
 *   - Nothing sleeps. The legacy driver waited sleep(Tpkt/1000 + 1) for every
 *     transmission to end (ultima_missao/satellite/Moden.cpp:91); here the end
 *     of a transmission is an interrupt on DIO0, serviced by
 *     sx1278_service() when the caller's event loop sees it. The only wait is
 *     the reset pulse in sx1278_init(), before any loop starts.
 *
 * Only the three profiles of common/gama_lora.h can be configured: there is
 * no API to set SF, bandwidth or coding rate individually (ADR-0004).
 */

#ifndef TTEC_SX1278_H
#define TTEC_SX1278_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers used, SX1276/77/78/79 datasheet rev. 7, table 41 (LoRa mode). */
enum {
    SX_REG_FIFO               = 0x00,
    SX_REG_OP_MODE            = 0x01,
    SX_REG_FRF_MSB            = 0x06,
    SX_REG_FRF_MID            = 0x07,
    SX_REG_FRF_LSB            = 0x08,
    SX_REG_PA_CONFIG          = 0x09,
    SX_REG_OCP                = 0x0B,
    SX_REG_FIFO_ADDR_PTR      = 0x0D,
    SX_REG_FIFO_TX_BASE       = 0x0E,
    SX_REG_FIFO_RX_BASE       = 0x0F,
    SX_REG_FIFO_RX_CURRENT    = 0x10,
    SX_REG_IRQ_FLAGS          = 0x12,
    SX_REG_RX_NB_BYTES        = 0x13,
    SX_REG_MODEM_STAT         = 0x18,
    SX_REG_PKT_SNR            = 0x19,
    SX_REG_PKT_RSSI           = 0x1A,
    SX_REG_MODEM_CONFIG_1     = 0x1D,
    SX_REG_MODEM_CONFIG_2     = 0x1E,
    SX_REG_PREAMBLE_MSB       = 0x20,
    SX_REG_PREAMBLE_LSB       = 0x21,
    SX_REG_PAYLOAD_LENGTH     = 0x22,
    SX_REG_MODEM_CONFIG_3     = 0x26,
    SX_REG_DETECT_OPTIMIZE    = 0x31,
    SX_REG_DETECTION_THRESH   = 0x37,
    SX_REG_SYNC_WORD          = 0x39,
    SX_REG_DIO_MAPPING_1      = 0x40,
    SX_REG_VERSION            = 0x42,
    SX_REG_PA_DAC             = 0x4D
};

/* RegOpMode */
#define SX_MODE_LORA        0x80u
#define SX_MODE_LOW_FREQ    0x08u   /* LowFrequencyModeOn: the 433 MHz band */
#define SX_MODE_SLEEP       0x00u
#define SX_MODE_STDBY       0x01u
#define SX_MODE_TX          0x03u
#define SX_MODE_RX_CONT     0x05u

/* RegIrqFlags */
#define SX_IRQ_RX_TIMEOUT   0x80u
#define SX_IRQ_RX_DONE      0x40u
#define SX_IRQ_CRC_ERROR    0x20u
#define SX_IRQ_VALID_HEADER 0x10u
#define SX_IRQ_TX_DONE      0x08u

/* RegModemStat */
#define SX_STAT_SIGNAL_DETECTED 0x01u
#define SX_STAT_SIGNAL_SYNCED   0x02u
#define SX_STAT_RX_ONGOING      0x04u
#define SX_STAT_HEADER_VALID    0x08u

/* RegVersion of every SX1276/77/78/79. Anything else is no chip, or a
 * miswired one — reading 0x00 or 0xFF is the usual symptom. */
#define SX_VERSION          0x12u

/* One SPI transaction with chip select held for its whole length. */
typedef struct {
    void *ctx;
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    /* Pulses NRESET low and waits for the chip to come up. Blocking; called
     * only from sx1278_init(), before any event loop runs. */
    int (*reset)(void *ctx);
} sx1278_bus_t;

/*
 * Listen-before-talk sensitivity (ADR-0007). HEADER reports a reception once
 * the modem is synchronised or has a header — the pessimistic model the
 * link simulation assumes. PREAMBLE also counts "signal detected", which the
 * datasheet sets on preamble detection; if the bench confirms it asserts
 * within a few symbols, it shrinks the collision window about fourfold.
 */
typedef enum {
    SX_LBT_HEADER   = 0,
    SX_LBT_PREAMBLE = 1
} sx1278_lbt_t;

typedef struct {
    sx1278_bus_t bus;
    uint8_t      profile;
    int8_t       power_dbm;
    sx1278_lbt_t lbt;
    bool         transmitting;
} sx1278_t;

typedef enum {
    SX_EVENT_NONE = 0,
    SX_EVENT_RX,            /* a packet with a good PHY CRC */
    SX_EVENT_RX_CRC_ERROR,  /* a packet that failed the PHY CRC */
    SX_EVENT_TX_DONE
} sx1278_event_t;

/* Resets the chip, checks its version, and configures it: LoRa mode, 433 MHz,
 * sync word, preamble, the given profile and power, then continuous reception.
 * Returns 0, or -1 with the reason in *why (never NULL on failure). */
int sx1278_init(sx1278_t *d, const sx1278_bus_t *bus, uint8_t profile,
                int8_t power_dbm, sx1278_lbt_t lbt, const char **why);

/* Reconfigures the modem for one of the three profiles and resumes reception.
 * Refused while transmitting. */
int sx1278_set_profile(sx1278_t *d, uint8_t profile);

/* PA_BOOST output power, 2 .. 20 dBm, with the matching PA DAC and current
 * limit. Refused out of range. */
int sx1278_set_power(sx1278_t *d, int8_t dbm);

/* Whether the modem is receiving a packet right now (listen before talk). */
int sx1278_rx_busy(sx1278_t *d, bool *busy);

/* Loads the FIFO and starts transmitting. Returns at once; the end is a
 * DIO0 interrupt reported by sx1278_service(). */
int sx1278_transmit(sx1278_t *d, const uint8_t *buf, uint8_t len);

/*
 * Services a DIO0 interrupt: reads and clears the IRQ flags and reports what
 * happened. On RX, copies the packet into buf (at least 255 bytes) and sets
 * *len, *rssi_dbm and *snr_db. On TX_DONE, returns the modem to continuous
 * reception. Calling it with nothing pending returns SX_EVENT_NONE.
 */
int sx1278_service(sx1278_t *d, sx1278_event_t *event, uint8_t *buf, uint8_t *len,
                   int16_t *rssi_dbm, int8_t *snr_db);

/* The three RegModemConfig values for a profile, exposed for the tests and
 * the bench's register dump. Returns -1 for an undefined profile. */
int sx1278_profile_registers(uint8_t profile, uint8_t *cfg1, uint8_t *cfg2, uint8_t *cfg3);

int sx1278_read(sx1278_t *d, uint8_t reg, uint8_t *val);
int sx1278_write(sx1278_t *d, uint8_t reg, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* TTEC_SX1278_H */
