#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "store";

static void qemu_defaults(canary_config *cfg)
{
#if CONFIG_CANARY_QEMU
    canary_config_set(cfg, "name", "canary-qemu");
    canary_config_set(cfg, "probe1.type", "alive");
    canary_config_set(cfg, "probe1.url", CONFIG_CANARY_TEST_URL);
    canary_config_set(cfg, "probe1.interval", "10");
    canary_config_set(cfg, "probe2.type", "contact");
    canary_config_set(cfg, "probe2.url", CONFIG_CANARY_TEST_CONTACT_URL);
    canary_config_set(cfg, "probe2.pin", "4");
    canary_config_set(cfg, "probe2.active", "high");
    canary_config_set(cfg, "probe2.interval", "10");
#else
    (void)cfg;
#endif
}

void store_load(canary_config *cfg)
{
    canary_config_defaults(cfg);
    qemu_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open("canary", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, "config", NULL, &len) == ESP_OK && len > 0) {
        char *text = malloc(len);
        if (text && nvs_get_str(h, "config", text, &len) == ESP_OK) {
            canary_config loaded;
            canary_config_defaults(&loaded);
            char err[96];
            if (canary_config_parse(&loaded, text, err, sizeof err) == 0) {
                *cfg = loaded;
            } else {
                ESP_LOGE(TAG, "stored configuration ignored: %s", err);
            }
        }
        free(text);
    }
    nvs_close(h);
}

bool store_save(const canary_config *cfg)
{
    size_t len = canary_config_format(cfg, NULL, 0) + 1;
    char *text = malloc(len);
    if (!text) {
        return false;
    }
    canary_config_format(cfg, text, len);
    nvs_handle_t h;
    esp_err_t err = nvs_open("canary", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "config", text);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the configuration failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}
