// Probes turn sensor readings into OHP signals.
//
// A probe reports its state to one check URL: a success signal while the
// condition is fine, a fail signal when it is not. It sends at every interval
// and immediately when the state changes, so the heartbeat server alerts both
// when the device goes silent (power cut, network down, crash) and when the
// device reports a problem (freezer too warm, water on the floor).
#ifndef CANARY_PROBE_H
#define CANARY_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "canary/ohp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROBE_URL_MAX 256
#define PROBE_BODY_MAX 160

typedef enum {
    PROBE_NONE,
    PROBE_ALIVE,   // always fine: proves power, network and firmware are up
    PROBE_TEMP,    // temperature (DS18B20) within min..max
    PROBE_CONTACT, // digital input (leak sensor, float switch, door) not active
} probe_type;

typedef struct {
    probe_type type;
    char url[PROBE_URL_MAX];
    uint32_t interval_s; // heartbeat period
    float min, max;      // PROBE_TEMP limits, NAN when unset
    float hysteresis;    // PROBE_TEMP: margin needed to return to fine
    uint32_t delay_s;    // a problem must last this long before it is reported
    int pin;             // GPIO of the sensor (PROBE_TEMP, PROBE_CONTACT)
    char source[96];     // Linux: file holding the reading, e.g. a sysfs path
    float scale;         // Linux: multiplier for the number read from source
    bool active_high;    // PROBE_CONTACT: level that means "problem"
} probe_cfg;

typedef struct {
    bool started;     // a signal was sent at least once
    bool reported_ok; // state of the last signal
    bool cond_ok;     // condition after hysteresis, before delay
    uint32_t bad_since;
    uint32_t last_sent;
} probe_state;

typedef struct {
    bool valid;  // false: the sensor could not be read
    float value; // PROBE_TEMP: degrees Celsius; PROBE_CONTACT: 1 when active
} probe_reading;

typedef struct {
    bool send;
    ohp_signal sig; // OHP_SUCCESS or OHP_FAIL
    char body[PROBE_BODY_MAX];
} probe_action;

void probe_cfg_defaults(probe_cfg *cfg);

// Evaluates a reading taken at now_s (monotonic seconds) and decides whether
// a signal is due. Call it at every sample, for example once per second.
void probe_step(const probe_cfg *cfg, probe_state *st, probe_reading r, uint32_t now_s, probe_action *act);

const char *probe_type_name(probe_type t);

#ifdef __cplusplus
}
#endif

#endif
