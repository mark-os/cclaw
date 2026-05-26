#include "json_escape.h"
#include <stdio.h>

/* Shared escape logic — processes src_len bytes from src */
static size_t escape_core(char *dest, size_t cap, const char *src, size_t src_len) {
    size_t w = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = NULL;
        char ubuf[7];
        size_t elen = 0;
        switch (c) {
        case '"':  esc = "\\\""; elen = 2; break;
        case '\\': esc = "\\\\"; elen = 2; break;
        case '\n': esc = "\\n";  elen = 2; break;
        case '\r': esc = "\\r";  elen = 2; break;
        case '\t': esc = "\\t";  elen = 2; break;
        default:
            if (c < 0x20) {
                snprintf(ubuf, sizeof(ubuf), "\\u%04x", c);
                esc = ubuf; elen = 6;
            } else {
                if (w < cap) dest[w] = (char)c;
                w++;
                continue;
            }
        }
        for (size_t j = 0; j < elen; j++) {
            if (w < cap) dest[w] = esc[j];
            w++;
        }
    }
    if (w < cap) dest[w] = '\0';
    else if (cap > 0) dest[cap - 1] = '\0';
    return w;
}

size_t json_escape_into(char *dest, size_t cap, const char *src) {
    if (!src) { if (cap > 0 && dest) dest[0] = '\0'; return 0; }
    size_t len = 0;
    while (src[len]) len++;
    return escape_core(dest, cap, src, len);
}

size_t json_escape_into_n(char *dest, size_t cap, const char *src, size_t src_len) {
    if (!src || src_len == 0) { if (cap > 0 && dest) dest[0] = '\0'; return 0; }
    return escape_core(dest, cap, src, src_len);
}
