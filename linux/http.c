// HTTP POST through libcurl, loaded at run time with dlopen so that building
// canary needs no development headers: every Linux distribution, Raspberry Pi
// OS included, ships libcurl.so.4.
#include "linux.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "canary/ohp.h"

// Stable libcurl ABI values (curl/curl.h).
#define CURLOPT_TIMEOUT_MS 155
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_POSTFIELDSIZE 60
#define CURLOPT_POST 47
#define CURLOPT_URL 10002
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_USERAGENT 10018
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_HEADERDATA 10029
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_HEADERFUNCTION 20079
#define CURLINFO_RESPONSE_CODE 0x200002
#define CURL_GLOBAL_DEFAULT 3

typedef void CURL;
struct curl_slist;

static struct {
    void *lib;
    int (*global_init)(long);
    CURL *(*easy_init)(void);
    int (*easy_setopt)(CURL *, int, ...);
    int (*easy_perform)(CURL *);
    int (*easy_getinfo)(CURL *, int, ...);
    void (*easy_cleanup)(CURL *);
    const char *(*easy_strerror)(int);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
} curl;

int http_init(char *err, size_t err_n)
{
    const char *names[] = {"libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4"};
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !curl.lib; i++) {
        curl.lib = dlopen(names[i], RTLD_NOW);
    }
    if (!curl.lib) {
        snprintf(err, err_n, "libcurl not found (install libcurl4 or curl)");
        return -1;
    }
#define LOAD(field, sym)                                         \
    do {                                                         \
        *(void **)(&curl.field) = dlsym(curl.lib, sym);          \
        if (!curl.field) {                                       \
            snprintf(err, err_n, "libcurl: missing %s", sym);    \
            return -1;                                           \
        }                                                        \
    } while (0)
    LOAD(global_init, "curl_global_init");
    LOAD(easy_init, "curl_easy_init");
    LOAD(easy_setopt, "curl_easy_setopt");
    LOAD(easy_perform, "curl_easy_perform");
    LOAD(easy_getinfo, "curl_easy_getinfo");
    LOAD(easy_cleanup, "curl_easy_cleanup");
    LOAD(easy_strerror, "curl_easy_strerror");
    LOAD(slist_append, "curl_slist_append");
    LOAD(slist_free_all, "curl_slist_free_all");
#undef LOAD
    return curl.global_init(CURL_GLOBAL_DEFAULT) == 0 ? 0 : -1;
}

static size_t discard(char *ptr, size_t size, size_t nmemb, void *ud)
{
    (void)ptr;
    (void)ud;
    return size * nmemb;
}

static size_t on_header(char *ptr, size_t size, size_t nmemb, void *ud)
{
    size_t n = size * nmemb;
    int *retry_after = ud;
    if (n > 12 && strncasecmp(ptr, "Retry-After:", 12) == 0) {
        char buf[32];
        size_t len = n - 12 < sizeof buf - 1 ? n - 12 : sizeof buf - 1;
        memcpy(buf, ptr + 12, len);
        buf[len] = '\0';
        char *end;
        long v = strtol(buf, &end, 10);
        if (end != buf && v >= 0 && v < 86400) {
            *retry_after = (int)v;
        }
    }
    return n;
}

static int post(void *ctx, const char *url, const char *body, int *retry_after_s)
{
    const http_options *opt = ctx;
    *retry_after_s = -1;
    CURL *h = curl.easy_init();
    if (!h) {
        return 0;
    }
    char host_header[128];
    snprintf(host_header, sizeof host_header, "OHP-Host: %s", opt->host);
    struct curl_slist *headers = NULL;
    headers = curl.slist_append(headers, "Content-Type: text/plain; charset=utf-8");
    headers = curl.slist_append(headers, "OHP-Version: " OHP_VERSION);
    headers = curl.slist_append(headers, host_header);

    curl.easy_setopt(h, CURLOPT_URL, url);
    curl.easy_setopt(h, CURLOPT_POST, 1L);
    curl.easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl.easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl.easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl.easy_setopt(h, CURLOPT_USERAGENT, "canary/" CANARY_VERSION);
    curl.easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)OHP_TIMEOUT_MS);
    curl.easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl.easy_setopt(h, CURLOPT_WRITEFUNCTION, discard);
    curl.easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl.easy_setopt(h, CURLOPT_HEADERDATA, retry_after_s);

    long status = 0;
    int rc = curl.easy_perform(h);
    if (rc == 0) {
        curl.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    } else if (opt->verbose) {
        char red[96];
        ohp_redact(red, sizeof red, url);
        fprintf(stderr, "canary: %s: %s\n", red, curl.easy_strerror(rc));
    }
    curl.easy_cleanup(h);
    curl.slist_free_all(headers);
    return (int)status;
}

static void sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

ohp_transport http_transport(http_options *opt)
{
    return (ohp_transport){.post = post, .sleep_ms = sleep_ms, .ctx = opt};
}
