// Device configuration as "key=value" lines.
//
//   name=freezer
//   probe1.type=temp
//   probe1.url=https://hc-ping.com/<uuid>
//   probe1.interval=300
//   probe1.max=-15
//
// The same text format is stored in flash, read from /etc/canary.conf on
// Linux and posted by the setup page, whose form fields use the same keys.
#ifndef CANARY_CONFIG_H
#define CANARY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "canary/probe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CANARY_MAX_PROBES 4
#define CANARY_NAME_MAX 32

typedef struct {
    char name[CANARY_NAME_MAX]; // device name, also the mDNS host name
    bool led;                   // status LED on
    int led_pin;                // -1: board default
    probe_cfg probes[CANARY_MAX_PROBES];
} canary_config;

typedef enum {
    CONFIG_OK = 0,
    CONFIG_UNKNOWN_KEY = -1,
    CONFIG_BAD_VALUE = -2,
} config_result;

void canary_config_defaults(canary_config *cfg);

// Sets one key. Probes are numbered from 1 in keys ("probe1.url").
config_result canary_config_set(canary_config *cfg, const char *key, const char *value);

// Parses "key=value" lines; blank lines and lines starting with '#' are
// skipped, spaces around keys and values are trimmed. Stops at the first
// error and describes it in err. Returns the number of the bad line, or 0.
int canary_config_parse(canary_config *cfg, const char *text, char *err, size_t err_n);

// Checks the whole configuration (URLs, limits, pins). Returns false and
// describes the first problem in err.
bool canary_config_validate(const canary_config *cfg, char *err, size_t err_n);

// Writes the configuration as "key=value" lines. Returns the length needed,
// like snprintf.
size_t canary_config_format(const canary_config *cfg, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
