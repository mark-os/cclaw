#ifndef CCLAW_EXTERNAL_CONTENT_H
#define CCLAW_EXTERNAL_CONTENT_H

#include <stddef.h>

/* Wrap untrusted external content with randomized anti-spoofing boundaries.
 * Sanitizes any spoofed markers (ASCII + Unicode homoglyphs) in content.
 * Returns malloc'd string. Caller frees. */
char *wrap_external_content(const char *content, size_t len, const char *source);

/* Sanitize spoofed boundary markers in-place (replaces with [[MARKER_SANITIZED]]).
 * Handles ASCII and common Unicode homoglyphs. Returns new malloc'd string. */
char *sanitize_markers(const char *content, size_t len);

#endif
