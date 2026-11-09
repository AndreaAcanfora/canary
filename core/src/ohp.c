#include "canary/ohp.h"

#include <stdio.h>
#include <string.h>

int ohp_build_url(char *out, size_t n, const char *check, ohp_signal sig, int code, const char *rid)
{
    if (!out || n == 0 || !check) {
        return -1;
    }
    out[0] = '\0';
    if (strncmp(check, "http://", 7) != 0 && strncmp(check, "https://", 8) != 0) {
        return -1;
    }
    const char *query = strchr(check, '?');
    size_t path_len = query ? (size_t)(query - check) : strlen(check);
    while (path_len > 0 && check[path_len - 1] == '/') {
        path_len--;
    }

    char suffix[8] = "";
    switch (sig) {
    case OHP_SUCCESS:
        break;
    case OHP_START:
        strcpy(suffix, "/start");
        break;
    case OHP_FAIL:
        strcpy(suffix, "/fail");
        break;
    case OHP_LOG:
        strcpy(suffix, "/log");
        break;
    case OHP_EXIT:
        if (code < 0 || code > 255) {
            return -1;
        }
        snprintf(suffix, sizeof suffix, "/%d", code);
        break;
    default:
        return -1;
    }

    int w;
    if (query && rid) {
        w = snprintf(out, n, "%.*s%s%s&rid=%s", (int)path_len, check, suffix, query, rid);
    } else if (query) {
        w = snprintf(out, n, "%.*s%s%s", (int)path_len, check, suffix, query);
    } else if (rid) {
        w = snprintf(out, n, "%.*s%s?rid=%s", (int)path_len, check, suffix, rid);
    } else {
        w = snprintf(out, n, "%.*s%s", (int)path_len, check, suffix);
    }
    if (w < 0 || (size_t)w >= n) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

void ohp_uuid4(char out[37], const uint8_t random[16])
{
    uint8_t b[16];
    memcpy(b, random, sizeof b);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    static const char hex[] = "0123456789abcdef";
    char *p = out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            *p++ = '-';
        }
        *p++ = hex[b[i] >> 4];
        *p++ = hex[b[i] & 0x0f];
    }
    *p = '\0';
}

ohp_outcome ohp_classify(int http_status)
{
    if (http_status >= 200 && http_status < 300) {
        return OHP_DONE;
    }
    if (http_status <= 0 || http_status == 429 || http_status >= 500) {
        return OHP_RETRY;
    }
    return OHP_GIVE_UP;
}

uint32_t ohp_retry_delay_ms(int failed, int http_status, int retry_after_s)
{
    if (http_status == 429 && retry_after_s >= 0 && retry_after_s <= 60) {
        return (uint32_t)retry_after_s * 1000u;
    }
    if (failed < 1) {
        failed = 1;
    }
    if (failed > 8) {
        failed = 8;
    }
    return 1000u << (failed - 1);
}

void ohp_redact(char *out, size_t n, const char *url)
{
    if (n == 0) {
        return;
    }
    const char *host = strstr(url, "://");
    if (!host) {
        snprintf(out, n, "(invalid URL)");
        return;
    }
    host += 3;
    size_t host_len = strcspn(host, "/?#");
    snprintf(out, n, "%.*s/\xe2\x80\xa6", (int)(host - url + host_len), url);
}

int ohp_send(const ohp_transport *t, const char *check, ohp_signal sig, int code, const char *rid,
             const char *body)
{
    char url[320];
    if (ohp_build_url(url, sizeof url, check, sig, code, rid) != 0) {
        return 400;
    }
    int status = 0;
    for (int attempt = 1; attempt <= OHP_MAX_ATTEMPTS; attempt++) {
        int retry_after = -1;
        status = t->post(t->ctx, url, body ? body : "", &retry_after);
        if (ohp_classify(status) != OHP_RETRY || attempt == OHP_MAX_ATTEMPTS) {
            break;
        }
        t->sleep_ms(t->ctx, ohp_retry_delay_ms(attempt, status, retry_after));
    }
    return status;
}
