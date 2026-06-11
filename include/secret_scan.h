#ifndef CCLAW_SECRET_SCAN_H
#define CCLAW_SECRET_SCAN_H

#include <stddef.h>

#define SCAN_MAX_FINDINGS 32

typedef struct {
    const char *rule_id;  /* points into static scan_rules[] */
    int offset;           /* byte offset in scanned text */
    int match_len;        /* length of matched secret region */
} ScanFinding;

/* Scan text for leaked secrets using AC automaton.
 * Writes findings to caller-provided array. Returns count found.
 * Zero heap allocation — safe for hot path. */
int secret_scan(const char *text, size_t len, ScanFinding *out, int max_findings);

/* Scan and redact in-place. Replaces findings with [SECRET_DETECTED:<rule_id>].
 * Updates *len to new length. cap = buffer capacity.
 * Returns number of redactions applied. */
int secret_scan_redact(char *text, size_t *len, size_t cap);

/* Shannon entropy of a byte string. Returns bits per byte (0.0–8.0). */
float secret_scan_entropy(const char *s, int len);

#endif /* CCLAW_SECRET_SCAN_H */
