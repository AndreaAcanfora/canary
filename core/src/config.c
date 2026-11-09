#include "canary/config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void canary_config_defaults(canary_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    strcpy(cfg->name, "canary");
    cfg->led = true;
    cfg->led_pin = -1;
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        probe_cfg_defaults(&cfg->probes[i]);
    }
}

static bool parse_float(const char *s, float *out)
{
    if (*s == '\0') {
        *out = NAN; // empty clears the limit
        return true;
    }
    char *end;
    errno = 0;
    float v = strtof(s, &end);
    if (errno || *end != '\0' || isnan(v) || isinf(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_uint(const char *s, unsigned long max, unsigned long *out)
{
    if (!isdigit((unsigned char)*s)) {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end != '\0' || v > max) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_bool(const char *s, bool *out)
{
    if (!strcmp(s, "on") || !strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "yes")) {
        *out = true;
        return true;
    }
    if (!strcmp(s, "off") || !strcmp(s, "false") || !strcmp(s, "0") || !strcmp(s, "no")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_pin(const char *s, int *out)
{
    if (*s == '\0' || !strcmp(s, "-1")) {
        *out = -1;
        return true;
    }
    unsigned long v;
    if (!parse_uint(s, 63, &v)) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= CANARY_NAME_MAX || s[0] == '-' || s[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '-') {
            return false;
        }
    }
    return true;
}

static config_result set_probe(probe_cfg *p, const char *field, const char *value)
{
    unsigned long u;
    if (!strcmp(field, "type")) {
        if (!strcmp(value, "none") || *value == '\0') {
            p->type = PROBE_NONE;
        } else if (!strcmp(value, "alive")) {
            p->type = PROBE_ALIVE;
        } else if (!strcmp(value, "temp")) {
            p->type = PROBE_TEMP;
        } else if (!strcmp(value, "contact")) {
            p->type = PROBE_CONTACT;
        } else {
            return CONFIG_BAD_VALUE;
        }
    } else if (!strcmp(field, "url")) {
        if (strlen(value) >= sizeof p->url) {
            return CONFIG_BAD_VALUE;
        }
        strcpy(p->url, value);
    } else if (!strcmp(field, "interval")) {
        if (!parse_uint(value, 86400, &u) || u < 10) {
            return CONFIG_BAD_VALUE;
        }
        p->interval_s = (uint32_t)u;
    } else if (!strcmp(field, "delay")) {
        if (!parse_uint(value, 86400, &u)) {
            return CONFIG_BAD_VALUE;
        }
        p->delay_s = (uint32_t)u;
    } else if (!strcmp(field, "min")) {
        return parse_float(value, &p->min) ? CONFIG_OK : CONFIG_BAD_VALUE;
    } else if (!strcmp(field, "max")) {
        return parse_float(value, &p->max) ? CONFIG_OK : CONFIG_BAD_VALUE;
    } else if (!strcmp(field, "hysteresis")) {
        float h;
        if (!parse_float(value, &h) || isnan(h) || h < 0) {
            return CONFIG_BAD_VALUE;
        }
        p->hysteresis = h;
    } else if (!strcmp(field, "source")) {
        if (strlen(value) >= sizeof p->source) {
            return CONFIG_BAD_VALUE;
        }
        strcpy(p->source, value);
    } else if (!strcmp(field, "scale")) {
        float f;
        if (!parse_float(value, &f) || isnan(f) || f == 0.0f) {
            return CONFIG_BAD_VALUE;
        }
        p->scale = f;
    } else if (!strcmp(field, "pin")) {
        return parse_pin(value, &p->pin) ? CONFIG_OK : CONFIG_BAD_VALUE;
    } else if (!strcmp(field, "active")) {
        if (!strcmp(value, "high")) {
            p->active_high = true;
        } else if (!strcmp(value, "low")) {
            p->active_high = false;
        } else {
            return CONFIG_BAD_VALUE;
        }
    } else {
        return CONFIG_UNKNOWN_KEY;
    }
    return CONFIG_OK;
}

config_result canary_config_set(canary_config *cfg, const char *key, const char *value)
{
    if (!strcmp(key, "name")) {
        if (!valid_name(value)) {
            return CONFIG_BAD_VALUE;
        }
        strcpy(cfg->name, value);
        return CONFIG_OK;
    }
    if (!strcmp(key, "led")) {
        return parse_bool(value, &cfg->led) ? CONFIG_OK : CONFIG_BAD_VALUE;
    }
    if (!strcmp(key, "led.pin")) {
        return parse_pin(value, &cfg->led_pin) ? CONFIG_OK : CONFIG_BAD_VALUE;
    }
    if (!strncmp(key, "probe", 5) && key[5] >= '1' && key[5] < '1' + CANARY_MAX_PROBES && key[6] == '.') {
        return set_probe(&cfg->probes[key[5] - '1'], key + 7, value);
    }
    return CONFIG_UNKNOWN_KEY;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = '\0';
    }
    return s;
}

int canary_config_parse(canary_config *cfg, const char *text, char *err, size_t err_n)
{
    if (err_n) {
        err[0] = '\0';
    }
    int line_no = 0;
    const char *p = text;
    while (*p) {
        line_no++;
        size_t len = strcspn(p, "\n");
        char line[PROBE_URL_MAX + 64];
        if (len >= sizeof line) {
            snprintf(err, err_n, "line %d: too long", line_no);
            return line_no;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p += len;
        if (*p == '\n') {
            p++;
        }

        char *s = trim(line);
        if (*s == '\0' || *s == '#') {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, err_n, "line %d: expected key=value", line_no);
            return line_no;
        }
        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);
        switch (canary_config_set(cfg, key, value)) {
        case CONFIG_OK:
            break;
        case CONFIG_UNKNOWN_KEY:
            snprintf(err, err_n, "line %d: unknown key \"%.40s\"", line_no, key);
            return line_no;
        default:
            snprintf(err, err_n, "line %d: bad value for %.40s", line_no, key);
            return line_no;
        }
    }
    return 0;
}

bool canary_config_validate(const canary_config *cfg, char *err, size_t err_n)
{
    if (!valid_name(cfg->name)) {
        snprintf(err, err_n, "name: use letters, digits and '-'");
        return false;
    }
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        const probe_cfg *p = &cfg->probes[i];
        if (p->type == PROBE_NONE) {
            continue;
        }
        if (strncmp(p->url, "https://", 8) != 0 && strncmp(p->url, "http://", 7) != 0) {
            snprintf(err, err_n, "probe%d.url: an http(s) check URL is required", i + 1);
            return false;
        }
        if ((p->type == PROBE_TEMP || p->type == PROBE_CONTACT) && p->pin < 0 && p->source[0] == '\0') {
            snprintf(err, err_n, "probe%d.pin: a GPIO number (or a source file on Linux) is required", i + 1);
            return false;
        }
        if (p->type == PROBE_TEMP && isnan(p->min) && isnan(p->max)) {
            snprintf(err, err_n, "probe%d: set min, max or both", i + 1);
            return false;
        }
        if (p->type == PROBE_TEMP && !isnan(p->min) && !isnan(p->max) && p->max - p->min <= 2 * p->hysteresis) {
            snprintf(err, err_n, "probe%d: max - min must exceed twice the hysteresis", i + 1);
            return false;
        }
    }
    return true;
}

__attribute__((format(printf, 4, 5))) static size_t append(char *out, size_t n, size_t pos, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(pos < n ? out + pos : NULL, pos < n ? n - pos : 0, fmt, ap);
    va_end(ap);
    return pos + (w > 0 ? (size_t)w : 0);
}

size_t canary_config_format(const canary_config *cfg, char *out, size_t n)
{
    size_t pos = 0;
    if (n) {
        out[0] = '\0';
    }
    pos = append(out, n, pos, "name=%s\nled=%s\n", cfg->name, cfg->led ? "on" : "off");
    if (cfg->led_pin >= 0) {
        pos = append(out, n, pos, "led.pin=%d\n", cfg->led_pin);
    }
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        const probe_cfg *p = &cfg->probes[i];
        if (p->type == PROBE_NONE) {
            continue;
        }
        int k = i + 1;
        pos = append(out, n, pos, "probe%d.type=%s\nprobe%d.url=%s\nprobe%d.interval=%lu\n", k,
                     probe_type_name(p->type), k, p->url, k, (unsigned long)p->interval_s);
        if (p->delay_s) {
            pos = append(out, n, pos, "probe%d.delay=%lu\n", k, (unsigned long)p->delay_s);
        }
        if (p->pin >= 0) {
            pos = append(out, n, pos, "probe%d.pin=%d\n", k, p->pin);
        }
        if (p->source[0]) {
            pos = append(out, n, pos, "probe%d.source=%s\n", k, p->source);
        }
        if (p->scale != 1.0f) {
            pos = append(out, n, pos, "probe%d.scale=%g\n", k, (double)p->scale);
        }
        if (p->type == PROBE_TEMP) {
            if (!isnan(p->min)) {
                pos = append(out, n, pos, "probe%d.min=%g\n", k, (double)p->min);
            }
            if (!isnan(p->max)) {
                pos = append(out, n, pos, "probe%d.max=%g\n", k, (double)p->max);
            }
            pos = append(out, n, pos, "probe%d.hysteresis=%g\n", k, (double)p->hysteresis);
        }
        if (p->type == PROBE_CONTACT) {
            pos = append(out, n, pos, "probe%d.active=%s\n", k, p->active_high ? "high" : "low");
        }
    }
    return pos;
}
