// Helpers for the setup page: form decoding and HTML escaping.
#ifndef CANARY_FORM_H
#define CANARY_FORM_H

#include <stddef.h>

#include "canary/config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Decodes application/x-www-form-urlencoded text in place ('+' and %XX).
// Returns -1 on a malformed escape.
int canary_url_decode(char *s);

// Applies a posted form (key=value&...) to cfg with canary_config_set.
// Fields with an empty "type" leave the probe disabled. The body is modified.
// Returns false and describes the first problem in err.
bool canary_form_apply(canary_config *cfg, char *body, char *err, size_t err_n);

// Copies s to out with &, <, >, " and ' escaped. Returns the length needed.
size_t canary_html_escape(char *out, size_t n, const char *s);

#ifdef __cplusplus
}
#endif

#endif
