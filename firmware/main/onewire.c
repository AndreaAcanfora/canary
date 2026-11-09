// Bit-banged 1-Wire for one DS18B20 per GPIO: no RMT channel and no
// external component, so it works on every ESP32 variant. The pin needs a
// 4.7 kΩ pull-up to 3.3 V (many DS18B20 modules have one on board).
#include "app.h"
#include "canary/ds18b20.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static void bus_setup(int pin)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, 1);
}

static bool bus_reset(int pin)
{
    gpio_set_level(pin, 0);
    esp_rom_delay_us(480);
    portENTER_CRITICAL(&mux);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(70);
    bool present = gpio_get_level(pin) == 0;
    portEXIT_CRITICAL(&mux);
    esp_rom_delay_us(410);
    return present;
}

static void write_bit(int pin, int bit)
{
    portENTER_CRITICAL(&mux);
    gpio_set_level(pin, 0);
    esp_rom_delay_us(bit ? 6 : 60);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(bit ? 64 : 10);
    portEXIT_CRITICAL(&mux);
}

static int read_bit(int pin)
{
    portENTER_CRITICAL(&mux);
    gpio_set_level(pin, 0);
    esp_rom_delay_us(3);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(10);
    int bit = gpio_get_level(pin);
    portEXIT_CRITICAL(&mux);
    esp_rom_delay_us(53);
    return bit;
}

static void write_byte(int pin, uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        write_bit(pin, (b >> i) & 1);
    }
}

static uint8_t read_byte(int pin)
{
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        b |= (uint8_t)(read_bit(pin) << i);
    }
    return b;
}

void ds18b20_start_conversion(int pin)
{
    bus_setup(pin);
    if (bus_reset(pin)) {
        write_byte(pin, DS18B20_SKIP_ROM);
        write_byte(pin, DS18B20_CONVERT_T);
    }
}

bool ds18b20_read(int pin, float *celsius)
{
    if (!bus_reset(pin)) {
        return false;
    }
    write_byte(pin, DS18B20_SKIP_ROM);
    write_byte(pin, DS18B20_READ_SCRATCHPAD);
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = read_byte(pin);
    }
    return ds18b20_decode(sp, celsius);
}
