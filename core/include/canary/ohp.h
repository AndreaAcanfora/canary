// Open Heartbeat Protocol (OHP 0.2) client helpers.
// Portable C11, no allocation: shared by the ESP-IDF firmware and the Linux port.
#ifndef CANARY_OHP_H
#define CANARY_OHP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OHP_VERSION "0.2"
#define OHP_MAX_ATTEMPTS 5
#define OHP_TIMEOUT_MS 10000

typedef enum {
    OHP_SUCCESS,
    OHP_START,
    OHP_FAIL,
    OHP_LOG,
    OHP_EXIT, // exit code signal, code in 0..255
} ohp_signal;

// Builds the URL of a signal: the check URL with its trailing '/' removed,
// the signal suffix appended to the path and, when rid is not NULL, ?rid=.
// An existing query string in the check URL is kept after the suffix.
// Returns 0, or -1 when the URL does not fit or the arguments are invalid.
int ohp_build_url(char *out, size_t n, const char *check, ohp_signal sig, int code, const char *rid);

// Formats 16 random bytes as a version 4 UUID (36 characters + NUL).
void ohp_uuid4(char out[37], const uint8_t random[16]);

typedef enum {
    OHP_DONE,    // recorded (2xx)
    OHP_RETRY,   // network error (status 0), 429 or 5xx
    OHP_GIVE_UP, // any other status: retrying cannot help
} ohp_outcome;

ohp_outcome ohp_classify(int http_status);

// Delay before the next attempt after `failed` failed attempts (1-based):
// 1 s, 2 s, 4 s, 8 s. A Retry-After of at most 60 s (retry_after_s >= 0)
// takes precedence for 429 responses.
uint32_t ohp_retry_delay_ms(int failed, int http_status, int retry_after_s);

// Copies the URL with its secret part hidden: "https://hc-ping.com/…".
void ohp_redact(char *out, size_t n, const char *url);

// Transport used by ohp_send. post() sends one POST request with the body
// as text/plain and the OHP-Version header, and returns the HTTP status, or
// 0 on a network error. It stores the Retry-After header in seconds in
// *retry_after_s, or -1 when absent.
typedef struct {
    int (*post)(void *ctx, const char *url, const char *body, int *retry_after_s);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    void *ctx;
} ohp_transport;

// Sends a signal, retrying as section 7.1 of the specification requires.
// Returns the last HTTP status (0 when the server was never reached).
int ohp_send(const ohp_transport *t, const char *check, ohp_signal sig, int code, const char *rid,
             const char *body);

#ifdef __cplusplus
}
#endif

#endif
