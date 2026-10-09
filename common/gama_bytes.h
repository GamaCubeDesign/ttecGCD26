/*
 * gama_bytes.h — explicit little-endian scalar serialisation.
 *
 * Every multi-byte field that crosses a process or radio boundary goes
 * through these helpers. They exist to make one failure mode impossible:
 * the previous mission transmitted C structs by casting them to uint8_t*
 * (ultima_missao/satellite/Integration.cpp:149), which makes the wire format
 * depend on the compiler's padding and alignment choices. That happened to
 * work only because both ends were little-endian with compatible layout
 * rules; nothing in the source said so, and nothing would have caught it
 * breaking.
 *
 * These read and write one byte at a time, so they are also safe on
 * unaligned buffers -- which matters because payloads start at odd offsets
 * inside a frame.
 *
 * Shared verbatim between the Raspberry Pi and the ESP32. Header-only.
 */

#ifndef GAMA_BYTES_H
#define GAMA_BYTES_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- writers: return the number of bytes consumed ---- */

static inline size_t gama_put_u8(uint8_t *p, uint8_t v)
{
    p[0] = v;
    return 1;
}

static inline size_t gama_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    return 2;
}

static inline size_t gama_put_u24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    return 3;
}

static inline size_t gama_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
    return 4;
}

static inline size_t gama_put_i16(uint8_t *p, int16_t v)
{
    return gama_put_u16(p, (uint16_t)v);
}

/* Two's-complement 24-bit. Caller must have clamped v to [-8388608, 8388607]. */
static inline size_t gama_put_i24(uint8_t *p, int32_t v)
{
    return gama_put_u24(p, (uint32_t)v & 0x00FFFFFFu);
}

/* ---- readers ---- */

static inline uint8_t gama_get_u8(const uint8_t *p)
{
    return p[0];
}

static inline uint16_t gama_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t gama_get_u24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline uint32_t gama_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int16_t gama_get_i16(const uint8_t *p)
{
    return (int16_t)gama_get_u16(p);
}

/* Sign-extends the 24-bit value into a full int32_t. Done by explicit test
 * rather than by shifting left then arithmetic-shifting right, because the
 * result of right-shifting a negative integer is implementation-defined. */
static inline int32_t gama_get_i24(const uint8_t *p)
{
    uint32_t raw = gama_get_u24(p);
    return (raw & 0x00800000u) ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;
}

#ifdef __cplusplus
}
#endif

#endif /* GAMA_BYTES_H */
