#include "ds18b20.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ds18b20_result_t ds18b20_parse(const char *text, size_t len, int16_t *ccel)
{
    char buf[160];
    if (text == NULL || ccel == NULL || len == 0 || len >= sizeof(buf)) {
        return DS18B20_ERR_FORMAT;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl == NULL) {
        return DS18B20_ERR_FORMAT;
    }
    *nl = '\0';
    const char *crc = strstr(buf, "crc=");
    if (crc == NULL) {
        return DS18B20_ERR_FORMAT;
    }
    bool yes = strstr(crc, " YES") != NULL;
    if (!yes) {
        return strstr(crc, " NO") != NULL ? DS18B20_ERR_CRC : DS18B20_ERR_FORMAT;
    }

    const char *t = strstr(nl + 1, "t=");
    if (t == NULL) {
        return DS18B20_ERR_FORMAT;
    }
    char *end;
    errno = 0;
    long milli = strtol(t + 2, &end, 10);
    if (errno != 0 || end == t + 2 || (*end != '\0' && *end != '\n')) {
        return DS18B20_ERR_FORMAT;
    }
    if (milli == 85000) {
        return DS18B20_ERR_POWER_ON;
    }
    if (milli < -55000 || milli > 125000) {
        return DS18B20_ERR_RANGE;
    }
    long c = milli >= 0 ? (milli + 5) / 10 : (milli - 5) / 10;
    *ccel = (int16_t)c;
    return DS18B20_OK;
}

ds18b20_result_t ds18b20_read_file(const char *path, int16_t *ccel)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return DS18B20_ERR_IO;
    }
    char text[160];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, text + have, sizeof(text) - 1 - have);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        have += (size_t)n;
        if (have == sizeof(text) - 1) {
            break;
        }
    }
    close(fd);
    if (have == 0) {
        return DS18B20_ERR_IO;
    }
    return ds18b20_parse(text, have, ccel);
}

const char *ds18b20_result_name(ds18b20_result_t r)
{
    switch (r) {
    case DS18B20_OK:           return "OK";
    case DS18B20_ERR_FORMAT:   return "FORMAT";
    case DS18B20_ERR_CRC:      return "CRC";
    case DS18B20_ERR_POWER_ON: return "POWER_ON_VALUE";
    case DS18B20_ERR_RANGE:    return "RANGE";
    case DS18B20_ERR_IO:       return "IO";
    default:                   return "UNKNOWN";
    }
}
