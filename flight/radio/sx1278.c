#include "sx1278.h"
#include "gama_lora.h"

#include <string.h>

/* RegModemConfig1 bandwidth codes (bits 7:4), by bandwidth in Hz. */
static int bw_code(uint32_t bw_hz)
{
    switch (bw_hz) {
    case 7800u:   return 0;  case 10400u:  return 1;  case 15600u: return 2;
    case 20800u:  return 3;  case 31250u:  return 4;  case 41700u: return 5;
    case 62500u:  return 6;  case 125000u: return 7;  case 250000u: return 8;
    case 500000u: return 9;
    default:      return -1;
    }
}

int sx1278_read(sx1278_t *d, uint8_t reg, uint8_t *val)
{
    uint8_t tx[2] = { (uint8_t)(reg & 0x7Fu), 0 }, rx[2] = { 0, 0 };
    if (d->bus.xfer(d->bus.ctx, tx, rx, 2) != 0) {
        return -1;
    }
    *val = rx[1];
    return 0;
}

int sx1278_write(sx1278_t *d, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(reg | 0x80u), val }, rx[2];
    return d->bus.xfer(d->bus.ctx, tx, rx, 2);
}

/* Burst access to the FIFO: the address does not auto-increment for RegFifo,
 * so one transaction moves the whole packet. */
static int fifo_write(sx1278_t *d, const uint8_t *buf, uint8_t len)
{
    uint8_t tx[1 + GAMA_LORA_MAX_PAYLOAD], rx[1 + GAMA_LORA_MAX_PAYLOAD];
    tx[0] = SX_REG_FIFO | 0x80u;
    memcpy(tx + 1, buf, len);
    return d->bus.xfer(d->bus.ctx, tx, rx, (size_t)len + 1u);
}

static int fifo_read(sx1278_t *d, uint8_t *buf, uint8_t len)
{
    uint8_t tx[1 + GAMA_LORA_MAX_PAYLOAD], rx[1 + GAMA_LORA_MAX_PAYLOAD];
    memset(tx, 0, (size_t)len + 1u);
    tx[0] = SX_REG_FIFO;
    if (d->bus.xfer(d->bus.ctx, tx, rx, (size_t)len + 1u) != 0) {
        return -1;
    }
    memcpy(buf, rx + 1, len);
    return 0;
}

static int set_mode(sx1278_t *d, uint8_t mode)
{
    return sx1278_write(d, SX_REG_OP_MODE, (uint8_t)(SX_MODE_LORA | SX_MODE_LOW_FREQ | mode));
}

/* Continuous reception, DIO0 mapped to RxDone. */
static int start_rx(sx1278_t *d)
{
    if (sx1278_write(d, SX_REG_DIO_MAPPING_1, 0x00) != 0 ||
        sx1278_write(d, SX_REG_FIFO_ADDR_PTR, 0x00) != 0) {
        return -1;
    }
    return set_mode(d, SX_MODE_RX_CONT);
}

int sx1278_profile_registers(uint8_t profile, uint8_t *cfg1, uint8_t *cfg2, uint8_t *cfg3)
{
    const gama_lora_phy_t *phy = gama_lora_profile(profile);
    if (phy == NULL) {
        return -1;
    }
    int bw = bw_code(phy->bw_hz);
    if (bw < 0) {
        return -1;
    }
    *cfg1 = (uint8_t)(((unsigned)bw << 4) | ((unsigned)phy->cr << 1) |
                      (phy->implicit_header ? 1u : 0u));
    *cfg2 = (uint8_t)(((unsigned)phy->sf << 4) | (phy->crc_on ? 0x04u : 0u));
    /* LowDataRateOptimize (bit 3) is derived from the profile every time,
     * never left over from a previous one; AGC (bit 2) always on. */
    *cfg3 = (uint8_t)((gama_lora_ldro(phy) ? 0x08u : 0u) | 0x04u);
    return 0;
}

static int write_profile(sx1278_t *d, uint8_t profile)
{
    uint8_t c1, c2, c3;
    const gama_lora_phy_t *phy = gama_lora_profile(profile);
    if (sx1278_profile_registers(profile, &c1, &c2, &c3) != 0) {
        return -1;
    }
    if (sx1278_write(d, SX_REG_MODEM_CONFIG_1, c1) != 0 ||
        sx1278_write(d, SX_REG_MODEM_CONFIG_2, c2) != 0 ||
        sx1278_write(d, SX_REG_MODEM_CONFIG_3, c3) != 0 ||
        sx1278_write(d, SX_REG_PREAMBLE_MSB, (uint8_t)(phy->preamble_len >> 8)) != 0 ||
        sx1278_write(d, SX_REG_PREAMBLE_LSB, (uint8_t)(phy->preamble_len & 0xFFu)) != 0) {
        return -1;
    }
    d->profile = profile;
    return 0;
}

int sx1278_set_profile(sx1278_t *d, uint8_t profile)
{
    if (d->transmitting || gama_lora_profile(profile) == NULL) {
        return -1;
    }
    if (set_mode(d, SX_MODE_STDBY) != 0 || write_profile(d, profile) != 0) {
        return -1;
    }
    return start_rx(d);
}

int sx1278_set_power(sx1278_t *d, int8_t dbm)
{
    if (dbm < 2 || dbm > 20) {
        return -1;
    }
    /* PA_BOOST. Up to 17 dBm: Pout = 2 + OutputPower, default PA DAC, 100 mA
     * current limit. Above: the +20 dBm DAC setting, Pout = 5 + OutputPower,
     * and a 140 mA current limit so the PA is not starved. */
    bool high = dbm > 17;
    uint8_t out = (uint8_t)(high ? dbm - 5 : dbm - 2);
    uint8_t ocp = high ? (uint8_t)(0x20u | 17u)    /* -30 + 10 * 17 = 140 mA */
                       : (uint8_t)(0x20u | 11u);   /*  45 +  5 * 11 = 100 mA */
    if (sx1278_write(d, SX_REG_PA_CONFIG, (uint8_t)(0x80u | 0x70u | out)) != 0 ||
        sx1278_write(d, SX_REG_PA_DAC, high ? 0x87u : 0x84u) != 0 ||
        sx1278_write(d, SX_REG_OCP, ocp) != 0) {
        return -1;
    }
    d->power_dbm = dbm;
    return 0;
}

int sx1278_init(sx1278_t *d, const sx1278_bus_t *bus, uint8_t profile,
                int8_t power_dbm, sx1278_lbt_t lbt, const char **why)
{
    static const char *dummy;
    if (why == NULL) {
        why = &dummy;
    }
    memset(d, 0, sizeof(*d));
    d->bus = *bus;
    d->lbt = lbt;

    if (d->bus.reset != NULL && d->bus.reset(d->bus.ctx) != 0) {
        *why = "reset failed";
        return -1;
    }
    uint8_t version = 0;
    if (sx1278_read(d, SX_REG_VERSION, &version) != 0) {
        *why = "SPI read failed";
        return -1;
    }
    if (version != SX_VERSION) {
        *why = "unexpected RegVersion (no chip, or wiring)";
        return -1;
    }

    /* LongRangeMode can only be changed in sleep. */
    uint64_t frf = ((uint64_t)GAMA_LORA_FREQ_HZ << 19) / 32000000u;
    if (sx1278_write(d, SX_REG_OP_MODE, SX_MODE_SLEEP | SX_MODE_LOW_FREQ) != 0 ||
        set_mode(d, SX_MODE_SLEEP) != 0 ||
        sx1278_write(d, SX_REG_FRF_MSB, (uint8_t)(frf >> 16)) != 0 ||
        sx1278_write(d, SX_REG_FRF_MID, (uint8_t)(frf >> 8)) != 0 ||
        sx1278_write(d, SX_REG_FRF_LSB, (uint8_t)frf) != 0 ||
        sx1278_write(d, SX_REG_SYNC_WORD, GAMA_LORA_SYNC_WORD) != 0 ||
        sx1278_write(d, SX_REG_FIFO_TX_BASE, 0x00) != 0 ||
        sx1278_write(d, SX_REG_FIFO_RX_BASE, 0x00) != 0 ||
        sx1278_write(d, SX_REG_DETECT_OPTIMIZE, 0xC3) != 0 ||    /* SF7..12 */
        sx1278_write(d, SX_REG_DETECTION_THRESH, 0x0A) != 0 ||   /* SF7..12 */
        set_mode(d, SX_MODE_STDBY) != 0 ||
        write_profile(d, profile) != 0) {
        *why = "configuration write failed";
        return -1;
    }
    if (sx1278_set_power(d, power_dbm) != 0) {
        *why = "power out of range";
        return -1;
    }
    if (sx1278_write(d, SX_REG_IRQ_FLAGS, 0xFF) != 0 || start_rx(d) != 0) {
        *why = "could not start reception";
        return -1;
    }
    return 0;
}

int sx1278_rx_busy(sx1278_t *d, bool *busy)
{
    uint8_t stat;
    if (sx1278_read(d, SX_REG_MODEM_STAT, &stat) != 0) {
        return -1;
    }
    /* Not SX_STAT_RX_ONGOING: on the real chip it stays set for as long as
     * the modem is in RX continuous, with an empty channel too (0x04 on both
     * RA-02, bench of 2026-10-09). Counting it, listen before talk never let
     * a frame out. */
    uint8_t mask = SX_STAT_SIGNAL_SYNCED | SX_STAT_HEADER_VALID;
    if (d->lbt == SX_LBT_PREAMBLE) {
        mask |= SX_STAT_SIGNAL_DETECTED;
    }
    *busy = (stat & mask) != 0;
    return 0;
}

int sx1278_transmit(sx1278_t *d, const uint8_t *buf, uint8_t len)
{
    if (d->transmitting || len == 0) {
        return -1;
    }
    if (set_mode(d, SX_MODE_STDBY) != 0 ||
        sx1278_write(d, SX_REG_DIO_MAPPING_1, 0x40) != 0 ||      /* DIO0: TxDone */
        sx1278_write(d, SX_REG_FIFO_ADDR_PTR, 0x00) != 0 ||
        fifo_write(d, buf, len) != 0 ||
        sx1278_write(d, SX_REG_PAYLOAD_LENGTH, len) != 0 ||
        sx1278_write(d, SX_REG_IRQ_FLAGS, 0xFF) != 0 ||
        set_mode(d, SX_MODE_TX) != 0) {
        start_rx(d);
        return -1;
    }
    d->transmitting = true;
    return 0;
}

int sx1278_service(sx1278_t *d, sx1278_event_t *event, uint8_t *buf, uint8_t *len,
                   int16_t *rssi_dbm, int8_t *snr_db)
{
    *event = SX_EVENT_NONE;
    uint8_t flags;
    /* Clear exactly the flags read, not 0xFF. DIO0 is a level that stays high
     * while any mapped flag is set, but the kernel delivers edges: a flag that
     * arrived between this read and a blanket clear would be erased, and since
     * DIO0 never fell, no new edge would ever announce it. */
    if (sx1278_read(d, SX_REG_IRQ_FLAGS, &flags) != 0) {
        return -1;
    }
    if (flags == 0) {
        return 0;
    }
    if (sx1278_write(d, SX_REG_IRQ_FLAGS, flags) != 0) {
        return -1;
    }

    if (d->transmitting && (flags & SX_IRQ_TX_DONE)) {
        d->transmitting = false;
        *event = SX_EVENT_TX_DONE;
        return start_rx(d);
    }
    if (flags & SX_IRQ_RX_DONE) {
        if (flags & SX_IRQ_CRC_ERROR) {
            *event = SX_EVENT_RX_CRC_ERROR;
            return 0;
        }
        uint8_t n, at, snr_raw, rssi_raw;
        if (sx1278_read(d, SX_REG_RX_NB_BYTES, &n) != 0 ||
            sx1278_read(d, SX_REG_FIFO_RX_CURRENT, &at) != 0 ||
            sx1278_write(d, SX_REG_FIFO_ADDR_PTR, at) != 0 ||
            fifo_read(d, buf, n) != 0 ||
            sx1278_read(d, SX_REG_PKT_SNR, &snr_raw) != 0 ||
            sx1278_read(d, SX_REG_PKT_RSSI, &rssi_raw) != 0) {
            return -1;
        }
        /* Datasheet §5.5.5, low-frequency port: RSSI = -164 + PacketRssi,
         * corrected by SNR / 4 when the packet is below the noise floor. */
        int8_t snr_q = (int8_t)snr_raw;             /* quarter dB */
        int rssi = -164 + (int)rssi_raw;
        if (snr_q < 0) {
            rssi += snr_q / 4;
        }
        *len = n;
        *snr_db = (int8_t)(snr_q / 4);
        *rssi_dbm = (int16_t)rssi;
        *event = SX_EVENT_RX;
        return 0;
    }
    return 0;
}
