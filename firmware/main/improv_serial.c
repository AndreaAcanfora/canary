// Improv Wi-Fi over the console port: after flashing from the browser,
// ESP Web Tools asks for the Wi-Fi network and sends it here.
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "canary/improv.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM
#endif

static const char *TAG = "improv";

static void send(const uint8_t *frame, size_t len)
{
    if (len == 0) {
        return;
    }
    // Lock stdout so that a log line cannot land inside the frame.
    flockfile(stdout);
    fwrite(frame, 1, len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void send_state(uint8_t state)
{
    uint8_t f[16];
    send(f, improv_build_state(f, sizeof f, state));
}

static void send_error(uint8_t error)
{
    uint8_t f[16];
    send(f, improv_build_error(f, sizeof f, error));
}

static void send_result(uint8_t command, const char *const *strings, size_t count)
{
    uint8_t f[IMPROV_MAX_FRAME];
    send(f, improv_build_result(f, sizeof f, command, strings, count));
}

static void setup_url(char *out, size_t n)
{
    char ip[16];
    net_ip(ip, sizeof ip);
    snprintf(out, n, "http://%s/", ip);
}

static void send_provisioned(uint8_t command)
{
    char url[40];
    setup_url(url, sizeof url);
    const char *strings[] = {url};
    send_state(IMPROV_STATE_PROVISIONED);
    send_result(command, strings, 1);
}

static const char *chip_name(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    switch (info.model) {
    case CHIP_ESP32:
        return "ESP32";
    case CHIP_ESP32S2:
        return "ESP32-S2";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32C3:
        return "ESP32-C3";
    case CHIP_ESP32C6:
        return "ESP32-C6";
    default:
        return "ESP32";
    }
}

static void handle(const improv_rpc *rpc)
{
    switch (rpc->command) {
    case IMPROV_CMD_WIFI_SETTINGS:
        send_error(IMPROV_ERROR_NONE);
        send_state(IMPROV_STATE_PROVISIONING);
        ESP_LOGI(TAG, "joining \"%s\"", rpc->ssid);
        if (net_connect_wifi(rpc->ssid, rpc->password, 20000)) {
            app_open_setup();
            send_provisioned(IMPROV_CMD_WIFI_SETTINGS);
        } else {
            send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
            send_state(IMPROV_STATE_READY);
        }
        break;
    case IMPROV_CMD_GET_STATE:
        send_error(IMPROV_ERROR_NONE);
        if (net_up()) {
            send_provisioned(IMPROV_CMD_GET_STATE);
        } else {
            send_state(IMPROV_STATE_READY);
        }
        break;
    case IMPROV_CMD_GET_INFO: {
        send_error(IMPROV_ERROR_NONE);
        char name[CANARY_NAME_MAX];
        xSemaphoreTake(app.lock, portMAX_DELAY);
        strlcpy(name, app.cfg.name, sizeof name);
        xSemaphoreGive(app.lock);
        const char *strings[] = {"canary", CANARY_VERSION, chip_name(), name};
        send_result(IMPROV_CMD_GET_INFO, strings, 4);
        break;
    }
    default:
        send_error(IMPROV_ERROR_UNKNOWN_RPC);
        break;
    }
}

static int read_console(uint8_t *buf, size_t n)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(buf, n, portMAX_DELAY);
#else
    return uart_read_bytes(CONSOLE_UART, buf, n, portMAX_DELAY);
#endif
}

static void improv_task(void *arg)
{
    (void)arg;
    improv_parser p;
    improv_parser_init(&p);
    uint8_t buf[64];
    for (;;) {
        int n = read_console(buf, sizeof buf);
        for (int i = 0; i < n; i++) {
            improv_frame f;
            improv_status s = improv_feed(&p, buf[i], &f);
            if (s == IMPROV_BAD_FRAME) {
                send_error(IMPROV_ERROR_INVALID_RPC);
            } else if (s == IMPROV_FRAME && f.type == IMPROV_TYPE_RPC) {
                improv_rpc rpc;
                if (improv_parse_rpc(&f, &rpc)) {
                    handle(&rpc);
                } else {
                    send_error(IMPROV_ERROR_INVALID_RPC);
                }
                memset(&rpc, 0, sizeof rpc); // do not keep the password around
            }
        }
    }
}

void improv_start(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
#else
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 512, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(CONSOLE_UART);
#endif
    xTaskCreate(improv_task, "improv", 4096, NULL, 5, NULL);
}
