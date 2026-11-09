// DS18B20 helpers that do not touch hardware: CRC and scratchpad decoding.
#ifndef CANARY_DS18B20_H
#define CANARY_DS18B20_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS18B20_SKIP_ROM 0xcc
#define DS18B20_CONVERT_T 0x44
#define DS18B20_READ_SCRATCHPAD 0xbe
#define DS18B20_CONVERSION_MS 750

// Dallas/Maxim 1-Wire CRC-8 (polynomial x^8 + x^5 + x^4 + 1).
uint8_t ds18b20_crc8(const uint8_t *data, size_t len);

// Decodes the 9-byte scratchpad. Returns false when the CRC is wrong, when the
// bus reads all ones or all zeros (no sensor, short circuit), or when the
// sensor reports its 85 °C power-on value.
bool ds18b20_decode(const uint8_t scratchpad[9], float *celsius);

#ifdef __cplusplus
}
#endif

#endif
