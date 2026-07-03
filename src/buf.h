#ifndef CCLAW_BUF_H
#define CCLAW_BUF_H

#include <stddef.h>

/* Growable byte buffer. Zero-init (`Buf b = {0};`) is a valid empty buffer.
 * OOM sets a sticky flag; further appends become no-ops so callers only
 * need to check once, via buf_take(), instead of after every append. */
typedef struct {
    char *data;
    size_t len, cap;
    int oom;
} Buf;

void buf_append(Buf *b, const char *s, size_t n);
void buf_append_str(Buf *b, const char *s);
void buf_append_char(Buf *b, char c);
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void buf_appendf(Buf *b, const char *fmt, ...);

/* Returns the NUL-terminated data on success (b is zeroed, caller frees the
 * return value), or NULL if any append hit OOM (b's storage is freed). */
char *buf_take(Buf *b);
void buf_free(Buf *b);

typedef struct {
    const char *key; /* e.g. "{date}" */
    const char *val;
} TemplateVar;

/* Replace every occurrence of each var's key with its val. Unmatched `{...}`
 * runs pass through unchanged. Returns NULL on OOM. */
char *template_render(const char *tmpl, const TemplateVar *vars, size_t nvars);

#endif
