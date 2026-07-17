#ifndef CCLAW_SECRET_SCAN_H
#define CCLAW_SECRET_SCAN_H

/* AC-based secret/DLP scanner: locates leaked credentials in tool output
 * and user messages before they reach the context window, using the
 * generated Aho-Corasick tables. Owns ScanFinding and the redactor.
 */

#include <stddef.h>

#define SCAN_MAX_FINDINGS 32

typedef struct {
    const char *rule_id;  /* points into static scan_rules[] */
    int offset;           /* byte offset in scanned text */
    int match_len;        /* length of matched secret region */
} ScanFinding;

/* Scan text for leaked secrets using AC automaton.
 * Writes up to max_findings results to out[]. Returns count written,
 * or max_findings+1 if the input is saturated (more matches exist
 * than can be recorded — caller should treat entire content as tainted).
 * Precondition: text must contain complete logical units; SSE-chunked
 * streams must be reassembled before scanning. Zero heap allocation. */
int secret_scan(const char *text, size_t len, ScanFinding *out, int max_findings);

/* Scan and redact in-place. Replaces findings with [SECRET_DETECTED:<rule_id>].
 * Updates *len to new length. cap = buffer capacity.
 * Every finding is always removed, even when cap is too small for the tag to
 * expand into — trailing bytes that no longer fit are dropped in that case.
 * Returns number of redactions applied, or -1 if any bytes were dropped. */
int secret_scan_redact(char *text, size_t *len, size_t cap);

/* Shannon entropy of a byte string. Returns bits per byte (0.0–8.0). */
float secret_scan_entropy(const char *s, int len);

#endif /* CCLAW_SECRET_SCAN_H */
