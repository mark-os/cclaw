#include "unicode_normalize.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Decode a strictly valid UTF-8 sequence at s[0..len). Returns the sequence
 * length (1–4) and sets *cp, or 0 if the bytes are not well-formed UTF-8
 * (caller passes them through verbatim). */
static size_t decode_utf8_strict(const unsigned char *s, size_t len, unsigned *cp) {
    unsigned char c = s[0];
    unsigned v;
    size_t n;
    if (c < 0x80) { *cp = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { v = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { v = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { v = c & 0x07; n = 4; }
    else return 0;
    if (n > len) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return n;
}

static int is_invisible(unsigned cp) {
    return (cp >= 0x200B && cp <= 0x200F) ||  /* zero-width, LRM/RLM */
           (cp >= 0x202A && cp <= 0x202E) ||  /* bidi overrides */
           cp == 0x2060 ||                    /* word joiner */
           (cp >= 0x2066 && cp <= 0x2069) ||  /* bidi isolates */
           cp == 0xFEFF ||                    /* BOM/ZWNBSP (pos 0 special-cased) */
           cp == 0x00AD ||                    /* soft hyphen */
           (cp >= 0xFE00 && cp <= 0xFE0F);    /* variation selectors */
}

char *unicode_strip_invisible(const char *s, size_t len, size_t *out_len) {
    if (!s) return NULL;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0, pos = 0;
    while (pos < len) {
        unsigned cp;
        size_t n = decode_utf8_strict((const unsigned char *)s + pos, len - pos, &cp);
        if (n == 0) { out[o++] = s[pos++]; continue; }  /* invalid: pass through */
        /* A leading BOM is a legitimate file signature, not smuggling. */
        if (is_invisible(cp) && !(cp == 0xFEFF && pos == 0)) { pos += n; continue; }
        memcpy(out + o, s + pos, n);
        o += n;
        pos += n;
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

char *utf8_sanitize(const char *s, size_t len) {
    if (!s) return NULL;
    /* Worst case: every byte is invalid → each becomes 3-byte U+FFFD */
    if (len > (SIZE_MAX - 1) / 3) return NULL;
    char *out = malloc(len * 3 + 1);
    if (!out) return NULL;
    size_t o = 0, pos = 0;
    while (pos < len) {
        unsigned cp;
        size_t n = decode_utf8_strict((const unsigned char *)s + pos, len - pos, &cp);
        if (n == 0) {
            /* Invalid byte: replace with U+FFFD (ef bf bd) */
            out[o++] = (char)0xEF;
            out[o++] = (char)0xBF;
            out[o++] = (char)0xBD;
            pos++;
        } else {
            /* Also reject surrogates and overlong NULs */
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                out[o++] = (char)0xEF;
                out[o++] = (char)0xBF;
                out[o++] = (char)0xBD;
                pos += n;
            } else {
                memcpy(out + o, s + pos, n);
                o += n;
                pos += n;
            }
        }
    }
    out[o] = '\0';
    /* Shrink to fit */
    char *trimmed = realloc(out, o + 1);
    return trimmed ? trimmed : out;
}
