#include "ina219.h"

int ina219_init(const ina219_bus_t *bus)
{
    return bus->write_reg16(bus->ctx, INA219_REG_CONFIG, INA219_CONFIG_VALUE) == 0 ? 0 : -1;
}

int ina219_read(const ina219_bus_t *bus, uint32_t shunt_mohm,
                uint16_t *battery_mv, int16_t *current_ma)
{
    uint16_t bus_raw, shunt_raw;
    if (shunt_mohm == 0 ||
        bus->read_reg16(bus->ctx, INA219_REG_BUS, &bus_raw) != 0 ||
        bus->read_reg16(bus->ctx, INA219_REG_SHUNT, &shunt_raw) != 0) {
        return -1;
    }
    if (bus_raw & 0x0001u) {
        return -1;                       /* OVF: the reading is not valid */
    }
    /* Bus voltage: bits 15..3, 4 mV each; at most 8191 x 4 = 32764 mV. */
    *battery_mv = (uint16_t)((bus_raw >> 3) * 4u);

    /* Shunt voltage: two's complement, 10 uV per bit, at most +/-32000.
     * I [mA] = V [uV] / R [mOhm] = raw x 10 / R. Rounded to nearest. */
    int32_t uv = (int32_t)(int16_t)shunt_raw * 10;
    int32_t r = (int32_t)shunt_mohm;
    int32_t ma = (uv >= 0 ? uv + r / 2 : uv - r / 2) / r;
    if (ma > INT16_MAX - 1) {
        ma = INT16_MAX - 1;
    }
    if (ma < INT16_MIN + 1) {
        ma = INT16_MIN + 1;              /* INT16_MIN is "no data" (D6) */
    }
    *current_ma = (int16_t)ma;
    return 0;
}
