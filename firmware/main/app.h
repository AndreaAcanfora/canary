// State shared by the firmware tasks.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "canary/config.h"
#include "canary/probe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SETUP_WINDOW_S (15 * 60)

typedef struct {
    probe_reading reading;
    bool reported_ok;
    bool started;
    int last_status;     // HTTP status of the last signal, 0: network error
    int64_t last_sent_s; // uptime of the last signal, -1: never
} probe_status;

typedef struct {
    SemaphoreHandle_t lock;
    canary_config cfg;
    probe_status probes[CANARY_MAX_PROBES];
    bool configured;          // at least one probe has a URL
    int64_t setup_until_s;    // the setup page accepts changes until this uptime
    const char *reset_reason; // why the chip started: tells a power cut from a crash
} app_state;

extern app_state app;

static inline int64_t uptime_s(void)
{
    return (int64_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
}

bool app_setup_open(void);
void app_open_setup(void);

// store.c: configuration in NVS
void store_load(canary_config *cfg);
bool store_save(const canary_config *cfg);

// net.c: Wi-Fi (or QEMU Ethernet), Improv provisioning glue
void net_start(const char *hostname);
bool net_wait(uint32_t timeout_ms);
bool net_up(void);
bool net_has_credentials(void);
bool net_connect_wifi(const char *ssid, const char *password, uint32_t timeout_ms);
void net_ip(char *out, size_t n);
int net_rssi(void);

// improv_serial.c
void improv_start(void);

// web.c
void web_start(void);

// onewire.c: one DS18B20 per GPIO
void ds18b20_start_conversion(int pin);
bool ds18b20_read(int pin, float *celsius);

// led.c
void led_start(int pin, bool active_low);
