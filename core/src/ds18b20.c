#include "canary/ds18b20.h"

uint8_t ds18b20_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (uint8_t)((crc ^ b) & 1);
            crc >>= 1;
            if (mix) {
                crc ^= 0x8c;
            }
            b >>= 1;
        }
    }
    return crc;
}

bool ds18b20_decode(const uint8_t scratchpad[9], float *celsius)
{
    bool all_ones = true, all_zeros = true;
    for (int i = 0; i < 9; i++) {
        all_ones = all_ones && scratchpad[i] == 0xff;
        all_zeros = all_zeros && scratchpad[i] == 0x00;
    }
    if (all_ones || all_zeros || ds18b20_crc8(scratchpad, 8) != scratchpad[8]) {
        return false;
    }
    int16_t raw = (int16_t)((uint16_t)scratchpad[1] << 8 | scratchpad[0]);
    // Configuration register bits 5-6: resolution; low bits are undefined
    // below 12 bits.
    switch ((scratchpad[4] >> 5) & 3) {
    case 0:
        raw = (int16_t)(raw & ~7);
        break;
    case 1:
        raw = (int16_t)(raw & ~3);
        break;
    case 2:
        raw = (int16_t)(raw & ~1);
        break;
    default:
        break;
    }
    if (raw == 0x0550) { // 85 °C: conversion never ran
        return false;
    }
    *celsius = (float)raw / 16.0f;
    return true;
}
