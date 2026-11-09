// canary for Linux (Raspberry Pi and any other machine): the same probes as
// the ESP32 firmware, reading sensors from files such as
// /sys/bus/w1/devices/28-*/temperature or /sys/class/gpio/gpio17/value.
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "canary/config.h"
#include "canary/ohp.h"
#include "canary/probe.h"
#include "linux.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static uint32_t now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t n;
    while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 == cap) {
            char *bigger = realloc(buf, cap *= 2);
            if (!bigger) {
                free(buf);
                buf = NULL;
            }
            buf = bigger;
        }
    }
    fclose(f);
    if (buf) {
        buf[len] = '\0';
    }
    return buf;
}

static probe_reading read_probe(const probe_cfg *p)
{
    probe_reading r = {0};
    if (p->type == PROBE_ALIVE) {
        r.valid = true;
        return r;
    }
    char *text = p->source[0] ? read_file(p->source) : NULL;
    if (!text) {
        return r;
    }
    // The 1-Wire w1_slave format ends with "t=21375"; plain files hold a number.
    const char *num = strstr(text, "t=");
    num = num ? num + 2 : text;
    char *end;
    errno = 0;
    double v = strtod(num, &end);
    if (end != num && errno == 0 && isfinite(v)) {
        r.valid = true;
        r.value = (float)(v * p->scale);
        if (p->type == PROBE_CONTACT) {
            bool high = r.value != 0.0f;
            r.value = high == p->active_high ? 1.0f : 0.0f;
        }
    }
    free(text);
    return r;
}

static void usage(void)
{
    fputs("usage: canary [-c config] [-v] [-n]\n"
          "  -c file  configuration (default /etc/canary.conf)\n"
          "  -v       log every signal\n"
          "  -n       check the configuration and exit\n"
          "  -V       print the version\n",
          stderr);
}

int main(int argc, char **argv)
{
    const char *path = "/etc/canary.conf";
    bool verbose = false, check_only = false;
    int opt;
    while ((opt = getopt(argc, argv, "c:vnVh")) != -1) {
        switch (opt) {
        case 'c':
            path = optarg;
            break;
        case 'v':
            verbose = true;
            break;
        case 'n':
            check_only = true;
            break;
        case 'V':
            puts("canary " CANARY_VERSION " (OHP " OHP_VERSION ")");
            return 0;
        default:
            usage();
            return 2;
        }
    }

    char *text = read_file(path);
    if (!text) {
        fprintf(stderr, "canary: %s: %s\n", path, strerror(errno));
        return 1;
    }
    canary_config cfg;
    canary_config_defaults(&cfg);
    char err[128];
    int bad = canary_config_parse(&cfg, text, err, sizeof err);
    free(text);
    if (bad || !canary_config_validate(&cfg, err, sizeof err)) {
        fprintf(stderr, "canary: %s: %s\n", path, err);
        return 1;
    }
    int active = 0;
    for (int i = 0; i < CANARY_MAX_PROBES; i++) {
        active += cfg.probes[i].type != PROBE_NONE;
    }
    if (active == 0) {
        fprintf(stderr, "canary: %s: no probes configured\n", path);
        return 1;
    }
    if (check_only) {
        printf("%s: ok, %d probe%s\n", path, active, active == 1 ? "" : "s");
        return 0;
    }
    if (http_init(err, sizeof err) != 0) {
        fprintf(stderr, "canary: %s\n", err);
        return 1;
    }

    http_options http = {.verbose = verbose};
    gethostname(http.host, sizeof http.host - 1);
    ohp_transport t = http_transport(&http);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    probe_state state[CANARY_MAX_PROBES] = {0};
    uint32_t start = now_s();
    fprintf(stderr, "canary %s: %d probe%s, config %s\n", CANARY_VERSION, active, active == 1 ? "" : "s", path);

    while (!stop) {
        uint32_t now = now_s();
        for (int i = 0; i < CANARY_MAX_PROBES && !stop; i++) {
            const probe_cfg *p = &cfg.probes[i];
            if (p->type == PROBE_NONE) {
                continue;
            }
            probe_action act;
            probe_step(p, &state[i], read_probe(p), now, &act);
            if (!act.send) {
                continue;
            }
            char body[PROBE_BODY_MAX + 64];
            snprintf(body, sizeof body, "%s uptime_s=%lu", act.body, (unsigned long)(now - start));
            int status = ohp_send(&t, p->url, act.sig, 0, NULL, body);
            if (verbose || ohp_classify(status) != OHP_DONE) {
                char red[96];
                ohp_redact(red, sizeof red, p->url);
                fprintf(stderr, "canary: probe%d %s %s -> %d (%s)\n", i + 1, act.sig == OHP_FAIL ? "fail" : "success",
                        red, status, act.body);
            }
        }
        sleep(1);
    }
    return 0;
}
