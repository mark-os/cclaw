#ifndef CCLAW_EXTERNAL_CONTENT_H
#define CCLAW_EXTERNAL_CONTENT_H

#include <stddef.h>

/* Neutralize spoofed <<<UNTRUSTED_EXTERNAL_CONTENT>>> boundary markers
 * (replaced with [[MARKER_SANITIZED]]). Handles ASCII and common Unicode
 * homoglyphs. Returns new malloc'd string. Untrusted content is wrapped in
 * real markers at query time (llm_payload) via the entry's network_hosts tag;
 * this sanitizer runs at storage time so the wrap can't be broken out of. */
char *sanitize_markers(const char *content, size_t len);

#endif
