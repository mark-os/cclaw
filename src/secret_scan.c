#include "secret_scan.h"
#include "secret_scan_ac.h"
#include "secret_scan_rules.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

_Static_assert(sizeof(scan_ac_col) == 128, "scan_ac_col must cover ASCII");
_Static_assert(sizeof(scan_ac_goto) / sizeof(scan_ac_goto[0]) == SCAN_AC_STATES,
               "goto rows must match state count");
_Static_assert(sizeof(scan_ac_accept) / sizeof(scan_ac_accept[0]) == SCAN_AC_STATES,
               "accept rows must match state count");

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
    case SCAN_CHARSET_HEX:
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    case SCAN_CHARSET_ALNUM:
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+' ||
               c == '/' || c == '=' || c == '.';
    default: /* ANY */
        return c > 32 && c < 127;
    }
}

/* Word char per regex \b semantics: [A-Za-z0-9_]. */
static int is_word_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Reject a prefix/literal match glued to the end of another token, mirroring
 * gitleaks' leading \b. Only enforced when the anchor itself starts with a
 * word char — anchors like "-----begin" have no meaningful word boundary. */
static int boundary_ok(const char *text, int offset, const ScanRule *rule) {
    if (offset <= 0) return 1;
    if (!is_word_char((unsigned char)rule->keyword[0])) return 1;
    return !is_word_char((unsigned char)text[offset - 1]);
}

/* Validate a prefix-type match: check that tail_min..tail_max chars follow
 * the keyword at text[offset + kw_len] and match the charset. */
static int validate_prefix(const char *text, size_t text_len, int offset,
                           const ScanRule *rule) {
    int kw_len = (int)strlen(rule->keyword);
    int start = offset + kw_len;

    if (!boundary_ok(text, offset, rule)) return 0;
    /* Case-folded AC match is a prefilter; case-sensitive rules re-verify the
     * exact anchor bytes (e.g. twilio "SK", not every "sk"/"Sk"). */
    if (!rule->nocase && strncmp(text + offset, rule->keyword, (size_t)kw_len) != 0)
        return 0;
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
        if (c == '\n' || c == '\r') break;
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

/* Base64 symbol value, accepting both standard (+/) and url (-_) alphabets.
 * Returns -1 for non-base64 bytes (incl. '=' padding). */
static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Validate an HTTP Basic value: the run after the anchor must base64-decode to
 * printable ASCII containing ':' (the user:pass shape). This catches weak,
 * low-entropy credentials an entropy gate misses, while rejecting plain words
 * like "instructions" (which decode to non-printable bytes with no colon).
 * Zero heap — decodes a bounded prefix into a stack buffer, enough to confirm
 * the shape. */
static int validate_base64(const char *text, size_t text_len, int offset,
                           const ScanRule *rule, int *out_start, int *out_len) {
    int kw_len = (int)strlen(rule->keyword);
    if (!boundary_ok(text, offset, rule)) return 0;
    int start = offset + kw_len;
    if (start >= (int)text_len) return 0;

    /* Span of the base64 run (data + '=' padding), bounded by tail_max. */
    int run = 0;
    for (int i = start; i < (int)text_len && run < rule->tail_max; i++) {
        unsigned char c = (unsigned char)text[i];
        if (b64val(c) < 0 && c != '=') break;
        run++;
    }
    if (run < rule->tail_min) return 0;

    /* Decode a bounded prefix; reject on any non-printable byte, require ':'. */
    unsigned char dec[96];
    int dn = 0, bits = 0, has_colon = 0;
    unsigned int acc = 0;
    for (int i = start; i < start + run && dn < (int)sizeof(dec); i++) {
        int v = b64val((unsigned char)text[i]);
        if (v < 0) break;                 /* '=' padding ends the data */
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            unsigned char o = (unsigned char)((acc >> bits) & 0xFF);
            if (o < 0x20 || o > 0x7E) return 0;   /* not printable → not Basic */
            if (o == ':') has_colon = 1;
            dec[dn++] = o;
        }
    }
    if (dn < 2 || !has_colon) return 0;

    *out_start = start;
    *out_len = run;
    return 1;
}

int secret_scan(const char *text, size_t len, ScanFinding *out, int max_findings) {
    int count = 0;
    int state = 0;
    int saturated = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)text[i];
        if (b >= 128) { state = 0; continue; } /* non-ASCII resets */
        if (b >= 'A' && b <= 'Z') b |= 0x20;  /* case-fold at scan time */
        state = scan_ac_goto[state][scan_ac_col[b]];

        /* Check accepts at this state */
        int n_accept = scan_ac_accept[state][0];
        for (int a = 0; a < n_accept; a++) {
            if (saturated) break;
            int rule_idx = scan_ac_accept[state][1 + a];
            if (rule_idx < 0 || rule_idx >= SCAN_RULE_COUNT) continue;
            const ScanRule *rule = &scan_rules[rule_idx];
            int kw_len = (int)strlen(rule->keyword);
            int match_offset = (int)i - kw_len + 1;
            if (match_offset < 0) continue;

            if (rule->vtype == SCAN_VTYPE_LITERAL) {
                if (boundary_ok(text, match_offset, rule) &&
                    (rule->nocase ||
                     strncmp(text + match_offset, rule->keyword, (size_t)kw_len) == 0)) {
                    if (count < max_findings) {
                        out[count].rule_id = rule->id;
                        out[count].offset = match_offset;
                        out[count].match_len = kw_len;
                        count++;
                    } else { saturated = 1; }
                }
            } else if (rule->vtype == SCAN_VTYPE_PREFIX) {
                if (validate_prefix(text, len, match_offset, rule)) {
                    if (count < max_findings) {
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
                    } else { saturated = 1; }
                }
            } else if (rule->vtype == SCAN_VTYPE_BASE64) {
                int val_start, val_len;
                if (validate_base64(text, len, match_offset, rule, &val_start, &val_len)) {
                    if (count < max_findings) {
                        out[count].rule_id = rule->id;
                        out[count].offset = val_start;
                        out[count].match_len = val_len;
                        count++;
                    } else { saturated = 1; }
                }
            } else { /* SCAN_VTYPE_KEYWORD */
                int val_start, val_len;
                if (validate_keyword(text, len, match_offset, rule, &val_start, &val_len)) {
                    if (count < max_findings) {
                        out[count].rule_id = rule->id;
                        out[count].offset = val_start;
                        out[count].match_len = val_len;
                        count++;
                    } else { saturated = 1; }
                }
            }
        }
    }
    return saturated ? max_findings + 1 : count;
}

/* In-place replacement helper (same as tool_shell.c mask_replace) */
static int scan_replace(char *buf, size_t *len, size_t cap,
                        int offset, int old_len,
                        const char *tag, int tag_len) {
    size_t tail = *len - (size_t)(offset + old_len);
    if ((size_t)offset + (size_t)tag_len + tail < cap) {
        memmove(buf + offset + tag_len, buf + offset + old_len, tail + 1);
        memcpy(buf + offset, tag, (size_t)tag_len);
        *len = *len - (size_t)old_len + (size_t)tag_len;
        return 0;
    }
    /* Tag + tail won't fit. The secret must still be removed: write as much
     * of the tag as fits, then keep as much tail as remains — only the bytes
     * that genuinely don't fit are dropped. */
    int fit = (int)(cap - 1) - offset;
    if (fit <= 0) {
        *len = (size_t)offset;
        buf[*len] = '\0';
        return -1;
    }
    int tcopy = fit < tag_len ? fit : tag_len;
    size_t keep = (size_t)(fit - tcopy);
    if (keep > tail) keep = tail;
    memmove(buf + offset + tcopy, buf + offset + old_len, keep);
    memcpy(buf + offset, tag, (size_t)tcopy);
    *len = (size_t)offset + (size_t)tcopy + keep;
    buf[*len] = '\0';
    return -1;
}

static int cmp_findings(const void *a, const void *b) {
    const ScanFinding *fa = a, *fb = b;
    if (fa->offset != fb->offset) return fa->offset - fb->offset;
    return fb->match_len - fa->match_len; /* longer first at same offset */
}

int secret_scan_redact(char *text, size_t *len, size_t cap) {
    ScanFinding findings[SCAN_MAX_FINDINGS];
    int n = secret_scan(text, *len, findings, SCAN_MAX_FINDINGS);
    if (n == 0) return 0;
    if (n > SCAN_MAX_FINDINGS) {
        static const char tag[] = "[SECRET_DETECTED:content_redacted]";
        size_t tag_len = sizeof(tag) - 1;
        if (tag_len < cap) {
            memcpy(text, tag, tag_len);
            text[tag_len] = '\0';
            *len = tag_len;
        } else {
            text[0] = '\0';
            *len = 0;
        }
        return 1;
    }

    qsort(findings, (size_t)n, sizeof(ScanFinding), cmp_findings);

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

    /* Apply replacements back-to-front to preserve offsets. A truncated
     * replacement must not abort the loop: the remaining findings sit to the
     * left of the truncation point and still need redacting. */
    int truncated = 0;
    for (int i = nkeep - 1; i >= 0; i--) {
        ScanFinding *f = &findings[keep[i]];
        char tag[80];
        int tag_len = snprintf(tag, sizeof(tag), "[SECRET_DETECTED:%s]", f->rule_id);
        if (tag_len >= (int)sizeof(tag)) tag_len = (int)sizeof(tag) - 1;
        if (scan_replace(text, len, cap, f->offset, f->match_len, tag, tag_len) < 0)
            truncated = 1;
    }
    text[*len] = '\0';
    return truncated ? -1 : nkeep;
}
