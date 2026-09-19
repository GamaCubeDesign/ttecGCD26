/*
 * SX1278 driver against an emulated chip: a register file, a FIFO whose
 * address pointer auto-increments, and write-one-to-clear IRQ flags, as the
 * datasheet describes. What cannot be checked here — actual RF, timing, and
 * how soon RegModemStat reports a reception — is the bench's job (PLANO 2.7).
 */

#include "test_util.h"
#include "sx1278.h"
#include "gama_tc.h"

typedef struct {
    uint8_t regs[128];
    uint8_t fifo[256];
    int     resets;
    bool    fail;
} chip_t;

static chip_t CHIP;

static void chip_write(chip_t *c, uint8_t a, uint8_t v)
{
    if (a == SX_REG_FIFO) {
        c->fifo[c->regs[SX_REG_FIFO_ADDR_PTR]++] = v;
    } else if (a == SX_REG_IRQ_FLAGS) {
        c->regs[a] = (uint8_t)(c->regs[a] & ~v);     /* write one to clear */
    } else {
        c->regs[a] = v;
    }
}

static uint8_t chip_read(chip_t *c, uint8_t a)
{
    if (a == SX_REG_FIFO) {
        return c->fifo[c->regs[SX_REG_FIFO_ADDR_PTR]++];
    }
    return c->regs[a];
}

static int chip_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    chip_t *c = ctx;
    if (c->fail) {
        return -1;
    }
    uint8_t addr = tx[0] & 0x7Fu;
    bool write = (tx[0] & 0x80u) != 0;
    rx[0] = 0;
    for (size_t i = 1; i < len; i++) {
        /* Burst access auto-increments the address, except on the FIFO. */
        uint8_t a = addr == SX_REG_FIFO ? addr : (uint8_t)(addr + i - 1u);
        if (write) {
            chip_write(c, a, tx[i]);
        } else {
            rx[i] = chip_read(c, a);
        }
    }
    return 0;
}

static int chip_reset(void *ctx)
{
    chip_t *c = ctx;
    c->resets++;
    return 0;
}

static const sx1278_bus_t BUS = { .ctx = &CHIP, .xfer = chip_xfer, .reset = chip_reset };

static sx1278_t D;

static void power_on(void)
{
    memset(&CHIP, 0, sizeof(CHIP));
    CHIP.regs[SX_REG_VERSION] = SX_VERSION;
}

static int init_nominal(void)
{
    power_on();
    const char *why = NULL;
    return sx1278_init(&D, &BUS, GAMA_RATE_NOMINAL, 20, SX_LBT_HEADER, &why);
}

/* Places a received packet in the FIFO the way the modem does. */
static void inject_rx(const uint8_t *bytes, uint8_t len, uint8_t at,
                      uint8_t rssi_raw, int8_t snr_quarter_db, bool crc_error)
{
    memcpy(CHIP.fifo + at, bytes, len);
    CHIP.regs[SX_REG_FIFO_RX_CURRENT] = at;
    CHIP.regs[SX_REG_RX_NB_BYTES] = len;
    CHIP.regs[SX_REG_PKT_RSSI] = rssi_raw;
    CHIP.regs[SX_REG_PKT_SNR] = (uint8_t)snr_quarter_db;
    CHIP.regs[SX_REG_IRQ_FLAGS] |= SX_IRQ_RX_DONE | (crc_error ? SX_IRQ_CRC_ERROR : 0u);
}

int main(void)
{
    TEST_GROUP("sx1278: init configures 433 MHz LoRa at NOMINAL and listens");
    {
        CHECK_EQ_INT(init_nominal(), 0);
        CHECK_EQ_INT(CHIP.resets, 1);
        /* Frf = 433 MHz * 2^19 / 32 MHz = 7094272 = 0x6C4000. */
        CHECK_EQ_INT(CHIP.regs[SX_REG_FRF_MSB], 0x6C);
        CHECK_EQ_INT(CHIP.regs[SX_REG_FRF_MID], 0x40);
        CHECK_EQ_INT(CHIP.regs[SX_REG_FRF_LSB], 0x00);
        CHECK_EQ_INT(CHIP.regs[SX_REG_SYNC_WORD], 0x12);
        CHECK_EQ_INT(CHIP.regs[SX_REG_PREAMBLE_MSB], 0x00);
        CHECK_EQ_INT(CHIP.regs[SX_REG_PREAMBLE_LSB], 0x08);
        /* BW 125 kHz, CR 4/5, explicit header | SF9, CRC on | AGC, no LDRO */
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_1], 0x72);
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_2], 0x94);
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_3], 0x04);
        /* LoRa | low-frequency band | continuous reception, DIO0 on RxDone */
        CHECK_EQ_INT(CHIP.regs[SX_REG_OP_MODE], 0x8D);
        CHECK_EQ_INT(CHIP.regs[SX_REG_DIO_MAPPING_1], 0x00);
    }

    TEST_GROUP("sx1278: a missing or miswired chip is reported, not configured");
    {
        const char *why = NULL;
        power_on();
        CHIP.regs[SX_REG_VERSION] = 0x00;
        CHECK_EQ_INT(sx1278_init(&D, &BUS, GAMA_RATE_NOMINAL, 20, SX_LBT_HEADER, &why), -1);
        CHECK(why != NULL && strstr(why, "RegVersion") != NULL);
        CHIP.regs[SX_REG_VERSION] = 0xFF;
        CHECK_EQ_INT(sx1278_init(&D, &BUS, GAMA_RATE_NOMINAL, 20, SX_LBT_HEADER, &why), -1);
        CHIP.regs[SX_REG_VERSION] = SX_VERSION;
        CHIP.fail = true;
        CHECK_EQ_INT(sx1278_init(&D, &BUS, GAMA_RATE_NOMINAL, 20, SX_LBT_HEADER, &why), -1);
    }

    TEST_GROUP("sx1278: each profile writes its three modem registers");
    {
        CHECK_EQ_INT(init_nominal(), 0);
        CHECK_EQ_INT(sx1278_set_profile(&D, GAMA_RATE_SAFE), 0);
        /* BW 125, CR 4/8 | SF12, CRC | LDRO + AGC */
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_1], 0x78);
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_2], 0xC4);
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_3], 0x0C);
        CHECK_EQ_INT(CHIP.regs[SX_REG_OP_MODE], 0x8D);      /* back to listening */

        CHECK_EQ_INT(sx1278_set_profile(&D, GAMA_RATE_FAST), 0);
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_1], 0x72);
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_2], 0x74);
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_3], 0x04);

        CHECK_EQ_INT(sx1278_set_profile(&D, 7), -1);        /* no such profile */
        CHECK_EQ_INT(CHIP.regs[SX_REG_MODEM_CONFIG_2], 0x74);
    }

    TEST_GROUP("sx1278: LowDataRateOptimize follows every profile change");
    {
        /* The legacy driver rewrote this bit only inside send/receive, so a
         * configuration change left it stale until the next packet. */
        CHECK_EQ_INT(init_nominal(), 0);
        for (int i = 0; i < 3; i++) {
            sx1278_set_profile(&D, GAMA_RATE_SAFE);
            CHECK(CHIP.regs[SX_REG_MODEM_CONFIG_3] & 0x08u);
            sx1278_set_profile(&D, GAMA_RATE_NOMINAL);
            CHECK(!(CHIP.regs[SX_REG_MODEM_CONFIG_3] & 0x08u));
        }
    }

    TEST_GROUP("sx1278: output power, PA DAC and current limit together");
    {
        CHECK_EQ_INT(init_nominal(), 0);
        /* 20 dBm: +20 dBm DAC, OutputPower 15, 140 mA */
        CHECK_EQ_INT(CHIP.regs[SX_REG_PA_CONFIG], 0xFF);
        CHECK_EQ_INT(CHIP.regs[SX_REG_PA_DAC], 0x87);
        CHECK_EQ_INT(CHIP.regs[SX_REG_OCP], 0x31);

        CHECK_EQ_INT(sx1278_set_power(&D, 18), 0);
        CHECK_EQ_INT(CHIP.regs[SX_REG_PA_CONFIG], 0xFD);
        CHECK_EQ_INT(CHIP.regs[SX_REG_PA_DAC], 0x87);

        CHECK_EQ_INT(sx1278_set_power(&D, 17), 0);
        CHECK_EQ_INT(CHIP.regs[SX_REG_PA_CONFIG], 0xFF);
        CHECK_EQ_INT(CHIP.regs[SX_REG_PA_DAC], 0x84);
        CHECK_EQ_INT(CHIP.regs[SX_REG_OCP], 0x2B);          /* 100 mA */

        CHECK_EQ_INT(sx1278_set_power(&D, 2), 0);            /* bench power */
        CHECK_EQ_INT(CHIP.regs[SX_REG_PA_CONFIG], 0xF0);

        CHECK_EQ_INT(sx1278_set_power(&D, 1), -1);
        CHECK_EQ_INT(sx1278_set_power(&D, 21), -1);
        CHECK_EQ_INT(D.power_dbm, 2);
    }

    TEST_GROUP("sx1278: transmit loads the FIFO and starts, without waiting");
    {
        CHECK_EQ_INT(init_nominal(), 0);
        const uint8_t pkt[] = { 0x01, 0x10, 0x05, 0x00, 0x00, 0xAB, 0xCD };
        CHECK_EQ_INT(sx1278_transmit(&D, pkt, sizeof(pkt)), 0);
        CHECK_MEM_EQ(CHIP.fifo, pkt, sizeof(pkt));
        CHECK_EQ_INT(CHIP.regs[SX_REG_PAYLOAD_LENGTH], sizeof(pkt));
        CHECK_EQ_INT(CHIP.regs[SX_REG_DIO_MAPPING_1], 0x40);   /* DIO0: TxDone */
        CHECK_EQ_INT(CHIP.regs[SX_REG_OP_MODE], 0x8B);          /* TX */
        CHECK(D.transmitting);

        /* Half-duplex: nothing else may touch the modem until TxDone. */
        CHECK_EQ_INT(sx1278_transmit(&D, pkt, sizeof(pkt)), -1);
        CHECK_EQ_INT(sx1278_set_profile(&D, GAMA_RATE_FAST), -1);
    }

    TEST_GROUP("sx1278: TxDone returns the modem to reception");
    {
        CHIP.regs[SX_REG_IRQ_FLAGS] |= SX_IRQ_TX_DONE;
        sx1278_event_t ev; uint8_t buf[256], len; int16_t rssi; int8_t snr;
        CHECK_EQ_INT(sx1278_service(&D, &ev, buf, &len, &rssi, &snr), 0);
        CHECK_EQ_INT(ev, SX_EVENT_TX_DONE);
        CHECK(!D.transmitting);
        CHECK_EQ_INT(CHIP.regs[SX_REG_OP_MODE], 0x8D);
        CHECK_EQ_INT(CHIP.regs[SX_REG_DIO_MAPPING_1], 0x00);
        CHECK_EQ_INT(CHIP.regs[SX_REG_IRQ_FLAGS], 0x00);         /* cleared */
    }

    TEST_GROUP("sx1278: a received packet comes out with RSSI and SNR");
    {
        CHECK_EQ_INT(init_nominal(), 0);
        uint8_t pkt[40];
        for (int i = 0; i < 40; i++) { pkt[i] = (uint8_t)(i * 3 + 1); }
        inject_rx(pkt, sizeof(pkt), 0x20, 100, 40, false);       /* +10 dB SNR */
        sx1278_event_t ev; uint8_t buf[256], len; int16_t rssi; int8_t snr;
        CHECK_EQ_INT(sx1278_service(&D, &ev, buf, &len, &rssi, &snr), 0);
        CHECK_EQ_INT(ev, SX_EVENT_RX);
        CHECK_EQ_INT(len, 40);
        CHECK_MEM_EQ(buf, pkt, 40);
        CHECK_EQ_INT(rssi, -64);                                 /* -164 + 100 */
        CHECK_EQ_INT(snr, 10);

        inject_rx(pkt, 10, 0x80, 30, -20, false);                /* -5 dB SNR */
        CHECK_EQ_INT(sx1278_service(&D, &ev, buf, &len, &rssi, &snr), 0);
        CHECK_EQ_INT(snr, -5);
        CHECK_EQ_INT(rssi, -164 + 30 - 5);                       /* SNR-corrected */
    }

    TEST_GROUP("sx1278: a PHY CRC failure is reported, never delivered");
    {
        CHECK_EQ_INT(init_nominal(), 0);
        uint8_t pkt[8] = { 0 };
        inject_rx(pkt, 8, 0, 90, 20, true);
        sx1278_event_t ev; uint8_t buf[256], len = 0; int16_t rssi; int8_t snr;
        CHECK_EQ_INT(sx1278_service(&D, &ev, buf, &len, &rssi, &snr), 0);
        CHECK_EQ_INT(ev, SX_EVENT_RX_CRC_ERROR);
        CHECK_EQ_INT(len, 0);
        CHECK_EQ_INT(sx1278_service(&D, &ev, buf, &len, &rssi, &snr), 0);
        CHECK_EQ_INT(ev, SX_EVENT_NONE);                         /* flags cleared */
    }

    TEST_GROUP("sx1278: a flag arriving during service is not erased");
    {
        /* Only the flags that were read get cleared, so one that arrives in
         * between survives to be serviced on the next call. */
        CHECK_EQ_INT(init_nominal(), 0);
        uint8_t pkt[4] = { 1, 2, 3, 4 };
        inject_rx(pkt, 4, 0, 90, 20, false);
        CHIP.regs[SX_REG_IRQ_FLAGS] |= SX_IRQ_VALID_HEADER;   /* not ours */
        sx1278_event_t ev; uint8_t buf[256], len; int16_t rssi; int8_t snr;
        CHECK_EQ_INT(sx1278_service(&D, &ev, buf, &len, &rssi, &snr), 0);
        CHECK_EQ_INT(ev, SX_EVENT_RX);
        CHECK_EQ_INT(CHIP.regs[SX_REG_IRQ_FLAGS], 0x00);       /* read, then cleared */
    }

    TEST_GROUP("sx1278: listen before talk, in both sensitivities");
    {
        bool busy;
        CHECK_EQ_INT(init_nominal(), 0);
        CHIP.regs[SX_REG_MODEM_STAT] = 0x00;
        sx1278_rx_busy(&D, &busy); CHECK(!busy);
        CHIP.regs[SX_REG_MODEM_STAT] = SX_STAT_HEADER_VALID;
        sx1278_rx_busy(&D, &busy); CHECK(busy);
        CHIP.regs[SX_REG_MODEM_STAT] = SX_STAT_SIGNAL_DETECTED;
        sx1278_rx_busy(&D, &busy); CHECK(!busy);                 /* HEADER mode */
        D.lbt = SX_LBT_PREAMBLE;
        sx1278_rx_busy(&D, &busy); CHECK(busy);                  /* PREAMBLE mode */
    }

    TEST_SUMMARY("test_sx1278");
}
