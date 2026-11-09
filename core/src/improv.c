#include "canary/improv.h"

#include <string.h>

static const uint8_t header[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

void improv_parser_init(improv_parser *p)
{
    p->len = 0;
}

static uint8_t checksum(const uint8_t *b, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    return sum;
}

// After a mismatch, keep the longest suffix of the buffer that can still be
// the start of a header, so "IIMPROV" is not missed.
static void resync(improv_parser *p)
{
    size_t start = 1;
    for (; start < p->len; start++) {
        size_t n = p->len - start;
        if (n > sizeof header) {
            n = sizeof header;
        }
        if (memcmp(p->buf + start, header, n) == 0) {
            break;
        }
    }
    memmove(p->buf, p->buf + start, p->len - start);
    p->len -= start;
}

improv_status improv_feed(improv_parser *p, uint8_t byte, improv_frame *frame)
{
    p->buf[p->len++] = byte;
    while (p->len > 0 && p->len <= sizeof header) {
        if (memcmp(p->buf, header, p->len) == 0) {
            return IMPROV_NEED_MORE;
        }
        resync(p);
    }
    if (p->len == 0 || p->len < 9) {
        return IMPROV_NEED_MORE;
    }
    if (p->buf[6] != IMPROV_VERSION) {
        p->len = 0;
        return IMPROV_BAD_FRAME;
    }
    size_t data_len = p->buf[8];
    size_t total = 9 + data_len + 1;
    if (p->len < total) {
        return IMPROV_NEED_MORE;
    }
    p->len = 0;
    if (checksum(p->buf, total - 1) != p->buf[total - 1]) {
        return IMPROV_BAD_FRAME;
    }
    frame->type = p->buf[7];
    frame->len = (uint8_t)data_len;
    frame->data = p->buf + 9;
    return IMPROV_FRAME;
}

bool improv_parse_rpc(const improv_frame *f, improv_rpc *rpc)
{
    memset(rpc, 0, sizeof *rpc);
    if (f->type != IMPROV_TYPE_RPC || f->len < 2) {
        return false;
    }
    const uint8_t *d = f->data;
    size_t rpc_len = d[1];
    if (2 + rpc_len > f->len) {
        return false;
    }
    rpc->command = d[0];
    if (rpc->command != IMPROV_CMD_WIFI_SETTINGS) {
        return true;
    }
    const uint8_t *q = d + 2, *end = d + 2 + rpc_len;
    if (q >= end) {
        return false;
    }
    size_t ssid_len = *q++;
    if (ssid_len == 0 || ssid_len >= sizeof rpc->ssid || q + ssid_len >= end) {
        return false;
    }
    memcpy(rpc->ssid, q, ssid_len);
    q += ssid_len;
    size_t pass_len = *q++;
    if (pass_len >= sizeof rpc->password || q + pass_len != end) {
        return false;
    }
    memcpy(rpc->password, q, pass_len);
    return true;
}

size_t improv_build(uint8_t *out, size_t n, uint8_t type, const uint8_t *data, size_t len)
{
    size_t total = 9 + len + 1;
    if (len > IMPROV_MAX_DATA || total > n) {
        return 0;
    }
    memcpy(out, header, sizeof header);
    out[6] = IMPROV_VERSION;
    out[7] = type;
    out[8] = (uint8_t)len;
    if (len) {
        memcpy(out + 9, data, len);
    }
    out[total - 1] = checksum(out, total - 1);
    return total;
}

size_t improv_build_state(uint8_t *out, size_t n, uint8_t state)
{
    return improv_build(out, n, IMPROV_TYPE_CURRENT_STATE, &state, 1);
}

size_t improv_build_error(uint8_t *out, size_t n, uint8_t error)
{
    return improv_build(out, n, IMPROV_TYPE_ERROR_STATE, &error, 1);
}

size_t improv_build_result(uint8_t *out, size_t n, uint8_t command, const char *const *strings, size_t count)
{
    uint8_t data[IMPROV_MAX_DATA];
    size_t len = 2;
    for (size_t i = 0; i < count; i++) {
        size_t s = strlen(strings[i]);
        if (s > 255 || len + 1 + s > sizeof data) {
            return 0;
        }
        data[len++] = (uint8_t)s;
        memcpy(data + len, strings[i], s);
        len += s;
    }
    if (len + 1 > sizeof data) {
        return 0;
    }
    data[0] = command;
    data[1] = (uint8_t)(len - 2);
    // Like ESPHome, end the RPC data with an unused checksum byte: the
    // frame checksum already covers it, and clients rely on data[1].
    data[len++] = 0;
    return improv_build(out, n, IMPROV_TYPE_RPC_RESULT, data, len);
}
