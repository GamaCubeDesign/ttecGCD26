/*
 * obcd's drivers without the hardware: the INA219 against an emulated chip,
 * the DS18B20 against the text the kernel's w1_therm writes, and against a
 * fake w1_slave file (OBC-7, HLR-EPS-04).
 */

#include "test_util.h"
#include "ds18b20.h"
#include "ina219.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* An INA219: its registers, and a bus that can be made to fail. */
static struct {
    uint16_t reg[8];
    int      writes;
    bool     fail;
} CHIP;

static int chip_write(void *ctx, uint8_t reg, uint16_t v)
{
    (void)ctx;
    if (CHIP.fail || reg > 7) { return -1; }
    CHIP.reg[reg] = v;
    CHIP.writes++;
    return 0;
}

static int chip_read(void *ctx, uint8_t reg, uint16_t *v)
{
    (void)ctx;
    if (CHIP.fail || reg > 7) { return -1; }
    *v = CHIP.reg[reg];
    return 0;
}

static const ina219_bus_t BUS = { .ctx = NULL, .write_reg16 = chip_write, .read_reg16 = chip_read };

/* What the chip would hold for this bus voltage and shunt voltage. */
static void chip_set(uint32_t bus_mv, int32_t shunt_uv)
{
    CHIP.reg[INA219_REG_BUS] = (uint16_t)((bus_mv / 4u) << 3 | 0x0002u);   /* CNVR set */
    CHIP.reg[INA219_REG_SHUNT] = (uint16_t)(int16_t)(shunt_uv / 10);
}

static const char *W1_GOOD =
    "72 01 4b 46 7f ff 0e 10 57 : crc=57 YES\n"
    "72 01 4b 46 7f ff 0e 10 57 t=23125\n";

int main(void)
{
    uint16_t mv;
    int16_t ma;

    TEST_GROUP("ina219: init writes the configuration of the previous OBC, 0x399F");
    {
        memset(&CHIP, 0, sizeof(CHIP));
        CHECK_EQ_INT(ina219_init(&BUS), 0);
        CHECK_EQ_INT(CHIP.reg[INA219_REG_CONFIG], 0x399F);
        CHIP.fail = true;
        CHECK_EQ_INT(ina219_init(&BUS), -1);
    }

    TEST_GROUP("ina219: a 2S battery at 8.2 V feeding 500 mA through 100 mOhm");
    {
        memset(&CHIP, 0, sizeof(CHIP));
        chip_set(8200, 50000);                     /* 0.5 A x 0.1 Ohm = 50 mV */
        CHECK_EQ_INT(ina219_read(&BUS, 100, &mv, &ma), 0);
        CHECK_EQ_INT(mv, 8200);
        CHECK_EQ_INT(ma, 500);
    }

    TEST_GROUP("ina219: the bus voltage is in 4 mV steps; the current rounds to the nearest mA");
    {
        chip_set(7403, 1240);                      /* 7403 -> 7400 mV; 12.4 mA */
        CHECK_EQ_INT(ina219_read(&BUS, 100, &mv, &ma), 0);
        CHECK_EQ_INT(mv, 7400);
        CHECK_EQ_INT(ma, 12);
        chip_set(7400, 1260);                      /* 12.6 mA */
        ina219_read(&BUS, 100, &mv, &ma);
        CHECK_EQ_INT(ma, 13);
    }

    TEST_GROUP("ina219: current against the load's direction reads negative");
    {
        chip_set(8000, -20000);
        CHECK_EQ_INT(ina219_read(&BUS, 100, &mv, &ma), 0);
        CHECK_EQ_INT(ma, -200);
    }

    TEST_GROUP("ina219: full scale with a small shunt stays clear of 'no data'");
    {
        chip_set(8000, -320000);                   /* -320 mV over 1 mOhm: -320 A */
        CHECK_EQ_INT(ina219_read(&BUS, 1, &mv, &ma), 0);
        CHECK(ma != INT16_MIN);                    /* GAMA_NO_DATA_I16 is reserved */
        CHECK_EQ_INT(ma, INT16_MIN + 1);
    }

    TEST_GROUP("ina219: overflow and bus errors are failures, never 0 V");
    {
        chip_set(8000, 1000);
        CHIP.reg[INA219_REG_BUS] |= 0x0001u;       /* OVF */
        CHECK_EQ_INT(ina219_read(&BUS, 100, &mv, &ma), -1);
        chip_set(8000, 1000);
        CHIP.fail = true;
        CHECK_EQ_INT(ina219_read(&BUS, 100, &mv, &ma), -1);
        CHIP.fail = false;
        CHECK_EQ_INT(ina219_read(&BUS, 0, &mv, &ma), -1);   /* no shunt: no current */
    }

    int16_t c;

    TEST_GROUP("ds18b20: the kernel's two lines, to hundredths of a degree");
    {
        CHECK_EQ_INT(ds18b20_parse(W1_GOOD, strlen(W1_GOOD), &c), DS18B20_OK);
        CHECK_EQ_INT(c, 2313);                     /* 23.125 rounds to 23.13 */
        const char *neg = "90 fe 4b 46 7f ff 0c 10 1c : crc=1c YES\n"
                          "90 fe 4b 46 7f ff 0c 10 1c t=-23125\n";
        CHECK_EQ_INT(ds18b20_parse(neg, strlen(neg), &c), DS18B20_OK);
        CHECK_EQ_INT(c, -2313);
        const char *zero = "00 00 4b 46 7f ff 0c 10 00 : crc=00 YES\nxx t=0\n";
        CHECK_EQ_INT(ds18b20_parse(zero, strlen(zero), &c), DS18B20_OK);
        CHECK_EQ_INT(c, 0);
    }

    TEST_GROUP("ds18b20: a CRC failure, the power-on value and garbage are refused");
    {
        const char *crc = "72 01 4b 46 7f ff 0e 10 57 : crc=58 NO\n"
                          "72 01 4b 46 7f ff 0e 10 57 t=23125\n";
        CHECK_EQ_INT(ds18b20_parse(crc, strlen(crc), &c), DS18B20_ERR_CRC);
        const char *por = "50 05 4b 46 7f ff 0c 10 1c : crc=1c YES\n"
                          "50 05 4b 46 7f ff 0c 10 1c t=85000\n";
        CHECK_EQ_INT(ds18b20_parse(por, strlen(por), &c), DS18B20_ERR_POWER_ON);
        const char *hot = "aa : crc=aa YES\naa t=130000\n";
        CHECK_EQ_INT(ds18b20_parse(hot, strlen(hot), &c), DS18B20_ERR_RANGE);
        CHECK_EQ_INT(ds18b20_parse("", 0, &c), DS18B20_ERR_FORMAT);
        CHECK_EQ_INT(ds18b20_parse("no newline", 10, &c), DS18B20_ERR_FORMAT);
        const char *cut = "72 01 : crc=57 YES\n72 01 t=";
        CHECK_EQ_INT(ds18b20_parse(cut, strlen(cut), &c), DS18B20_ERR_FORMAT);
        const char *junk = "72 01 : crc=57 YES\n72 01 t=12a\n";
        CHECK_EQ_INT(ds18b20_parse(junk, strlen(junk), &c), DS18B20_ERR_FORMAT);
    }

    TEST_GROUP("ds18b20: read from a w1_slave file, and a missing one is an I/O error");
    {
        char path[] = "/tmp/w1_slave-XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(write(fd, W1_GOOD, strlen(W1_GOOD)) == (ssize_t)strlen(W1_GOOD));
            close(fd);
            CHECK_EQ_INT(ds18b20_read_file(path, &c), DS18B20_OK);
            CHECK_EQ_INT(c, 2313);
            unlink(path);
        }
        CHECK_EQ_INT(ds18b20_read_file(path, &c), DS18B20_ERR_IO);
        CHECK_STR_EQ(ds18b20_result_name(DS18B20_ERR_CRC), "CRC");
    }

    TEST_SUMMARY("test_obcd_drivers");
}
