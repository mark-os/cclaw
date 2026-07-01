#ifndef CCLAW_UNICODE_NORMALIZE_H
#define CCLAW_UNICODE_NORMALIZE_H

#include <stddef.h>

/* Strip invisible Unicode used for prompt-injection smuggling: zero-width
 * chars (U+200B–U+200F), bidi overrides (U+202A–U+202E), word joiner
 * (U+2060), bidi isolates (U+2066–U+2069), BOM (U+FEFF, kept at position 0),
 * soft hyphen (U+00AD), variation selectors (U+FE00–U+FE0F).
 *
 * Invalid UTF-8 bytes pass through untouched (binary-ish content is not
 * corrupted). Returns a malloc'd NUL-terminated copy; *out_len (optional)
 * receives the resulting byte length. NULL on OOM or NULL input. */
char *unicode_strip_invisible(const char *s, size_t len, size_t *out_len);

#endif
