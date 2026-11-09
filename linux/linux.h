#ifndef CANARY_LINUX_H
#define CANARY_LINUX_H

#include <stdbool.h>
#include <stddef.h>

#include "canary/ohp.h"

#ifndef CANARY_VERSION
#define CANARY_VERSION "0.1.0-dev"
#endif

typedef struct {
    char host[64];
    bool verbose;
} http_options;

int http_init(char *err, size_t err_n);
ohp_transport http_transport(http_options *opt);

#endif
