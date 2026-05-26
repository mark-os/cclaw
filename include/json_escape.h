#ifndef CCLAW_JSON_ESCAPE_H
#define CCLAW_JSON_ESCAPE_H

#include <stddef.h>

/* V60: Linear-pass JSON string escaper. Writes escaped content into dest (no surrounding quotes).
 * Returns bytes written (excluding NUL). If cap insufficient, returns required size.
 * Handles: " \\ \n \r \t and control chars < 0x20 as \u00XX. Zero-alloc. */
size_t json_escape_into(char *dest, size_t cap, const char *src);

#endif
