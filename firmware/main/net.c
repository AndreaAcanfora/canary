// Network: Wi-Fi station on real boards, the emulated OpenCores Ethernet
// MAC in QEMU builds. Also announces the setup page over mDNS.
#include <string.h>

#include "app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "sdkconfig.h"

#if CONFIG_CANARY_QEMU
#include "esp_eth.h"
#endif

static const char *TAG = "net";
static EventGroupHandle_t events;
static esp_netif_t *netif;
static volatile bool provisioning;
static esp_timer_handle_t reconnect;

__attribute__((unused)) static void on_reconnect(void *arg)
{
    (void)arg;
    if (!provisioning && net_has_credentials()) {
        esp_wifi_connect();
    }
}

#define GOT_IP BIT0
#define FAILED BIT1

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "address " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupClearBits(events, FAILED);
        xEventGroupSetBits(events, GOT_IP);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && net_has_credentials()) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, GOT_IP);
        if (provisioning) {
            xEventGroupSetBits(events, FAILED);
            return;
        }
        // Keep trying: after a power cut the router often boots after us.
        // A timer, so that the event loop is not blocked meanwhile.
        if (reconnect && !esp_timer_is_active(reconnect)) {
            esp_timer_start_once(reconnect, 3 * 1000000);
        }
        return;
    }
#if CONFIG_CANARY_QEMU
    if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        xEventGroupClearBits(events, GOT_IP);
    }
#endif
}

#if CONFIG_CANARY_QEMU
static void start_ethernet(void)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    netif = esp_netif_new(&cfg);
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}
#else
static void start_wifi(const char *hostname)
{
    netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, hostname);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Mains-powered: favour reaction time over power saving.
    esp_wifi_set_ps(WIFI_PS_NONE);
    const esp_timer_create_args_t timer = {.callback = on_reconnect, .name = "reconnect"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &reconnect));
    ESP_ERROR_CHECK(esp_wifi_start());
}
#endif

void net_start(const char *hostname)
{
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
#if CONFIG_CANARY_QEMU
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    start_ethernet();
#else
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    start_wifi(hostname);
#endif

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(hostname);
        mdns_instance_name_set("canary");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

bool net_wait(uint32_t timeout_ms)
{
    return xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP;
}

bool net_up(void)
{
    return events && (xEventGroupGetBits(events) & GOT_IP);
}

bool net_has_credentials(void)
{
#if CONFIG_CANARY_QEMU
    return true;
#else
    wifi_config_t c;
    return esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0] != '\0';
#endif
}

bool net_connect_wifi(const char *ssid, const char *password, uint32_t timeout_ms)
{
#if CONFIG_CANARY_QEMU
    (void)ssid;
    (void)password;
    (void)timeout_ms;
    return false;
#else
    wifi_config_t old, c = {0};
    esp_wifi_get_config(WIFI_IF_STA, &old);
    strlcpy((char *)c.sta.ssid, ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, password, sizeof c.sta.password);
    c.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    provisioning = true;
    esp_wifi_disconnect();
    xEventGroupClearBits(events, GOT_IP | FAILED);
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
    // A wrong password can disconnect several times before giving up:
    // wait for an address until the timeout.
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    bool ok = false;
    while (!ok && xTaskGetTickCount() < deadline) {
        EventBits_t b = xEventGroupWaitBits(events, GOT_IP | FAILED, pdTRUE, pdFALSE, deadline - xTaskGetTickCount());
        if (b & GOT_IP) {
            xEventGroupSetBits(events, GOT_IP);
            ok = true;
        } else if (b & FAILED) {
            esp_wifi_connect();
        }
    }
    provisioning = false;
    if (!ok) {
        ESP_LOGW(TAG, "could not join \"%s\"", ssid);
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &old);
        if (old.sta.ssid[0]) {
            esp_wifi_connect();
        }
    }
    return ok;
#endif
}

void net_ip(char *out, size_t n)
{
    esp_netif_ip_info_t ip;
    if (netif && net_up() && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out, n, "-");
    }
}

int net_rssi(void)
{
#if CONFIG_CANARY_QEMU
    return 0;
#else
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
#endif
}
