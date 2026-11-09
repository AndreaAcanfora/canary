// Host tests for the portable core: make test
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "canary/config.h"
#include "canary/ds18b20.h"
#include "canary/form.h"
#include "canary/improv.h"
#include "canary/ohp.h"
#include "canary/probe.h"

static int failures, checks;

#define CHECK(cond)                                                   \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                             \
    } while (0)

#define CHECK_STR(got, want)                                                       \
    do {                                                                           \
        checks++;                                                                  \
        if (strcmp((got), (want)) != 0) {                                          \
            failures++;                                                            \
            fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); \
        }                                                                          \
    } while (0)

static void test_build_url(void)
{
    char u[128];
    const char *rid = "0f8fad5b-d9cb-469f-a165-70867728950e";
    CHECK(ohp_build_url(u, sizeof u, "https://hc-ping.com/abc", OHP_SUCCESS, 0, NULL) == 0);
    CHECK_STR(u, "https://hc-ping.com/abc");
    CHECK(ohp_build_url(u, sizeof u, "https://hc-ping.com/abc/", OHP_FAIL, 0, NULL) == 0);
    CHECK_STR(u, "https://hc-ping.com/abc/fail");
    CHECK(ohp_build_url(u, sizeof u, "https://x/abc", OHP_START, 0, rid) == 0);
    CHECK_STR(u, "https://x/abc/start?rid=0f8fad5b-d9cb-469f-a165-70867728950e");
    CHECK(ohp_build_url(u, sizeof u, "https://x/abc", OHP_LOG, 0, NULL) == 0);
    CHECK_STR(u, "https://x/abc/log");
    CHECK(ohp_build_url(u, sizeof u, "https://x/abc", OHP_EXIT, 7, NULL) == 0);
    CHECK_STR(u, "https://x/abc/7");
    CHECK(ohp_build_url(u, sizeof u, "https://x/abc", OHP_EXIT, 256, NULL) == -1);
    CHECK(ohp_build_url(u, sizeof u, "https://x/p/k/s?create=1", OHP_FAIL, 0, NULL) == 0);
    CHECK_STR(u, "https://x/p/k/s/fail?create=1");
    CHECK(ohp_build_url(u, sizeof u, "https://x/p/k/s/?create=1", OHP_SUCCESS, 0, rid) == 0);
    CHECK_STR(u, "https://x/p/k/s?create=1&rid=0f8fad5b-d9cb-469f-a165-70867728950e");
    CHECK(ohp_build_url(u, sizeof u, "ftp://x/abc", OHP_SUCCESS, 0, NULL) == -1);
    CHECK(ohp_build_url(u, 10, "https://hc-ping.com/abc", OHP_SUCCESS, 0, NULL) == -1);
    CHECK(u[0] == '\0');
}

static void test_uuid(void)
{
    uint8_t r[16];
    memset(r, 0xff, sizeof r);
    char s[37];
    ohp_uuid4(s, r);
    CHECK_STR(s, "ffffffff-ffff-4fff-bfff-ffffffffffff");
    memset(r, 0, sizeof r);
    ohp_uuid4(s, r);
    CHECK_STR(s, "00000000-0000-4000-8000-000000000000");
}

static void test_retry(void)
{
    CHECK(ohp_classify(200) == OHP_DONE);
    CHECK(ohp_classify(201) == OHP_DONE);
    CHECK(ohp_classify(0) == OHP_RETRY);
    CHECK(ohp_classify(-1) == OHP_RETRY);
    CHECK(ohp_classify(429) == OHP_RETRY);
    CHECK(ohp_classify(503) == OHP_RETRY);
    CHECK(ohp_classify(400) == OHP_GIVE_UP);
    CHECK(ohp_classify(404) == OHP_GIVE_UP);
    CHECK(ohp_retry_delay_ms(1, 0, -1) == 1000);
    CHECK(ohp_retry_delay_ms(2, 503, -1) == 2000);
    CHECK(ohp_retry_delay_ms(4, 0, -1) == 8000);
    CHECK(ohp_retry_delay_ms(1, 429, 30) == 30000);
    CHECK(ohp_retry_delay_ms(1, 429, 120) == 1000); // too large: ignored
    CHECK(ohp_retry_delay_ms(1, 503, 30) == 1000);  // only for 429
}

static void test_redact(void)
{
    char r[64];
    ohp_redact(r, sizeof r, "https://hc-ping.com/e0252e78-54aa");
    CHECK_STR(r, "https://hc-ping.com/\xe2\x80\xa6");
    ohp_redact(r, sizeof r, "http://10.0.2.2:8080/ping/key/slug?create=1");
    CHECK_STR(r, "http://10.0.2.2:8080/\xe2\x80\xa6");
    ohp_redact(r, sizeof r, "nonsense");
    CHECK_STR(r, "(invalid URL)");
}

static probe_reading temp(float v)
{
    return (probe_reading){.valid = true, .value = v};
}

static void test_probe_alive(void)
{
    probe_cfg c;
    probe_cfg_defaults(&c);
    c.type = PROBE_ALIVE;
    strcpy(c.url, "https://x/a");
    c.interval_s = 60;
    probe_state st = {0};
    probe_action a;
    probe_reading none = {0};

    probe_step(&c, &st, none, 1000, &a);
    CHECK(a.send && a.sig == OHP_SUCCESS);
    CHECK_STR(a.body, "alive");
    probe_step(&c, &st, none, 1059, &a);
    CHECK(!a.send);
    probe_step(&c, &st, none, 1060, &a);
    CHECK(a.send && a.sig == OHP_SUCCESS);

    // No URL: never sends.
    probe_state st2 = {0};
    c.url[0] = '\0';
    probe_step(&c, &st2, none, 0, &a);
    CHECK(!a.send);
}

static void test_probe_temp_hysteresis(void)
{
    probe_cfg c;
    probe_cfg_defaults(&c);
    c.type = PROBE_TEMP;
    strcpy(c.url, "https://x/t");
    c.max = -15.0f;
    c.hysteresis = 1.0f;
    c.interval_s = 300;
    probe_state st = {0};
    probe_action a;

    probe_step(&c, &st, temp(-18.0f), 0, &a);
    CHECK(a.send && a.sig == OHP_SUCCESS);
    CHECK_STR(a.body, "temp_c=-18.00");

    probe_step(&c, &st, temp(-15.0f), 1, &a); // at the limit: still fine
    CHECK(!a.send);

    probe_step(&c, &st, temp(-14.5f), 2, &a); // above: reported at once
    CHECK(a.send && a.sig == OHP_FAIL);
    CHECK_STR(a.body, "temp_c=-14.50 problem=\"above max\"");

    probe_step(&c, &st, temp(-15.5f), 3, &a); // back under max, not by the hysteresis
    CHECK(!a.send);

    probe_step(&c, &st, temp(-16.0f), 4, &a); // max - hysteresis: fine again
    CHECK(a.send && a.sig == OHP_SUCCESS);

    probe_step(&c, &st, (probe_reading){.valid = false}, 5, &a);
    CHECK(a.send && a.sig == OHP_FAIL);
    CHECK_STR(a.body, "temp_c=unknown problem=\"sensor error\"");
}

static void test_probe_min(void)
{
    probe_cfg c;
    probe_cfg_defaults(&c);
    c.type = PROBE_TEMP;
    strcpy(c.url, "https://x/t");
    c.min = 2.0f;
    c.max = 8.0f;
    probe_state st = {0};
    probe_action a;

    probe_step(&c, &st, temp(1.0f), 0, &a); // bad from the first reading
    CHECK(a.send && a.sig == OHP_FAIL);
    CHECK_STR(a.body, "temp_c=1.00 problem=\"below min\"");
    probe_step(&c, &st, temp(2.2f), 1, &a);
    CHECK(!a.send);
    probe_step(&c, &st, temp(2.5f), 2, &a);
    CHECK(a.send && a.sig == OHP_SUCCESS);
}

static void test_probe_delay(void)
{
    probe_cfg c;
    probe_cfg_defaults(&c);
    c.type = PROBE_TEMP;
    strcpy(c.url, "https://x/t");
    c.max = -15.0f;
    c.delay_s = 600; // a door left open for a moment is not an alert
    c.interval_s = 300;
    probe_state st = {0};
    probe_action a;

    probe_step(&c, &st, temp(-18.0f), 0, &a);
    CHECK(a.send && a.sig == OHP_SUCCESS);
    probe_step(&c, &st, temp(-10.0f), 100, &a);
    CHECK(!a.send);
    probe_step(&c, &st, temp(-10.0f), 300, &a); // interval: still "fine"
    CHECK(a.send && a.sig == OHP_SUCCESS);
    probe_step(&c, &st, temp(-10.0f), 599, &a);
    CHECK(!a.send);
    probe_step(&c, &st, temp(-10.0f), 700, &a); // lasted 600 s
    CHECK(a.send && a.sig == OHP_FAIL);
    probe_step(&c, &st, temp(-17.0f), 710, &a); // recovery at once
    CHECK(a.send && a.sig == OHP_SUCCESS);

    // A short excursion resets the timer.
    probe_step(&c, &st, temp(-10.0f), 800, &a);
    CHECK(!a.send);
    probe_step(&c, &st, temp(-17.0f), 900, &a);
    CHECK(!a.send);
    probe_step(&c, &st, temp(-10.0f), 1000, &a);
    probe_step(&c, &st, temp(-10.0f), 1399, &a);
    CHECK(!a.send || a.sig == OHP_SUCCESS);
}

static void test_probe_contact(void)
{
    probe_cfg c;
    probe_cfg_defaults(&c);
    c.type = PROBE_CONTACT;
    strcpy(c.url, "https://x/c");
    c.pin = 4;
    probe_state st = {0};
    probe_action a;

    probe_step(&c, &st, (probe_reading){.valid = true, .value = 0}, 0, &a);
    CHECK(a.send && a.sig == OHP_SUCCESS);
    CHECK_STR(a.body, "contact=idle");
    probe_step(&c, &st, (probe_reading){.valid = true, .value = 1}, 1, &a);
    CHECK(a.send && a.sig == OHP_FAIL);
    CHECK_STR(a.body, "contact=active problem=\"contact active\"");
    probe_step(&c, &st, (probe_reading){.valid = true, .value = 1}, 2, &a);
    CHECK(!a.send);
    probe_step(&c, &st, (probe_reading){.valid = true, .value = 0}, 3, &a);
    CHECK(a.send && a.sig == OHP_SUCCESS);
}

static void test_config(void)
{
    canary_config c;
    canary_config_defaults(&c);
    char err[96];
    const char *text = "# freezer in the garage\n"
                       "name = garage-freezer\n"
                       "led=off\n"
                       "\n"
                       "probe1.type=alive\n"
                       "probe1.url=https://hc-ping.com/aaa\n"
                       "probe2.type=temp\n"
                       "probe2.url=https://hc-ping.com/bbb\n"
                       "probe2.pin=4\n"
                       "probe2.max=-15\n"
                       "probe2.delay=600\n"
                       "probe2.interval=300\r\n"
                       "probe3.type=temp\n"
                       "probe3.url=http://10.0.0.2:8080/ping/k/cpu\n"
                       "probe3.source=/sys/class/thermal/thermal_zone0/temp\n"
                       "probe3.scale=0.001\n"
                       "probe3.max=80\n";
    CHECK(canary_config_parse(&c, text, err, sizeof err) == 0);
    CHECK_STR(err, "");
    CHECK_STR(c.name, "garage-freezer");
    CHECK(!c.led);
    CHECK(c.probes[0].type == PROBE_ALIVE);
    CHECK(c.probes[1].type == PROBE_TEMP && c.probes[1].pin == 4);
    CHECK(c.probes[1].max == -15.0f && isnan(c.probes[1].min));
    CHECK(c.probes[1].interval_s == 300 && c.probes[1].delay_s == 600);
    CHECK(c.probes[2].scale == 0.001f);
    CHECK_STR(c.probes[2].source, "/sys/class/thermal/thermal_zone0/temp");
    CHECK(canary_config_validate(&c, err, sizeof err));

    // Round trip.
    char out[1024];
    size_t n = canary_config_format(&c, out, sizeof out);
    CHECK(n == strlen(out));
    canary_config c2;
    canary_config_defaults(&c2);
    CHECK(canary_config_parse(&c2, out, err, sizeof err) == 0);
    char out2[1024];
    canary_config_format(&c2, out2, sizeof out2);
    CHECK_STR(out2, out);

    // Truncated output reports the needed size.
    char small[8];
    CHECK(canary_config_format(&c, small, sizeof small) == n);
    CHECK(strlen(small) == 7);

    CHECK(canary_config_parse(&c, "probe9.url=x\n", err, sizeof err) == 1);
    CHECK_STR(err, "line 1: unknown key \"probe9.url\"");
    CHECK(canary_config_parse(&c, "\nprobe1.interval=5\n", err, sizeof err) == 2);
    CHECK_STR(err, "line 2: bad value for probe1.interval");
    CHECK(canary_config_parse(&c, "nonsense\n", err, sizeof err) == 1);
    CHECK(canary_config_set(&c, "name", "bad name") == CONFIG_BAD_VALUE);
    CHECK(canary_config_set(&c, "probe1.max", "12abc") == CONFIG_BAD_VALUE);
    CHECK(canary_config_set(&c, "probe1.max", "") == CONFIG_OK && isnan(c.probes[0].max));
    CHECK(canary_config_set(&c, "probe1.pin", "64") == CONFIG_BAD_VALUE);
    CHECK(canary_config_set(&c, "probe1.active", "high") == CONFIG_OK && c.probes[0].active_high);

    canary_config_defaults(&c);
    canary_config_set(&c, "probe1.type", "temp");
    canary_config_set(&c, "probe1.url", "https://x/y");
    CHECK(!canary_config_validate(&c, err, sizeof err));
    CHECK_STR(err, "probe1.pin: a GPIO number (or a source file on Linux) is required");
    canary_config_set(&c, "probe1.pin", "4");
    CHECK(!canary_config_validate(&c, err, sizeof err));
    CHECK_STR(err, "probe1: set min, max or both");
    canary_config_set(&c, "probe1.min", "2");
    canary_config_set(&c, "probe1.max", "2.5");
    CHECK(!canary_config_validate(&c, err, sizeof err));
    canary_config_set(&c, "probe1.max", "8");
    CHECK(canary_config_validate(&c, err, sizeof err));
    canary_config_set(&c, "probe1.url", "hc-ping.com/x");
    CHECK(!canary_config_validate(&c, err, sizeof err));
}

static void test_improv_roundtrip(void)
{
    // Frame from the Improv documentation example: Wi-Fi settings for
    // "MyWirelessAP" / "mysecurepassword".
    uint8_t rpc[2 + 1 + 12 + 1 + 16];
    rpc[0] = IMPROV_CMD_WIFI_SETTINGS;
    rpc[1] = 0x1e;
    rpc[2] = 12;
    memcpy(rpc + 3, "MyWirelessAP", 12);
    rpc[15] = 16;
    memcpy(rpc + 16, "mysecurepassword", 16);
    uint8_t frame[IMPROV_MAX_FRAME];
    size_t n = improv_build(frame, sizeof frame, IMPROV_TYPE_RPC, rpc, sizeof rpc);
    CHECK(n == 9 + sizeof rpc + 1);

    improv_parser p;
    improv_parser_init(&p);
    improv_frame f;
    // Noise before the frame, including a false start.
    const char *noise = "I (123) boot: IMPRO\n";
    for (const char *c = noise; *c; c++) {
        CHECK(improv_feed(&p, (uint8_t)*c, &f) == IMPROV_NEED_MORE);
    }
    improv_status s = IMPROV_NEED_MORE;
    for (size_t i = 0; i < n; i++) {
        s = improv_feed(&p, frame[i], &f);
        if (i + 1 < n) {
            CHECK(s == IMPROV_NEED_MORE);
        }
    }
    CHECK(s == IMPROV_FRAME);
    CHECK(f.type == IMPROV_TYPE_RPC && f.len == sizeof rpc);
    improv_rpc r;
    CHECK(improv_parse_rpc(&f, &r));
    CHECK(r.command == IMPROV_CMD_WIFI_SETTINGS);
    CHECK_STR(r.ssid, "MyWirelessAP");
    CHECK_STR(r.password, "mysecurepassword");

    // Corrupted checksum.
    frame[n - 1] ^= 0xff;
    for (size_t i = 0; i < n; i++) {
        s = improv_feed(&p, frame[i], &f);
    }
    CHECK(s == IMPROV_BAD_FRAME);

    // "IIMPROV": the second I starts the header.
    uint8_t state_req[] = {IMPROV_CMD_GET_STATE, 0};
    n = improv_build(frame, sizeof frame, IMPROV_TYPE_RPC, state_req, sizeof state_req);
    CHECK(improv_feed(&p, 'I', &f) == IMPROV_NEED_MORE);
    for (size_t i = 0; i < n; i++) {
        s = improv_feed(&p, frame[i], &f);
    }
    CHECK(s == IMPROV_FRAME);
    CHECK(improv_parse_rpc(&f, &r) && r.command == IMPROV_CMD_GET_STATE);
}

static void test_improv_build(void)
{
    uint8_t out[IMPROV_MAX_FRAME];
    size_t n = improv_build_state(out, sizeof out, IMPROV_STATE_READY);
    const uint8_t want[] = {'I', 'M', 'P', 'R', 'O', 'V', 1, 1, 1, 2, 0};
    uint8_t sum = 0;
    for (size_t i = 0; i < 10; i++) {
        sum = (uint8_t)(sum + want[i]);
    }
    CHECK(n == 11);
    CHECK(memcmp(out, want, 10) == 0 && out[10] == sum);

    const char *info[] = {"canary", "0.1.0", "ESP32-C3", "freezer"};
    n = improv_build_result(out, sizeof out, IMPROV_CMD_GET_INFO, info, 4);
    // data: cmd, len, 4 strings with length prefixes, trailing byte
    size_t strings = 1 + 6 + 1 + 5 + 1 + 8 + 1 + 7;
    CHECK(n == 9 + 2 + strings + 1 + 1);
    CHECK(out[7] == IMPROV_TYPE_RPC_RESULT && out[8] == 2 + strings + 1);
    CHECK(out[9] == IMPROV_CMD_GET_INFO && out[10] == strings);
    CHECK(out[11] == 6 && memcmp(out + 12, "canary", 6) == 0);

    n = improv_build_result(out, sizeof out, IMPROV_CMD_GET_NETWORKS, NULL, 0);
    CHECK(n == 9 + 3 + 1 && out[10] == 0);

    // Malformed RPCs are rejected.
    improv_frame f = {.type = IMPROV_TYPE_RPC, .len = 3, .data = (const uint8_t[]){1, 5, 3}};
    improv_rpc r;
    CHECK(!improv_parse_rpc(&f, &r));
    f = (improv_frame){.type = IMPROV_TYPE_RPC, .len = 4, .data = (const uint8_t[]){1, 2, 0, 0}};
    CHECK(!improv_parse_rpc(&f, &r)); // empty SSID
    f = (improv_frame){.type = IMPROV_TYPE_RPC, .len = 5, .data = (const uint8_t[]){1, 3, 1, 'x', 0}};
    CHECK(improv_parse_rpc(&f, &r)); // open network
    CHECK_STR(r.ssid, "x");
    CHECK_STR(r.password, "");
}

typedef struct {
    int statuses[8];
    int calls;
    uint32_t slept[8];
    int sleeps;
    char last_url[320];
    char last_body[64];
} fake_net;

static int fake_post(void *ctx, const char *url, const char *body, int *retry_after_s)
{
    fake_net *f = ctx;
    snprintf(f->last_url, sizeof f->last_url, "%s", url);
    snprintf(f->last_body, sizeof f->last_body, "%s", body);
    int s = f->statuses[f->calls++];
    *retry_after_s = s == 429 ? 3 : -1;
    return s;
}

static void fake_sleep(void *ctx, uint32_t ms)
{
    fake_net *f = ctx;
    f->slept[f->sleeps++] = ms;
}

static void test_send(void)
{
    fake_net f = {.statuses = {0, 503, 429, 200}};
    ohp_transport t = {fake_post, fake_sleep, &f};
    CHECK(ohp_send(&t, "https://x/a", OHP_FAIL, 0, NULL, "temp_c=3") == 200);
    CHECK(f.calls == 4 && f.sleeps == 3);
    CHECK(f.slept[0] == 1000 && f.slept[1] == 2000 && f.slept[2] == 3000);
    CHECK_STR(f.last_url, "https://x/a/fail");
    CHECK_STR(f.last_body, "temp_c=3");

    fake_net g = {.statuses = {404, 200}};
    t.ctx = &g;
    CHECK(ohp_send(&t, "https://x/a", OHP_SUCCESS, 0, NULL, NULL) == 404);
    CHECK(g.calls == 1 && g.sleeps == 0);

    fake_net h = {.statuses = {0, 0, 0, 0, 0, 200}};
    t.ctx = &h;
    CHECK(ohp_send(&t, "https://x/a", OHP_SUCCESS, 0, NULL, NULL) == 0);
    CHECK(h.calls == OHP_MAX_ATTEMPTS && h.sleeps == OHP_MAX_ATTEMPTS - 1);

    CHECK(ohp_send(&t, "nonsense", OHP_SUCCESS, 0, NULL, NULL) == 400);
}

static void scratchpad(uint8_t sp[9], uint16_t raw, uint8_t conf)
{
    const uint8_t base[9] = {(uint8_t)raw, (uint8_t)(raw >> 8), 0x4b, 0x46, conf, 0xff, 0x0c, 0x10, 0};
    memcpy(sp, base, 9);
    sp[8] = ds18b20_crc8(sp, 8);
}

static void test_ds18b20(void)
{
    // ROM code example from Maxim application note 27.
    const uint8_t rom[8] = {0x02, 0x1c, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xa2};
    CHECK(ds18b20_crc8(rom, 7) == 0xa2);
    CHECK(ds18b20_crc8(rom, 8) == 0);

    uint8_t sp[9];
    float t = 0;
    // Values from the DS18B20 datasheet, table 1.
    scratchpad(sp, 0x0191, 0x7f);
    CHECK(ds18b20_decode(sp, &t) && t == 25.0625f);
    scratchpad(sp, 0xff5e, 0x7f);
    CHECK(ds18b20_decode(sp, &t) && t == -10.125f);
    scratchpad(sp, 0xfc90, 0x7f);
    CHECK(ds18b20_decode(sp, &t) && t == -55.0f);
    scratchpad(sp, 0x07d0, 0x7f);
    CHECK(ds18b20_decode(sp, &t) && t == 125.0f);
    // 9-bit resolution: undefined low bits are ignored.
    scratchpad(sp, 0x0197, 0x1f);
    CHECK(ds18b20_decode(sp, &t) && t == 25.0f);

    scratchpad(sp, 0x0550, 0x7f); // power-on value
    CHECK(!ds18b20_decode(sp, &t));
    scratchpad(sp, 0x0191, 0x7f);
    sp[0] ^= 1; // bit error
    CHECK(!ds18b20_decode(sp, &t));
    memset(sp, 0xff, 9); // no sensor
    CHECK(!ds18b20_decode(sp, &t));
    memset(sp, 0, 9); // CRC of zeros is zero: must still fail
    CHECK(!ds18b20_decode(sp, &t));
}

static void test_form(void)
{
    char s[64] = "a+b%2Fc%3d%E2%80%A6";
    CHECK(canary_url_decode(s) == 0);
    CHECK_STR(s, "a b/c=\xe2\x80\xa6");
    char bad[] = "x%2";
    CHECK(canary_url_decode(bad) == -1);
    char nul[] = "x%00y";
    CHECK(canary_url_decode(nul) == -1);

    canary_config c;
    canary_config_defaults(&c);
    char err[96];
    char body[] = "name=cellar&led=off&led=on&probe1.type=temp&probe1.url=+https%3A%2F%2Fhc-ping.com%2Fabc+"
                  "&probe1.pin=4&probe1.min=&probe1.max=-15&probe2.type=&probe2.url=&save=Save";
    CHECK(canary_form_apply(&c, body, err, sizeof err));
    CHECK_STR(c.name, "cellar");
    CHECK(c.led);
    CHECK_STR(c.probes[0].url, "https://hc-ping.com/abc");
    CHECK(c.probes[0].max == -15.0f && isnan(c.probes[0].min));
    CHECK(c.probes[1].type == PROBE_NONE);

    canary_config_defaults(&c);
    char body2[] = "probe1.type=alive&probe1.url=nope";
    CHECK(!canary_form_apply(&c, body2, err, sizeof err));
    CHECK_STR(err, "probe1.url: an http(s) check URL is required");
    char body3[] = "probe1.interval=abc";
    CHECK(!canary_form_apply(&c, body3, err, sizeof err));
    CHECK_STR(err, "probe1.interval: invalid value");

    char out[32];
    CHECK(canary_html_escape(out, sizeof out, "<a href=\"x\">&'") == 38);
    CHECK_STR(out, "&lt;a href=&quot;x&quot;&gt;");
    char tiny[8];
    canary_html_escape(tiny, sizeof tiny, "ab<cd");
    CHECK_STR(tiny, "ab&lt;c");
    canary_html_escape(tiny, sizeof tiny, "abcd<e");
    CHECK_STR(tiny, "abcd");
}

int main(void)
{
    test_ds18b20();
    test_form();
    test_send();
    test_build_url();
    test_uuid();
    test_retry();
    test_redact();
    test_probe_alive();
    test_probe_temp_hysteresis();
    test_probe_min();
    test_probe_delay();
    test_probe_contact();
    test_config();
    test_improv_roundtrip();
    test_improv_build();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
