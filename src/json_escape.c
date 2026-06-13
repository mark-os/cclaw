#include "json_escape.h"
#include <string.h>

/* Helper to parse a single hex digit. Returns -1 if invalid. */
static inline int parse_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t json_unescape(char *dest, size_t cap, const char *src, size_t src_len) {
    if (cap == 0) return 0;
    size_t w = 0;
    for (size_t i = 0; i < src_len && w < cap; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
            case 'n': dest[w++] = '\n'; break;
            case 'r': dest[w++] = '\r'; break;
            case 't': dest[w++] = '\t'; break;
            case '"': dest[w++] = '"'; break;
            case '\\': dest[w++] = '\\'; break;
            case '/': dest[w++] = '/'; break;
            case 'u':
                if (i + 4 < src_len) {
                    unsigned cp = 0;
                    int valid = 1;
                    for (int j = 1; j <= 4; j++) {
                        int v = parse_hex_digit(src[i + j]);
                        if (v < 0) { valid = 0; break; }
                        cp = (cp << 4) | (unsigned)v;
                    }
                    if (!valid) { dest[w++] = 'u'; break; }
                    i += 4;
                    /* UTF-16 surrogate pairs */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        int consumed_low = 0;
                        if (i + 6 < src_len && src[i + 1] == '\\' && src[i + 2] == 'u') {
                            unsigned lo = 0;
                            int valid_lo = 1;
                            for (int j = 3; j <= 6; j++) {
                                int v = parse_hex_digit(src[i + j]);
                                if (v < 0) { valid_lo = 0; break; }
                                lo = (lo << 4) | (unsigned)v;
                            }
                            if (valid_lo && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                i += 6;
                                consumed_low = 1;
                            }
                        }
                        if (!consumed_low) { cp = 0xFFFD; }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    /* Encode to UTF-8 */
                    if (cp < 0x80) {
                        dest[w++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (w + 1 < cap) {
                            dest[w++] = (char)(0xC0 | (cp >> 6));
                            dest[w++] = (char)(0x80 | (cp & 0x3F));
                        } else { break; }
                    } else if (cp < 0x10000) {
                        if (w + 2 < cap) {
                            dest[w++] = (char)(0xE0 | (cp >> 12));
                            dest[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            dest[w++] = (char)(0x80 | (cp & 0x3F));
                        } else { break; }
                    } else {
                        if (w + 3 < cap) {
                            dest[w++] = (char)(0xF0 | (cp >> 18));
                            dest[w++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            dest[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            dest[w++] = (char)(0x80 | (cp & 0x3F));
                        } else { break; }
                    }
                } else {
                    dest[w++] = 'u';
                }
                break;
            default: dest[w++] = src[i]; break;
            }
        } else {
            dest[w++] = src[i];
        }
    }
    return w;
}

size_t json_escape(char *dest, size_t cap, const char *src, size_t src_len) {
    if (cap == 0) return 0;
    size_t w = 0;
    for (size_t i = 0; i < src_len && w + 6 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"') { dest[w++] = '\\'; dest[w++] = '"'; }
        else if (c == '\\') { dest[w++] = '\\'; dest[w++] = '\\'; }
        else if (c == '\n') { dest[w++] = '\\'; dest[w++] = 'n'; }
        else if (c == '\r') { dest[w++] = '\\'; dest[w++] = 'r'; }
        else if (c == '\t') { dest[w++] = '\\'; dest[w++] = 't'; }
        else if (c < 0x20) {
            dest[w++] = '\\'; dest[w++] = 'u'; dest[w++] = '0'; dest[w++] = '0';
            dest[w++] = "0123456789abcdef"[c >> 4];
            dest[w++] = "0123456789abcdef"[c & 0xf];
        }
        else dest[w++] = (char)c;
    }
    dest[w] = '\0';
    return w;
}
