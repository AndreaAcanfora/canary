// Status LED patterns, readable on any single-colour LED:
//   slow blink (1 s)           waiting for Wi-Fi setup
//   fast blink                 no network
//   two quick flashes, every 2 s   a probe reports a problem
//   short flash every 3 s      all fine
#include "app.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int led_pin = -1;
static bool led_low;

static void set(bool on)
{
    gpio_set_level(led_pin, on != led_low);
}

static void blink(int on_ms, int off_ms)
{
    set(true);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    set(false);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static bool any_problem(void)
{
    bool problem = false;
    xSemaphoreTake(app.lock, portMAX_DELAY);
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        if (app.cfg.probes[i].type != PROBE_NONE && app.probes[i].started && !app.probes[i].reported_ok) {
            problem = true;
        }
    }
    xSemaphoreGive(app.lock);
    return problem;
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool enabled;
        xSemaphoreTake(app.lock, portMAX_DELAY);
        enabled = app.cfg.led;
        xSemaphoreGive(app.lock);
        if (!enabled) {
            set(false);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else if (!net_has_credentials()) {
            blink(1000, 1000);
        } else if (!net_up()) {
            blink(100, 100);
        } else if (any_problem()) {
            blink(80, 150);
            blink(80, 1700);
        } else {
            blink(40, 2960);
        }
    }
}

void led_start(int pin, bool active_low)
{
    if (pin < 0) {
        return;
    }
    led_pin = pin;
    led_low = active_low;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    set(false);
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
}
