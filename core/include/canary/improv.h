// Improv Wi-Fi serial protocol (https://www.improv-wifi.com/serial/), used by
// ESP Web Tools to send Wi-Fi credentials right after flashing in the browser.
//
// Frame: "IMPROV" | version 1 | type | length | data[length] | checksum,
// where checksum is the low byte of the sum of all preceding bytes. Like
// ESPHome, the firmware writes a '\n' after every frame.
#ifndef CANARY_IMPROV_H
#define CANARY_IMPROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMPROV_VERSION 1
#define IMPROV_MAX_DATA 255
#define IMPROV_MAX_FRAME (6 + 3 + IMPROV_MAX_DATA + 1)

enum {
    IMPROV_TYPE_CURRENT_STATE = 0x01,
    IMPROV_TYPE_ERROR_STATE = 0x02,
    IMPROV_TYPE_RPC = 0x03,
    IMPROV_TYPE_RPC_RESULT = 0x04,
};

enum {
    IMPROV_STATE_READY = 0x02,
    IMPROV_STATE_PROVISIONING = 0x03,
    IMPROV_STATE_PROVISIONED = 0x04,
};

enum {
    IMPROV_ERROR_NONE = 0x00,
    IMPROV_ERROR_INVALID_RPC = 0x01,
    IMPROV_ERROR_UNKNOWN_RPC = 0x02,
    IMPROV_ERROR_UNABLE_TO_CONNECT = 0x03,
    IMPROV_ERROR_UNKNOWN = 0xff,
};

enum {
    IMPROV_CMD_WIFI_SETTINGS = 0x01,
    IMPROV_CMD_GET_STATE = 0x02,
    IMPROV_CMD_GET_INFO = 0x03,
    IMPROV_CMD_GET_NETWORKS = 0x04,
};

// Incremental frame parser: feed it every byte read from the serial port.
// Bytes that are not part of a frame (log lines, typing) are skipped.
typedef struct {
    uint8_t buf[IMPROV_MAX_FRAME];
    size_t len;
} improv_parser;

typedef struct {
    uint8_t type;
    uint8_t len;
    const uint8_t *data; // points into the parser buffer
} improv_frame;

typedef enum {
    IMPROV_NEED_MORE,
    IMPROV_FRAME,     // *frame is valid until the next call
    IMPROV_BAD_FRAME, // checksum or version error
} improv_status;

void improv_parser_init(improv_parser *p);
improv_status improv_feed(improv_parser *p, uint8_t byte, improv_frame *frame);

// A decoded RPC command.
typedef struct {
    uint8_t command;
    char ssid[33];
    char password[65];
} improv_rpc;

// Decodes the data of an IMPROV_TYPE_RPC frame. Returns false when the
// layout is invalid (lengths do not add up, SSID or password too long).
bool improv_parse_rpc(const improv_frame *f, improv_rpc *rpc);

// Builders: write a complete frame to out and return its length, or 0 when
// it does not fit.
size_t improv_build(uint8_t *out, size_t n, uint8_t type, const uint8_t *data, size_t len);
size_t improv_build_state(uint8_t *out, size_t n, uint8_t state);
size_t improv_build_error(uint8_t *out, size_t n, uint8_t error);
// RPC result with a list of strings (count may be 0).
size_t improv_build_result(uint8_t *out, size_t n, uint8_t command, const char *const *strings, size_t count);

#ifdef __cplusplus
}
#endif

#endif
