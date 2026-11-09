// canary firmware: reads the probes once per second and sends OHP heartbeats.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "canary/ds18b20.h"
#include "canary/ohp.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "canary";

app_state app;

// Latest pending signal of each probe. A newer signal replaces an older one
// that could not be sent yet: after an outage only the current state matters.
typedef struct {
    bool pending;
    ohp_signal sig;
    char body[PROBE_BODY_MAX + 96];
} outbox_slot;

static outbox_slot outbox[CANARY_MAX_PROBES];
static TaskHandle_t sender;

bool app_setup_open(void)
{
    xSemaphoreTake(app.lock, portMAX_DELAY);
    bool open = !app.configured || uptime_s() < app.setup_until_s;
    xSemaphoreGive(app.lock);
    return open;
}

void app_open_setup(void)
{
    xSemaphoreTake(app.lock, portMAX_DELAY);
    app.setup_until_s = uptime_s() + SETUP_WINDOW_S;
    xSemaphoreGive(app.lock);
    ESP_LOGI(TAG, "setup page open for %d minutes", SETUP_WINDOW_S / 60);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SW:
        return "restart";
    case ESP_RST_PANIC:
        return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_EXT:
        return "reset-pin";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    default:
        return "unknown";
    }
}

// --- HTTP transport for ohp_send ---------------------------------------------

static esp_err_t on_http_event(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(ev->header_key, "Retry-After") == 0) {
        int *retry_after = ev->user_data;
        int v = atoi(ev->header_value);
        if (v >= 0) {
            *retry_after = v;
        }
    }
    return ESP_OK;
}

static int http_post(void *ctx, const char *url, const char *body, int *retry_after_s)
{
    const char *host = ctx;
    *retry_after_s = -1;
    esp_http_client_config_t c = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = OHP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "canary/" CANARY_VERSION,
        .event_handler = on_http_event,
        .user_data = retry_after_s,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        return 0;
    }
    esp_http_client_set_header(h, "Content-Type", "text/plain; charset=utf-8");
    esp_http_client_set_header(h, "OHP-Version", OHP_VERSION);
    esp_http_client_set_header(h, "OHP-Host", host);
    esp_http_client_set_post_field(h, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(h);
    int status = err == ESP_OK ? esp_http_client_get_status_code(h) : 0;
    if (err != ESP_OK) {
        char red[96];
        ohp_redact(red, sizeof red, url);
        ESP_LOGW(TAG, "%s: %s", red, esp_err_to_name(err));
    }
    esp_http_client_cleanup(h);
    return status;
}

static void http_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void sender_task(void *arg)
{
    (void)arg;
    char host[CANARY_NAME_MAX];
    xSemaphoreTake(app.lock, portMAX_DELAY);
    strlcpy(host, app.cfg.name, sizeof host);
    xSemaphoreGive(app.lock);
    ohp_transport t = {.post = http_post, .sleep_ms = http_sleep, .ctx = host};

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        for (int i = 0; i < CANARY_MAX_PROBES; i++) {
            if (!net_wait(0)) {
                break;
            }
            outbox_slot slot;
            char url[PROBE_URL_MAX];
            xSemaphoreTake(app.lock, portMAX_DELAY);
            slot = outbox[i];
            outbox[i].pending = false;
            strlcpy(url, app.cfg.probes[i].url, sizeof url);
            xSemaphoreGive(app.lock);
            if (!slot.pending) {
                continue;
            }

            int status = ohp_send(&t, url, slot.sig, 0, NULL, slot.body);
            char red[96];
            ohp_redact(red, sizeof red, url);
            ESP_LOGI(TAG, "probe%d %s %s -> %d (%s)", i + 1, slot.sig == OHP_FAIL ? "fail" : "success", red, status,
                     slot.body);

            xSemaphoreTake(app.lock, portMAX_DELAY);
            app.probes[i].last_status = status;
            app.probes[i].last_sent_s = uptime_s();
            if (ohp_classify(status) == OHP_RETRY && !outbox[i].pending) {
                outbox[i] = slot; // still unsent: try again with the next round
            }
            xSemaphoreGive(app.lock);
        }
    }
}

// --- sampling ------------------------------------------------------------------

static void setup_pins(const canary_config *cfg)
{
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        const probe_cfg *p = &cfg->probes[i];
        if (p->type == PROBE_CONTACT && p->pin >= 0) {
            gpio_config_t io = {
                .pin_bit_mask = 1ULL << p->pin,
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = p->active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
                .pull_down_en = p->active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            };
            gpio_config(&io);
        }
    }
}

static void sample_task(void *arg)
{
    (void)arg;
    canary_config cfg;
    xSemaphoreTake(app.lock, portMAX_DELAY);
    cfg = app.cfg;
    xSemaphoreGive(app.lock);
    setup_pins(&cfg);

    probe_state state[CANARY_MAX_PROBES] = {0};
    bool first[CANARY_MAX_PROBES];
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        first[i] = true;
    }

    for (;;) {
        TickType_t start = xTaskGetTickCount();

        bool any_temp = false;
        for (int i = 0; i < CANARY_MAX_PROBES; i++) {
            if (cfg.probes[i].type == PROBE_TEMP && cfg.probes[i].pin >= 0) {
                ds18b20_start_conversion(cfg.probes[i].pin);
                any_temp = true;
            }
        }
        if (any_temp) {
            vTaskDelay(pdMS_TO_TICKS(DS18B20_CONVERSION_MS));
        }

        uint32_t now = (uint32_t)uptime_s();
        for (int i = 0; i < CANARY_MAX_PROBES; i++) {
            const probe_cfg *p = &cfg.probes[i];
            if (p->type == PROBE_NONE) {
                continue;
            }
            probe_reading r = {0};
            if (p->type == PROBE_ALIVE) {
                r.valid = true;
            } else if (p->type == PROBE_TEMP) {
                r.valid = ds18b20_read(p->pin, &r.value);
            } else if (p->type == PROBE_CONTACT) {
                r.valid = true;
                r.value = (gpio_get_level(p->pin) != 0) == p->active_high ? 1.0f : 0.0f;
            }

            probe_action act;
            probe_step(p, &state[i], r, now, &act);

            xSemaphoreTake(app.lock, portMAX_DELAY);
            app.probes[i].reading = r;
            if (act.send) {
                app.probes[i].started = true;
                app.probes[i].reported_ok = act.sig == OHP_SUCCESS;
                outbox_slot *s = &outbox[i];
                s->pending = true;
                s->sig = act.sig;
                int n = snprintf(s->body, sizeof s->body, "%s uptime_s=%lu", act.body, (unsigned long)now);
                int rssi = net_rssi();
                if (rssi && n > 0 && (size_t)n < sizeof s->body) {
                    n += snprintf(s->body + n, sizeof s->body - (size_t)n, " rssi=%d", rssi);
                }
                if (first[i] && n > 0 && (size_t)n < sizeof s->body) {
                    snprintf(s->body + n, sizeof s->body - (size_t)n, " boot=%s", app.reset_reason);
                }
                first[i] = false;
            }
            xSemaphoreGive(app.lock);
            if (act.send) {
                xTaskNotifyGive(sender);
            }
        }

        vTaskDelayUntil(&start, pdMS_TO_TICKS(1000));
    }
}

__attribute__((unused)) static void button_task(void *arg)
{
    int pin = (int)(intptr_t)arg;
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    bool was_down = false;
    for (;;) {
        bool down = gpio_get_level(pin) == 0;
        if (down && !was_down) {
            app_open_setup();
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app.lock = xSemaphoreCreateMutex();
    app.reset_reason = reset_reason();
    store_load(&app.cfg);
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        app.probes[i].last_sent_s = -1;
        if (app.cfg.probes[i].type != PROBE_NONE && app.cfg.probes[i].url[0]) {
            app.configured = true;
        }
    }
    app.setup_until_s = SETUP_WINDOW_S;

    ESP_LOGI(TAG, "canary %s (OHP %s), name %s, started by %s", CANARY_VERSION, OHP_VERSION, app.cfg.name,
             app.reset_reason);

    led_start(app.cfg.led_pin >= 0 ? app.cfg.led_pin : CONFIG_CANARY_LED_GPIO, CONFIG_CANARY_LED_ACTIVE_LOW);
    net_start(app.cfg.name);
    improv_start();
    web_start();
#if CONFIG_CANARY_BUTTON_GPIO >= 0
    xTaskCreate(button_task, "button", 2048, (void *)(intptr_t)CONFIG_CANARY_BUTTON_GPIO, 3, NULL);
#endif

    xTaskCreate(sender_task, "sender", 8192, NULL, 4, &sender);
    xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL);

    if (!net_has_credentials()) {
        ESP_LOGI(TAG, "no Wi-Fi yet: set it up from https://andreaacanfora.github.io/canary/");
    }
}
