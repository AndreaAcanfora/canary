#include "canary/form.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int canary_url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%') {
            int hi = hex(r[1]), lo = hi < 0 ? -1 : hex(r[2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                *w = '\0';
                return -1;
            }
            *w++ = (char)(hi << 4 | lo);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
    return 0;
}

bool canary_form_apply(canary_config *cfg, char *body, char *err, size_t err_n)
{
    for (char *pair = body, *next; pair; pair = next) {
        next = strchr(pair, '&');
        if (next) {
            *next++ = '\0';
        }
        if (*pair == '\0') {
            continue;
        }
        char *eq = strchr(pair, '=');
        char *value = eq ? eq + 1 : pair + strlen(pair);
        if (eq) {
            *eq = '\0';
        }
        if (canary_url_decode(pair) != 0 || canary_url_decode(value) != 0) {
            snprintf(err, err_n, "malformed form data");
            return false;
        }
        // Trim spaces pasted around URLs and numbers.
        while (*value == ' ') {
            value++;
        }
        size_t len = strlen(value);
        while (len > 0 && value[len - 1] == ' ') {
            value[--len] = '\0';
        }
        config_result r = canary_config_set(cfg, pair, value);
        if (r == CONFIG_UNKNOWN_KEY) {
            continue; // buttons and other fields that are not settings
        }
        if (r != CONFIG_OK) {
            snprintf(err, err_n, "%.40s: invalid value", pair);
            return false;
        }
    }
    return canary_config_validate(cfg, err, err_n);
}

size_t canary_html_escape(char *out, size_t n, const char *s)
{
    size_t pos = 0, written = 0;
    bool full = n == 0;
    for (; *s; s++) {
        const char *rep;
        char one[2] = {*s, '\0'};
        switch (*s) {
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            rep = "&quot;";
            break;
        case '\'':
            rep = "&#39;";
            break;
        default:
            rep = one;
            break;
        }
        size_t l = strlen(rep);
        // Never write half an entity: stop at the first piece that does not fit.
        if (!full && written + l < n) {
            memcpy(out + written, rep, l);
            written += l;
        } else {
            full = true;
        }
        pos += l;
    }
    if (n) {
        out[written] = '\0';
    }
    return pos;
}
