/*
 * adsbd configuration: `key = value` lines, -o overrides, and the checks
 * that keep a deployment's settings out of the binary (AGENTS.md, rule 4).
 */

#include "test_util.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    TEST_GROUP("config: the defaults are valid, and start dump1090-fa without a shell");
    {
        adsbd_config_t cfg;
        adsbd_config_default(&cfg);
        char err[300] = "";
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), 0);
        char buf[400];
        char *argv[40];
        int n = adsbd_config_argv(&cfg, buf, sizeof(buf), argv, 40);
        CHECK(n > 5);
        CHECK_STR_EQ(argv[0], "/usr/bin/dump1090-fa");
        CHECK(argv[n] == NULL);
        CHECK_EQ_INT(cfg.p.snapshot_period, 1000);
        CHECK_EQ_INT(cfg.p.snapshot_max, 24);
    }

    TEST_GROUP("config: every key is read; an unknown key is an error, with its line");
    {
        adsbd_config_t cfg;
        adsbd_config_default(&cfg);
        char err[300] = "";
        const char *text =
            "# comment\n"
            "ipc_path = /tmp/x.sock\n"
            "sbs_host = 127.0.0.2   # trailing comment\n"
            "sbs_port = 31003\n"
            "dump1090_cmd = /opt/readsb --quiet\n"
            "ndjson_path = /tmp/a.ndjson\n"
            "ndjson_max_bytes = 100000\n"
            "log_path = -\n"
            "retry_ms = 500\n"
            "snapshot_period_ms = 2000\n"
            "snapshot_max_aircraft = 36\n"
            "table_capacity = 64\n"
            "position_max_age_ms = 5000\n";
        CHECK_EQ_INT(adsbd_config_parse(&cfg, text, err, sizeof(err)), 0);
        CHECK_STR_EQ(cfg.ipc_path, "/tmp/x.sock");
        CHECK_STR_EQ(cfg.sbs_host, "127.0.0.2");
        CHECK_EQ_INT(cfg.sbs_port, 31003);
        CHECK_STR_EQ(cfg.dump1090_cmd, "/opt/readsb --quiet");
        CHECK_EQ_INT(cfg.ndjson_max_bytes, 100000);
        CHECK_EQ_INT(cfg.retry_ms, 500);
        CHECK_EQ_INT(cfg.p.snapshot_period, 2000);
        CHECK_EQ_INT(cfg.p.snapshot_max, 36);
        CHECK_EQ_INT(cfg.p.table_capacity, 64);
        CHECK_EQ_INT(cfg.p.position_max_age, 5000);
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), 0);

        CHECK_EQ_INT(adsbd_config_parse(&cfg, "sbs_port = 30003\nbogus = 1\n", err, sizeof(err)), -1);
        CHECK(strstr(err, "line 2") != NULL && strstr(err, "bogus") != NULL);
        CHECK_EQ_INT(adsbd_config_set(&cfg, "sbs_port", "70000", err, sizeof(err)), -1);
        CHECK_EQ_INT(adsbd_config_set(&cfg, "snapshot_max_aircraft", "97", err, sizeof(err)), -1);
        CHECK_EQ_INT(adsbd_config_set(&cfg, "table_capacity", "129", err, sizeof(err)), -1);
    }

    TEST_GROUP("config: values that would break a JSON log line are refused");
    {
        adsbd_config_t cfg;
        adsbd_config_default(&cfg);
        char err[300] = "";
        CHECK_EQ_INT(adsbd_config_set(&cfg, "ndjson_path", "/tmp/a\"b", err, sizeof(err)), -1);
        CHECK_EQ_INT(adsbd_config_set(&cfg, "log_path", "/tmp/a\\b", err, sizeof(err)), -1);
    }

    TEST_GROUP("config: fields that must agree with each other");
    {
        char err[300] = "";
        adsbd_config_t cfg;
        adsbd_config_default(&cfg);
        cfg.p.position_max_age = 70000;                  /* > field_ttl */
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), -1);
        adsbd_config_default(&cfg);
        cfg.p.field_ttl = 90000;                         /* > track_expiry */
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), -1);
        adsbd_config_default(&cfg);
        cfg.p.table_capacity = 20;                       /* < snapshot_max */
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), -1);
        adsbd_config_default(&cfg);
        cfg.restart_min_ms = 40000;                      /* > restart_max */
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), -1);
        adsbd_config_default(&cfg);
        snprintf(cfg.dump1090_cmd, sizeof(cfg.dump1090_cmd), "dump1090-fa --quiet");
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), -1);   /* no PATH */
        adsbd_config_default(&cfg);
        snprintf(cfg.ipc_path, sizeof(cfg.ipc_path), "ttec.sock");
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), -1);
    }

    TEST_GROUP("config: an empty dump1090_cmd starts nothing, for a replay or an outside dump1090");
    {
        adsbd_config_t cfg;
        adsbd_config_default(&cfg);
        char err[300] = "";
        CHECK_EQ_INT(adsbd_config_set(&cfg, "dump1090_cmd", "", err, sizeof(err)), 0);
        CHECK_EQ_INT(adsbd_config_validate(&cfg, err, sizeof(err)), 0);
        char buf[400];
        char *argv[40];
        CHECK_EQ_INT(adsbd_config_argv(&cfg, buf, sizeof(buf), argv, 40), 0);
        CHECK(argv[0] == NULL);
    }

    TEST_SUMMARY("test_adsbd_config");
}
