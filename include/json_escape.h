#ifndef CCLAW_JSON_ESCAPE_H
#define CCLAW_JSON_ESCAPE_H

#include <stddef.h>

/* V60: Linear-pass JSON string escaper. Writes escaped content into dest (no surrounding quotes).
 * Returns bytes written (excluding NUL). If cap insufficient, returns required size.
 * Handles: " \\ \n \r \t and control chars < 0x20 as \u00XX. Zero-alloc. */
size_t json_escape_into(char *dest, size_t cap, const char *src);

/* T168: Same as json_escape_into but reads exactly src_len bytes (no NUL terminator needed). */
size_t json_escape_into_n(char *dest, size_t cap, const char *src, size_t src_len);

#endif
