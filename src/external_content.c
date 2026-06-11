#define _POSIX_C_SOURCE 200809L
#include "external_content.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#define MARKER_NAME "UNTRUSTED_EXTERNAL_CONTENT"
#define END_MARKER_NAME "END_UNTRUSTED_EXTERNAL_CONTENT"
#define MARKER_SANITIZED "[[MARKER_SANITIZED]]"
#define END_MARKER_SANITIZED "[[END_MARKER_SANITIZED]]"
#define BOUNDARY_ID_BYTES 8

/* ── Growable buffer ─────────────────────────────────────────────── */

typedef struct { char *data; size_t len, cap; } Buf;

static void buf_append(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *tmp = realloc(b->data, nc);
        if (!tmp) return;
        b->data = tmp; b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* ── Random hex boundary ─────────────────────────────────────────── */

static void generate_boundary_id(char out[BOUNDARY_ID_BYTES * 2 + 1]) {
    unsigned char bytes[BOUNDARY_ID_BYTES];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { read(fd, bytes, sizeof(bytes)); close(fd); }
    else memset(bytes, 0x42, sizeof(bytes)); /* fallback */
    for (int i = 0; i < BOUNDARY_ID_BYTES; i++)
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

/* ── Unicode homoglyph detection ─────────────────────────────────── */

/* Decode a UTF-8 codepoint at pos. Returns codepoint and advances *pos. */
static unsigned decode_utf8(const char *s, size_t len, size_t *pos) {
    unsigned char c = (unsigned char)s[*pos];
    if (c < 0x80) { (*pos)++; return c; }
    unsigned cp = 0; int extra = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { (*pos)++; return c; }
    if (*pos + 1 + extra > len) { (*pos)++; return c; }
    for (int i = 0; i < extra; i++) {
        unsigned char b = (unsigned char)s[*pos + 1 + i];
        if ((b & 0xC0) != 0x80) { (*pos)++; return c; }
        cp = (cp << 6) | (b & 0x3F);
    }
    *pos += 1 + extra;
    return cp;
}

/* Fold codepoint to ASCII equivalent if it's a homoglyph. Returns 0 if not. */
static char fold_homoglyph(unsigned cp) {
    /* Angle bracket homoglyphs → '<' */
    switch (cp) {
    case 0xFF1C: case 0x2329: case 0x3008: case 0x2039:
    case 0x27E8: case 0xFE64: case 0x00AB: case 0x300A:
        return '<';
    case 0xFF1E: case 0x232A: case 0x3009: case 0x203A:
    case 0x27E9: case 0xFE65: case 0x00BB: case 0x300B:
        return '>';
    }
    /* Fullwidth ASCII letters */
    if (cp >= 0xFF21 && cp <= 0xFF3A) return (char)(cp - 0xFEE0);
    if (cp >= 0xFF41 && cp <= 0xFF5A) return (char)(cp - 0xFEE0);
    return 0;
}

/* ── Marker matching with homoglyph folding ──────────────────────── */

/* Try to match literal (case-insensitive) at pos with homoglyph folding.
 * Returns new pos on success, 0 on failure. */
static size_t match_normalized(const char *s, size_t len, size_t pos, const char *lit) {
    while (*lit) {
        if (pos >= len) return 0;
        size_t next = pos;
        unsigned cp = decode_utf8(s, len, &next);
        char folded = fold_homoglyph(cp);
        char actual = folded ? folded : (cp < 128 ? (char)cp : 0);
        if (!actual) return 0;
        char a = actual >= 'A' && actual <= 'Z' ? actual + 32 : actual;
        char e = *lit >= 'A' && *lit <= 'Z' ? *lit + 32 : *lit;
        if (a != e) return 0;
        pos = next;
        lit++;
    }
    return pos;
}

/* Match "<<<" with homoglyph folding */
static size_t match_triple_open(const char *s, size_t len, size_t pos) {
    return match_normalized(s, len, pos, "<<<");
}

/* Match ">>>" with homoglyph folding */
static size_t match_triple_close(const char *s, size_t len, size_t pos) {
    for (size_t scan = pos; scan < len; ) {
        size_t end = match_normalized(s, len, scan, ">>>");
        if (end) return end;
        size_t next = scan;
        decode_utf8(s, len, &next);
        scan = next;
    }
    return 0;
}

char *sanitize_markers(const char *content, size_t len) {
    Buf out = {0};
    size_t pos = 0;
    while (pos < len) {
        size_t after_open = match_triple_open(content, len, pos);
        if (after_open) {
            /* Try END_MARKER_NAME first (longer) */
            size_t after_name = match_normalized(content, len, after_open, END_MARKER_NAME);
            if (after_name) {
                size_t end = match_triple_close(content, len, after_name);
                if (end) {
                    buf_append(&out, END_MARKER_SANITIZED, strlen(END_MARKER_SANITIZED));
                    pos = end;
                    continue;
                }
            }
            after_name = match_normalized(content, len, after_open, MARKER_NAME);
            if (after_name) {
                size_t end = match_triple_close(content, len, after_name);
                if (end) {
                    buf_append(&out, MARKER_SANITIZED, strlen(MARKER_SANITIZED));
                    pos = end;
                    continue;
                }
            }
        }
        buf_append(&out, content + pos, 1);
        pos++;
    }
    if (!out.data) return strdup("");
    return out.data;
}

char *wrap_external_content(const char *content, size_t len, const char *source) {
    char *sanitized = sanitize_markers(content, len);
    if (!sanitized) return NULL;

    char boundary[BOUNDARY_ID_BYTES * 2 + 1];
    generate_boundary_id(boundary);

    size_t san_len = strlen(sanitized);
    size_t src_len = strlen(source);
    /* <<<MARKER id="HEX">>>\nSource: SRC\n---\nCONTENT\n<<<END_MARKER id="HEX">>> */
    size_t total = 3 + strlen(MARKER_NAME) + 5 + 16 + 4 +
                   8 + src_len + 5 + san_len + 1 +
                   3 + strlen(END_MARKER_NAME) + 5 + 16 + 4;
    char *result = malloc(total + 1);
    if (!result) { free(sanitized); return NULL; }

    int wrote = snprintf(result, total + 1,
        "<<<" MARKER_NAME " id=\"%s\">>>\nSource: %s\n---\n%s\n<<<" END_MARKER_NAME " id=\"%s\">>>",
        boundary, source, sanitized, boundary);
    result[wrote] = '\0';

    free(sanitized);
    return result;
}
