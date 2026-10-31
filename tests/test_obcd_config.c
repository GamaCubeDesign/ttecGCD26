/*
 * obcd's configuration: the PLANO's defaults, no pin compiled in, and every
 * key range-checked (AGENTS.md, hard rule 4).
 */

#include "test_util.h"
#include "config.h"

int main(void)
{
    char err[300];
    obcd_config_t c;

    TEST_GROUP("obcd config: defaults are the PLANO's, and no pin or bus is compiled in");
    {
        obcd_config_default(&c);
        CHECK_EQ_INT(c.p.adsb_timeout, 600000);          /* OBC-6 */
        CHECK_EQ_INT(c.p.burnwire_on, 5000);             /* D3 */
        CHECK_EQ_INT(c.p.heater_on_below, 1000);         /* D4 */
        CHECK_EQ_INT(c.p.heater_off_above, 1200);
        CHECK_EQ_INT(c.p.telemetry_period, 1000);        /* D6 */
        CHECK_EQ_INT(c.burnwire_line, OBCD_LINE_NONE);
        CHECK_EQ_INT(c.heater_line, OBCD_LINE_NONE);
        CHECK_STR_EQ(c.i2c_dev, "");
        CHECK_STR_EQ(c.w1_path, "");
        CHECK_EQ_INT(obcd_config_validate(&c, err, sizeof(err)), 0);
    }

    TEST_GROUP("obcd config: a file with the bench's wiring");
    {
        obcd_config_default(&c);
        const char *text =
            "# bench\n"
            "i2c_dev = /dev/i2c-1\n"
            "ina219_addr = 0x41          # A0 high\n"
            "w1_path = /sys/bus/w1/devices/28-0000/w1_slave\n"
            "burnwire_line = 22\n"
            "heater_line = 23\n"
            "heater_on_below_ccel = -500\n"
            "heater_off_above_ccel = 300\n";
        CHECK_EQ_INT(obcd_config_parse(&c, text, err, sizeof(err)), 0);
        CHECK_EQ_INT(c.ina219_addr, 0x41);
        CHECK_EQ_INT(c.burnwire_line, 22);
        CHECK_EQ_INT(c.heater_line, 23);
        CHECK_EQ_INT(c.p.heater_on_below, -500);
        CHECK_EQ_INT(obcd_config_validate(&c, err, sizeof(err)), 0);
        CHECK_EQ_INT(obcd_config_set(&c, "burnwire_line", "none", err, sizeof(err)), 0);
        CHECK_EQ_INT(c.burnwire_line, OBCD_LINE_NONE);
    }

    TEST_GROUP("obcd config: unknown keys, bad numbers and contradictions are errors");
    {
        obcd_config_default(&c);
        CHECK_EQ_INT(obcd_config_parse(&c, "burn_wire_line = 22\n", err, sizeof(err)), -1);
        CHECK(strstr(err, "line 1") != NULL);
        CHECK_EQ_INT(obcd_config_set(&c, "ina219_addr", "0x80", err, sizeof(err)), -1);
        CHECK_EQ_INT(obcd_config_set(&c, "burnwire_ms", "50", err, sizeof(err)), -1);
        CHECK_EQ_INT(obcd_config_set(&c, "heater_line", "22x", err, sizeof(err)), -1);

        obcd_config_default(&c);
        obcd_config_set(&c, "heater_on_below_ccel", "1300", err, sizeof(err));
        CHECK_EQ_INT(obcd_config_validate(&c, err, sizeof(err)), -1);   /* on above off */

        obcd_config_default(&c);
        obcd_config_set(&c, "burnwire_line", "22", err, sizeof(err));
        obcd_config_set(&c, "heater_line", "22", err, sizeof(err));
        CHECK_EQ_INT(obcd_config_validate(&c, err, sizeof(err)), -1);   /* one pin, two jobs */

        obcd_config_default(&c);
        obcd_config_set(&c, "temp_period_ms", "10000", err, sizeof(err));
        CHECK_EQ_INT(obcd_config_validate(&c, err, sizeof(err)), -1);   /* stale before it reads */
    }

    TEST_SUMMARY("test_obcd_config");
}
