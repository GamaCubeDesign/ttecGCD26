/* ttcd configuration: defaults, parsing, overrides and every rejection. */

#include "test_util.h"
#include "config.h"
#include "gama_tc.h"

int main(void)
{
    ttcd_config_t c;
    char err[256];

    TEST_GROUP("config: defaults are the flight values of ADR-0004 and ADR-0007");
    {
        ttcd_config_default(&c);
        CHECK_STR_EQ(c.radio, "sx1278");
        CHECK_STR_EQ(c.ipc_path, "/run/gama/ttec.sock");
        CHECK_EQ_INT(c.p.initial_profile, GAMA_RATE_NOMINAL);
        CHECK_EQ_INT(c.p.tx_power_dbm, 20);
        CHECK_EQ_INT(c.p.hk_period, 10000);
        CHECK_EQ_INT(c.p.contact_timeout, 120000);
        CHECK_EQ_INT(c.p.rate_revert, 20000);
        CHECK_EQ_INT(c.reset_line, 20);
        CHECK_EQ_INT(c.dio0_line, 21);
        CHECK_EQ_INT(ttcd_config_validate(&c, err, sizeof(err)), 0);
    }

    TEST_GROUP("config: a file with comments, blanks and spacing");
    {
        ttcd_config_default(&c);
        const char *text =
            "# bench configuration\n"
            "\n"
            "radio = udp\n"
            "  profile=fast   # trailing comment\n"
            "tx_power_dbm = 2\n"
            "hk_period_ms = 2000\n"
            "ipc_path = /tmp/gama/ttec.sock\n"
            "lbt = preamble";
        CHECK_EQ_INT(ttcd_config_parse(&c, text, err, sizeof(err)), 0);
        CHECK_STR_EQ(c.radio, "udp");
        CHECK_EQ_INT(c.p.initial_profile, GAMA_RATE_FAST);
        CHECK_EQ_INT(c.p.tx_power_dbm, 2);
        CHECK_EQ_INT(c.p.hk_period, 2000);
        CHECK_STR_EQ(c.ipc_path, "/tmp/gama/ttec.sock");
        CHECK_STR_EQ(c.lbt, "preamble");
    }

    TEST_GROUP("config: a mistyped key is an error, with its line number");
    {
        ttcd_config_default(&c);
        const char *text = "radio = udp\ncontact_timout_ms = 5000\n";
        CHECK_EQ_INT(ttcd_config_parse(&c, text, err, sizeof(err)), -1);
        CHECK(strstr(err, "line 2") != NULL);
        CHECK(strstr(err, "unknown key") != NULL);
        CHECK_EQ_INT(c.p.contact_timeout, 120000);        /* default untouched */
    }

    TEST_GROUP("config: values out of range or malformed are refused");
    {
        ttcd_config_default(&c);
        CHECK_EQ_INT(ttcd_config_set(&c, "tx_power_dbm", "21", err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_set(&c, "tx_power_dbm", "1", err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_set(&c, "tx_power_dbm", "-5", err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_set(&c, "hk_period_ms", "10s", err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_set(&c, "hk_period_ms", "", err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_set(&c, "profile", "turbo", err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_set(&c, "radio", "wifi", err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_set(&c, "lbt", "never", err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_set(&c, "udp_port", "70000", err, sizeof(err)), -1);
        char longp[200];
        memset(longp, 'x', sizeof(longp) - 1);
        longp[sizeof(longp) - 1] = '\0';
        CHECK_EQ_INT(ttcd_config_set(&c, "ipc_path", longp, err, sizeof(err)), -1);
        CHECK_EQ_INT(ttcd_config_parse(&c, "no equals sign here\n", err, sizeof(err)), -1);
        CHECK_EQ_INT(c.p.tx_power_dbm, 20);               /* nothing applied */
    }

    TEST_GROUP("config: a command-line override wins over the file");
    {
        ttcd_config_default(&c);
        CHECK_EQ_INT(ttcd_config_parse(&c, "tx_power_dbm = 17\n", err, sizeof(err)), 0);
        CHECK_EQ_INT(ttcd_config_set(&c, "tx_power_dbm", "5", err, sizeof(err)), 0);
        CHECK_EQ_INT(c.p.tx_power_dbm, 5);
        CHECK_EQ_INT(ttcd_config_set(&c, "udp_peer_port", "47010", err, sizeof(err)), 0);
        CHECK_EQ_INT(ttcd_config_set(&c, "udp_port", "47009", err, sizeof(err)), 0);
        CHECK_EQ_INT(c.udp_peer_port, 47010);
        CHECK_EQ_INT(c.udp_port, 47009);
    }

    TEST_GROUP("config: cross-field checks");
    {
        ttcd_config_default(&c);
        ttcd_config_set(&c, "rate_revert_ms", "200000", err, sizeof(err));
        CHECK_EQ_INT(ttcd_config_validate(&c, err, sizeof(err)), -1);
        CHECK(strstr(err, "rate_revert_ms") != NULL);

        ttcd_config_default(&c);
        ttcd_config_set(&c, "ipc_path", "relative.sock", err, sizeof(err));
        CHECK_EQ_INT(ttcd_config_validate(&c, err, sizeof(err)), -1);
    }

    TEST_GROUP("config: a missing file is reported by name");
    {
        ttcd_config_default(&c);
        CHECK_EQ_INT(ttcd_config_load(&c, "/nonexistent/ttcd.conf", err, sizeof(err)), -1);
        CHECK(strstr(err, "/nonexistent/ttcd.conf") != NULL);
    }

    TEST_SUMMARY("test_config");
}
