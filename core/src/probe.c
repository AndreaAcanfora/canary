#include "canary/probe.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void probe_cfg_defaults(probe_cfg *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->type = PROBE_NONE;
    cfg->interval_s = 60;
    cfg->min = NAN;
    cfg->max = NAN;
    cfg->hysteresis = 0.5f;
    cfg->pin = -1;
    cfg->scale = 1.0f;
    cfg->active_high = false;
}

const char *probe_type_name(probe_type t)
{
    switch (t) {
    case PROBE_ALIVE:
        return "alive";
    case PROBE_TEMP:
        return "temp";
    case PROBE_CONTACT:
        return "contact";
    default:
        return "none";
    }
}

// Evaluates the condition, with hysteresis around the temperature limits.
// why receives a short reason when the condition is not fine.
static bool evaluate(const probe_cfg *cfg, bool was_ok, probe_reading r, const char **why)
{
    *why = "";
    switch (cfg->type) {
    case PROBE_TEMP: {
        if (!r.valid || isnan(r.value)) {
            *why = "sensor error";
            return false;
        }
        float margin = was_ok ? 0.0f : cfg->hysteresis;
        if (!isnan(cfg->max) && r.value > cfg->max - margin) {
            *why = r.value > cfg->max ? "above max" : "above max - hysteresis";
            return false;
        }
        if (!isnan(cfg->min) && r.value < cfg->min + margin) {
            *why = r.value < cfg->min ? "below min" : "below min + hysteresis";
            return false;
        }
        return true;
    }
    case PROBE_CONTACT:
        if (!r.valid) {
            *why = "sensor error";
            return false;
        }
        if (r.value != 0.0f) {
            *why = "contact active";
            return false;
        }
        return true;
    default:
        return true;
    }
}

void probe_step(const probe_cfg *cfg, probe_state *st, probe_reading r, uint32_t now_s, probe_action *act)
{
    memset(act, 0, sizeof *act);
    if (cfg->type == PROBE_NONE || cfg->url[0] == '\0') {
        return;
    }

    // Before the first signal there is no history: judge the reading against
    // the plain limits, as if the previous state were fine.
    bool was_ok = st->started ? st->cond_ok : true;
    const char *why;
    bool ok = evaluate(cfg, was_ok, r, &why);
    if (!ok && (was_ok || !st->started)) {
        st->bad_since = now_s;
    }
    st->cond_ok = ok;

    // A problem is reported only after it lasted delay_s; recovery is
    // reported at once. While waiting, keep reporting the previous state.
    bool report_ok = ok || (now_s - st->bad_since < cfg->delay_s && (!st->started || st->reported_ok));

    bool due = !st->started || report_ok != st->reported_ok || now_s - st->last_sent >= cfg->interval_s;
    if (!due) {
        return;
    }

    act->send = true;
    act->sig = report_ok ? OHP_SUCCESS : OHP_FAIL;
    switch (cfg->type) {
    case PROBE_TEMP:
        if (r.valid && !isnan(r.value)) {
            snprintf(act->body, sizeof act->body, "temp_c=%.2f", (double)r.value);
        } else {
            snprintf(act->body, sizeof act->body, "temp_c=unknown");
        }
        break;
    case PROBE_CONTACT:
        snprintf(act->body, sizeof act->body, "contact=%s",
                 !r.valid ? "unknown" : (r.value != 0.0f ? "active" : "idle"));
        break;
    default:
        snprintf(act->body, sizeof act->body, "alive");
        break;
    }
    if (!report_ok) {
        size_t len = strlen(act->body);
        snprintf(act->body + len, sizeof act->body - len, " problem=\"%s\"", why);
    }

    st->started = true;
    st->reported_ok = report_ok;
    st->last_sent = now_s;
}
