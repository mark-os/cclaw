#include "json_escape.h"
#include <stdio.h>

size_t json_escape_into(char *dest, size_t cap, const char *src) {
    if (!src) { if (cap > 0) dest[0] = '\0'; return 0; }
    size_t w = 0;
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
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
        for (size_t i = 0; i < elen; i++) {
            if (w < cap) dest[w] = esc[i];
            w++;
        }
    }
    if (w < cap) dest[w] = '\0';
    else if (cap > 0) dest[cap - 1] = '\0';
    return w;
}
