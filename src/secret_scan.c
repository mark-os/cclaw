#include "secret_scan.h"
#include "secret_scan_ac.h"
#include "secret_scan_rules.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

float secret_scan_entropy(const char *s, int len) {
    if (len <= 0) return 0.0f;
    int freq[256] = {0};
    for (int i = 0; i < len; i++)
        freq[(unsigned char)s[i]]++;
    float ent = 0.0f;
    for (int i = 0; i < 256; i++) {
        if (freq[i] == 0) continue;
        float p = (float)freq[i] / (float)len;
        ent -= p * log2f(p);
    }
    return ent;
}

/* Check if byte matches charset class */
static int charset_ok(unsigned char c, int charset) {
    switch (charset) {
    case SCAN_CHARSET_UPPER_ALNUM:
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    case SCAN_CHARSET_LOWER_ALNUM:
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    case SCAN_CHARSET_ALNUM:
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+' ||
               c == '/' || c == '=' || c == '.';
    default: /* ANY */
        return c > 32 && c < 127;
    }
}

/* Validate a prefix-type match: check that tail_min..tail_max chars follow
 * the keyword at text[offset + kw_len] and match the charset. */
static int validate_prefix(const char *text, size_t text_len, int offset,
                           const ScanRule *rule) {
    int kw_len = (int)strlen(rule->keyword);
    int start = offset + kw_len;

    /* Private key special case: just verify the prefix matched */
    if (rule->tail_min == 0 && rule->tail_max == 100)
        return 1;

    if (start >= (int)text_len) return 0;

    /* Count valid tail characters */
    int tail = 0;
    for (int i = start; i < (int)text_len && tail < rule->tail_max; i++) {
        if (!charset_ok((unsigned char)text[i], rule->charset))
            break;
        tail++;
    }

    if (tail < rule->tail_min) return 0;

    /* Entropy check if specified */
    if (rule->entropy > 0.0f && tail >= 8) {
        float ent = secret_scan_entropy(text + start, tail);
        if (ent < rule->entropy) return 0;
    }
    return 1;
}

/* Validate a keyword-type match: look for assignment pattern nearby,
 * then check entropy of the value. */
static int validate_keyword(const char *text, size_t text_len, int offset,
                            const ScanRule *rule, int *out_start, int *out_len) {
    int kw_len = (int)strlen(rule->keyword);
    /* Search forward from keyword for assignment operator within 30 chars */
    int search_end = offset + kw_len + 30;
    if (search_end > (int)text_len) search_end = (int)text_len;

    int eq_pos = -1;
    for (int i = offset + kw_len; i < search_end; i++) {
        char c = text[i];
        if (c == '=' || c == ':') { eq_pos = i; break; }
    }
    if (eq_pos < 0) return 0;

    /* Skip whitespace and quotes after operator */
    int val_start = eq_pos + 1;
    while (val_start < (int)text_len &&
           (text[val_start] == ' ' || text[val_start] == '\t' ||
            text[val_start] == '"' || text[val_start] == '\'' ||
            text[val_start] == '`'))
        val_start++;

    if (val_start >= (int)text_len) return 0;

    /* Extract value: run of non-whitespace, non-quote chars */
    int val_end = val_start;
    while (val_end < (int)text_len && val_end - val_start < rule->tail_max) {
        char c = text[val_end];
        if (c == '"' || c == '\'' || c == '`' || c == ';' ||
            c == '\n' || c == '\r' || c == ' ' || c == '\t')
            break;
        val_end++;
    }

    int val_len = val_end - val_start;
    if (val_len < rule->tail_min) return 0;

    /* Reject if it's all alpha or all-one-char (common false positives) */
    int all_alpha = 1;
    for (int i = val_start; i < val_end; i++) {
        if (!isalpha((unsigned char)text[i]) && text[i] != '_' && text[i] != '-' && text[i] != '.') {
            all_alpha = 0; break;
        }
    }
    if (all_alpha) return 0;

    /* Entropy check */
    float ent = secret_scan_entropy(text + val_start, val_len);
    if (ent < rule->entropy) return 0;

    *out_start = val_start;
    *out_len = val_len;
    return 1;
}

int secret_scan(const char *text, size_t len, ScanFinding *out, int max_findings) {
    int count = 0;
    int state = 0;

    for (size_t i = 0; i < len && count < max_findings; i++) {
        unsigned char b = (unsigned char)text[i];
        if (b >= 128) { state = 0; continue; } /* non-ASCII resets */
        state = scan_ac_goto[state][b];

        /* Check accepts at this state */
        int n_accept = scan_ac_accept[state][0];
        for (int a = 0; a < n_accept && count < max_findings; a++) {
            int rule_idx = scan_ac_accept[state][1 + a];
            if (rule_idx < 0 || rule_idx >= SCAN_RULE_COUNT) continue;
            const ScanRule *rule = &scan_rules[rule_idx];
            int kw_len = (int)strlen(rule->keyword);
            int match_offset = (int)i - kw_len + 1;
            if (match_offset < 0) continue;

            if (rule->vtype == SCAN_VTYPE_PREFIX) {
                if (validate_prefix(text, len, match_offset, rule)) {
                    int tail = 0;
                    int start = match_offset + kw_len;
                    for (int j = start; j < (int)len && tail < rule->tail_max; j++) {
                        if (!charset_ok((unsigned char)text[j], rule->charset)) break;
                        tail++;
                    }
                    out[count].rule_id = rule->id;
                    out[count].offset = match_offset;
                    out[count].match_len = kw_len + tail;
                    count++;
                }
            } else { /* SCAN_VTYPE_KEYWORD */
                int val_start, val_len;
                if (validate_keyword(text, len, match_offset, rule, &val_start, &val_len)) {
                    out[count].rule_id = rule->id;
                    out[count].offset = val_start;
                    out[count].match_len = val_len;
                    count++;
                }
            }
        }
    }
    return count;
}

/* In-place replacement helper (same as tool_shell.c mask_replace) */
static void scan_replace(char *buf, size_t *len, size_t cap,
                         int offset, int old_len,
                         const char *tag, int tag_len) {
    size_t tail = *len - (size_t)(offset + old_len);
    if ((size_t)offset + (size_t)tag_len + tail < cap) {
        memmove(buf + offset + tag_len, buf + offset + old_len, tail + 1);
        memcpy(buf + offset, tag, (size_t)tag_len);
        *len = *len - (size_t)old_len + (size_t)tag_len;
    } else {
        /* Truncate if replacement would overflow */
        int fit = (int)(cap - 1) - offset;
        if (fit > 0 && fit <= tag_len) {
            memcpy(buf + offset, tag, (size_t)fit);
            *len = (size_t)(offset + fit);
            buf[*len] = '\0';
        }
    }
}

int secret_scan_redact(char *text, size_t *len, size_t cap) {
    ScanFinding findings[SCAN_MAX_FINDINGS];
    int n = secret_scan(text, *len, findings, SCAN_MAX_FINDINGS);
    if (n == 0) return 0;

    /* Sort by offset ascending, then by match_len descending (prefer longer) */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (findings[j].offset < findings[i].offset ||
                (findings[j].offset == findings[i].offset &&
                 findings[j].match_len > findings[i].match_len)) {
                ScanFinding tmp = findings[i];
                findings[i] = findings[j];
                findings[j] = tmp;
            }

    /* Remove overlapping findings (keep first/longest at each position) */
    int keep[SCAN_MAX_FINDINGS];
    int nkeep = 0;
    int covered_end = -1;
    for (int i = 0; i < n; i++) {
        if (findings[i].offset >= covered_end) {
            keep[nkeep++] = i;
            covered_end = findings[i].offset + findings[i].match_len;
        }
    }

    /* Apply replacements back-to-front to preserve offsets */
    for (int i = nkeep - 1; i >= 0; i--) {
        ScanFinding *f = &findings[keep[i]];
        char tag[80];
        int tag_len = snprintf(tag, sizeof(tag), "[SECRET_DETECTED:%s]", f->rule_id);
        if (tag_len >= (int)sizeof(tag)) tag_len = (int)sizeof(tag) - 1;
        scan_replace(text, len, cap, f->offset, f->match_len, tag, tag_len);
    }
    text[*len] = '\0';
    return nkeep;
}
