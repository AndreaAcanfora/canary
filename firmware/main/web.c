// Setup and status page on port 80 (http://<name>.local/).
//
// Anyone on the local network can read the status. Settings can be changed
// only during the setup window: the first 15 minutes after power-on or Wi-Fi
// setup, or after pressing the BOOT button. Check URLs are secrets, so outside
// the window they are shown redacted.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "canary/form.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";

__attribute__((format(printf, 2, 3))) static void sendf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        httpd_resp_send_chunk(req, buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
    }
}

static const char *esc(char *buf, size_t n, const char *s)
{
    canary_html_escape(buf, n, s);
    return buf;
}

static void fmt_float(char *out, size_t n, float v)
{
    if (isnan(v)) {
        out[0] = '\0';
    } else {
        snprintf(out, n, "%g", (double)v);
    }
}

static const char head[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>canary</title><style>"
    ":root{color-scheme:light dark;--bg:#fbfaf7;--fg:#1d1d1b;--mut:#6b6b66;--line:#e2dfd8;--ok:#1f7a4d;--bad:#b3261e;--acc:#c8a400}"
    "@media(prefers-color-scheme:dark){:root{--bg:#141413;--fg:#ecebe6;--mut:#9a998f;--line:#2e2d2a;--ok:#5cc28d;--bad:#ff8a80}}"
    "body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}"
    "main{max-width:680px;margin:auto;padding:24px 16px}h1{font-size:22px;margin:0 0 4px}"
    "h1 span{color:var(--acc)}.mut{color:var(--mut)}table{border-collapse:collapse;width:100%;margin:16px 0}"
    "td,th{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:top}"
    ".ok{color:var(--ok);font-weight:600}.bad{color:var(--bad);font-weight:600}"
    "fieldset{border:1px solid var(--line);border-radius:8px;margin:12px 0;padding:8px 12px}"
    "label{display:block;margin:6px 0}input,select{font:inherit;width:100%;box-sizing:border-box;padding:6px;"
    "border:1px solid var(--line);border-radius:6px;background:transparent;color:inherit}"
    ".row{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px}"
    "button{font:inherit;padding:8px 18px;border:0;border-radius:6px;background:var(--fg);color:var(--bg);cursor:pointer}"
    ".note{border-left:3px solid var(--acc);padding:4px 10px}</style></head><body><main>";

static void send_status(httpd_req_t *req, const canary_config *cfg, const probe_status *st)
{
    char ip[16], e[192];
    net_ip(ip, sizeof ip);
    sendf(req, "<h1><span>&#9679;</span> %s</h1><p class=mut>canary " CANARY_VERSION
               " &middot; %s &middot; up %lds &middot; started by %s",
          esc(e, sizeof e, cfg->name), ip, (long)uptime_s(), app.reset_reason);
    int rssi = net_rssi();
    if (rssi) {
        sendf(req, " &middot; Wi-Fi %d dBm", rssi);
    }
    sendf(req, "</p><table><tr><th>Probe</th><th>Reading</th><th>Reported</th><th>Server</th></tr>");
    bool any = false;
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        const probe_cfg *p = &cfg->probes[i];
        if (p->type == PROBE_NONE) {
            continue;
        }
        any = true;
        char reading[32] = "-";
        if (p->type == PROBE_TEMP && st[i].reading.valid) {
            snprintf(reading, sizeof reading, "%.2f &deg;C", (double)st[i].reading.value);
        } else if (p->type == PROBE_CONTACT && st[i].reading.valid) {
            snprintf(reading, sizeof reading, "%s", st[i].reading.value != 0.0f ? "active" : "idle");
        } else if (p->type != PROBE_ALIVE) {
            snprintf(reading, sizeof reading, "sensor error");
        }
        char red[96];
        ohp_redact(red, sizeof red, p->url);
        char server[48];
        if (st[i].last_sent_s < 0) {
            snprintf(server, sizeof server, "-");
        } else {
            snprintf(server, sizeof server, "HTTP %d, %llds ago", st[i].last_status,
                     (long long)(uptime_s() - st[i].last_sent_s));
        }
        sendf(req, "<tr><td>%d. %s<br><span class=mut>%s</span></td><td>%s</td><td class=%s>%s</td><td>%s</td></tr>",
              i + 1, probe_type_name(p->type), esc(e, sizeof e, red), reading,
              !st[i].started ? "mut" : (st[i].reported_ok ? "ok" : "bad"),
              !st[i].started ? "-" : (st[i].reported_ok ? "fine" : "problem"), server);
    }
    if (!any) {
        sendf(req, "<tr><td colspan=4 class=mut>No probes yet: add one below.</td></tr>");
    }
    sendf(req, "</table>");
}

static void send_form(httpd_req_t *req, const canary_config *cfg, const char *error)
{
    char e[300], a[16], b[16], c[16];
    if (error) {
        sendf(req, "<p class='note bad'>%s</p>", esc(e, sizeof e, error));
    }
    sendf(req, "<form method=post action=/save><fieldset><legend>Device</legend>"
               "<div class=row><label>Name (also name.local)<input name=name value='%s' required></label>"
               "<label>Status LED"
               "<select name=led><option value=on%s>on<option value=off%s>off</select></label></div></fieldset>",
          esc(e, sizeof e, cfg->name), cfg->led ? " selected" : "", cfg->led ? "" : " selected");
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        const probe_cfg *p = &cfg->probes[i];
        int k = i + 1;
        sendf(req, "<fieldset><legend>Probe %d</legend><div class=row><label>Type<select name=probe%d.type>", k, k);
        const char *types[] = {"none", "alive", "temp", "contact"};
        const char *labels[] = {"off", "alive (power &amp; network)", "temperature (DS18B20)", "contact (leak, float, door)"};
        for (int t = 0; t < 4; t++) {
            sendf(req, "<option value=%s%s>%s", types[t], (int)p->type == t ? " selected" : "", labels[t]);
        }
        sendf(req, "</select></label><label>Every (s)<input name=probe%d.interval type=number min=10 value=%lu></label>"
                   "<label>Alert after (s)<input name=probe%d.delay type=number min=0 value=%lu></label></div>",
              k, (unsigned long)p->interval_s, k, (unsigned long)p->delay_s);
        sendf(req, "<label>Check URL (OHP / Healthchecks.io ping URL)<input name=probe%d.url type=url value='%s' "
                   "placeholder='https://hc-ping.com/your-uuid'></label>",
              k, esc(e, sizeof e, p->url));
        fmt_float(a, sizeof a, p->min);
        fmt_float(b, sizeof b, p->max);
        fmt_float(c, sizeof c, p->hysteresis);
        char pin[12] = "";
        if (p->pin >= 0) {
            snprintf(pin, sizeof pin, "%d", p->pin);
        }
        sendf(req, "<div class=row><label>GPIO<input name=probe%d.pin type=number min=0 max=63 value='%s'></label>"
                   "<label>Min &deg;C<input name=probe%d.min inputmode=decimal value='%s'></label>"
                   "<label>Max &deg;C<input name=probe%d.max inputmode=decimal value='%s'></label>",
              k, pin, k, a, k, b);
        sendf(req, "<label>Hysteresis<input name=probe%d.hysteresis inputmode=decimal value='%s'></label>"
                   "<label>Contact active<select name=probe%d.active><option value=low%s>low<option value=high%s>high"
                   "</select></label></div></fieldset>",
              k, c, k, p->active_high ? "" : " selected", p->active_high ? " selected" : "");
    }
    sendf(req, "<button>Save and restart</button></form>");
}

static esp_err_t get_root(httpd_req_t *req)
{
    canary_config *cfg = malloc(sizeof *cfg);
    probe_status *st = malloc(sizeof app.probes);
    if (!cfg || !st) {
        free(cfg);
        free(st);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    xSemaphoreTake(app.lock, portMAX_DELAY);
    *cfg = app.cfg;
    memcpy(st, app.probes, sizeof app.probes);
    xSemaphoreGive(app.lock);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    send_status(req, cfg, st);
    if (app_setup_open()) {
        send_form(req, cfg, NULL);
    } else {
        sendf(req, "<p class=note>Settings are locked. To change them, press the BOOT button or "
                   "power-cycle the device: the setup page stays open for 15 minutes.</p>");
    }
    sendf(req, "<p class=mut><a href=https://github.com/AndreaAcanfora/canary>canary on GitHub</a></p></main>");
    httpd_resp_send_chunk(req, NULL, 0);
    free(cfg);
    free(st);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t post_save(httpd_req_t *req)
{
    if (!app_setup_open()) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "settings are locked: press BOOT or power-cycle");
    }
    // Refuse forms posted by other web sites (CSRF): browsers send Origin.
    char origin[96] = "", host[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) == ESP_OK) {
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
        const char *o = strncmp(origin, "http://", 7) == 0 ? origin + 7 : origin;
        if (strcmp(o, host) != 0) {
            return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "cross-site request refused");
        }
    }
    if (req->content_len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form too large");
    }
    char *body = calloc(1, req->content_len + 1);
    canary_config *cfg = malloc(sizeof *cfg);
    if (!body || !cfg) {
        free(body);
        free(cfg);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            free(cfg);
            return ESP_FAIL;
        }
        got += (size_t)r;
    }

    // The form lists every field, so start from the defaults: a probe set
    // to "off" loses its old settings.
    canary_config_defaults(cfg);
    char err[96];
    bool ok = canary_form_apply(cfg, body, err, sizeof err);
    free(body);
    if (ok && !store_save(cfg)) {
        ok = false;
        snprintf(err, sizeof err, "could not save to flash");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    if (ok) {
        sendf(req, "<h1><span>&#9679;</span> Saved</h1><p>Restarting&hellip; this page reloads in a few seconds.</p>"
                   "<script>setTimeout(()=>location.href='/',6000)</script></main>");
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGI(TAG, "configuration saved, restarting");
        xTaskCreate(restart_task, "restart", 2048, NULL, 1, NULL);
    } else {
        send_form(req, cfg, err);
        sendf(req, "</main>");
        httpd_resp_send_chunk(req, NULL, 0);
    }
    free(cfg);
    return ESP_OK;
}

static esp_err_t get_status_json(httpd_req_t *req)
{
    canary_config *cfg = malloc(sizeof *cfg);
    probe_status *st = malloc(sizeof app.probes);
    if (!cfg || !st) {
        free(cfg);
        free(st);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    xSemaphoreTake(app.lock, portMAX_DELAY);
    *cfg = app.cfg;
    memcpy(st, app.probes, sizeof app.probes);
    xSemaphoreGive(app.lock);

    char e[96];
    httpd_resp_set_type(req, "application/json");
    sendf(req, "{\"name\":\"%s\",\"version\":\"" CANARY_VERSION "\",\"uptime_s\":%lld,\"reset_reason\":\"%s\","
               "\"rssi\":%d,\"setup_open\":%s,\"probes\":[",
          cfg->name, (long long)uptime_s(), app.reset_reason, net_rssi(), app_setup_open() ? "true" : "false");
    bool first = true;
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        const probe_cfg *p = &cfg->probes[i];
        if (p->type == PROBE_NONE) {
            continue;
        }
        ohp_redact(e, sizeof e, p->url);
        char value[24] = "null";
        if (st[i].reading.valid && p->type != PROBE_ALIVE) {
            snprintf(value, sizeof value, "%g", (double)st[i].reading.value);
        }
        sendf(req, "%s{\"probe\":%d,\"type\":\"%s\",\"url\":\"%s\",\"value\":%s,\"state\":\"%s\",\"last_status\":%d}",
              first ? "" : ",", i + 1, probe_type_name(p->type), e, value,
              !st[i].started ? "new" : (st[i].reported_ok ? "ok" : "problem"), st[i].last_status);
        first = false;
    }
    sendf(req, "]}");
    httpd_resp_send_chunk(req, NULL, 0);
    free(cfg);
    free(st);
    return ESP_OK;
}

void web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server did not start");
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_root},
        {.uri = "/save", .method = HTTP_POST, .handler = post_save},
        {.uri = "/api/status", .method = HTTP_GET, .handler = get_status_json},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
