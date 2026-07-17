#ifndef CCLAW_JSON_ESCAPE_H
#define CCLAW_JSON_ESCAPE_H

/* JSON string escape/unescape helpers (\uXXXX + surrogate pairs). The
 * db-less string codec for the corners SQLite JSON1 doesn't cover.
 */

#include <stddef.h>

/* Unescape a JSON string (handles \n, \t, \\, \", \/, \uXXXX with surrogate pairs).
 * Writes to dest, returns bytes written (no NUL appended). */
size_t json_unescape(char *dest, size_t cap, const char *src, size_t src_len);

/* Escape a string for embedding in a JSON string literal.
 * Writes to dest (NUL-terminated), returns bytes written excluding NUL. */
size_t json_escape(char *dest, size_t cap, const char *src, size_t src_len);

#endif
