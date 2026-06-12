#define _POSIX_C_SOURCE 200809L
#include "secret_interp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PREFIX "{{SECRET:"
#define PREFIX_LEN 9
#define SUFFIX "}}"
#define SUFFIX_LEN 2

#include "secret_scan.h"

char *secret_interpolate(const char *text, const ShellSecret *secrets, size_t count) {
    if (!text) return NULL;
    if (!secrets || count == 0) return strdup(text);

    size_t text_len = strlen(text);
    /* Worst case: every placeholder expands to max secret length */
    size_t cap = text_len * 2 + 1;
    char *out = malloc(cap);
    if (!out) return strdup(text);

    size_t oi = 0;
    const char *p = text;
    while (*p) {
        if (strncmp(p, PREFIX, PREFIX_LEN) == 0) {
            const char *name_start = p + PREFIX_LEN;
            const char *end = strstr(name_start, SUFFIX);
            if (end) {
                size_t name_len = (size_t)(end - name_start);
                /* Look up secret by name */
                const char *value = NULL;
                for (size_t i = 0; i < count; i++) {
                    if (strlen(secrets[i].name) == name_len &&
                        strncmp(secrets[i].name, name_start, name_len) == 0) {
                        value = secrets[i].value;
                        break;
                    }
                }
                if (value) {
                    size_t vlen = strlen(value);
                    /* Grow buffer if needed */
                    while (oi + vlen + 1 > cap) {
                        cap *= 2;
                        char *tmp = realloc(out, cap);
                        if (!tmp) { free(out); return strdup(text); }
                        out = tmp;
                    }
                    memcpy(out + oi, value, vlen);
                    oi += vlen;
                    p = end + SUFFIX_LEN;
                    continue;
                }
            }
        }
        /* Grow buffer if needed */
        if (oi + 2 > cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); return strdup(text); }
            out = tmp;
        }
        out[oi++] = *p++;
    }
    out[oi] = '\0';
    return out;
}

char *secret_deinterpolate(const char *text, const ShellSecret *secrets, size_t count) {
    if (!text) return NULL;
    if (!secrets || count == 0) return strdup(text);

    /* Sort indices by value length descending (longest first) */
    size_t order[128];
    size_t n = count < 128 ? count : 128;
    for (size_t i = 0; i < n; i++) order[i] = i;
    for (size_t i = 0; i < n - 1; i++)
        for (size_t j = i + 1; j < n; j++)
            if (strlen(secrets[order[j]].value) > strlen(secrets[order[i]].value)) {
                size_t tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }

    /* Iteratively replace each secret value with its placeholder */
    char *cur = strdup(text);
    if (!cur) return NULL;

    for (size_t idx = 0; idx < n; idx++) {
        const ShellSecret *s = &secrets[order[idx]];
        if (!s->value || !s->value[0]) continue;
        size_t vlen = strlen(s->value);
        /* Build placeholder: {{SECRET:name}} */
        char ph[256];
        int ph_len = snprintf(ph, sizeof(ph), "{{SECRET:%s}}", s->name);
        if (ph_len >= (int)sizeof(ph)) continue;

        /* Replace all occurrences — single forward pass (no re-scan) */
        size_t cur_len = strlen(cur);
        size_t out_cap = cur_len + 1;
        if ((size_t)ph_len > vlen)
            out_cap += (cur_len / vlen + 1) * ((size_t)ph_len - vlen);
        char *out = malloc(out_cap);
        if (!out) continue;
        size_t oi = 0;
        const char *scan = cur;
        const char *pos;
        while ((pos = strstr(scan, s->value)) != NULL) {
            size_t chunk = (size_t)(pos - scan);
            memcpy(out + oi, scan, chunk);
            oi += chunk;
            memcpy(out + oi, ph, (size_t)ph_len);
            oi += (size_t)ph_len;
            scan = pos + vlen;
        }
        size_t tail = cur_len - (size_t)(scan - cur) + 1;
        memcpy(out + oi, scan, tail);
        free(cur);
        cur = out;
    }
    return cur;
}

char *tool_result_postprocess(const char *result, const ShellSecret *secrets, size_t count) {
    if (!result) return NULL;
    char *cur = NULL;

    /* Deinterpolate: replace secret values with placeholders */
    if (secrets && count > 0) {
        cur = secret_deinterpolate(result, secrets, count);
        if (cur && strcmp(cur, result) == 0) { free(cur); cur = NULL; }
    }

    /* Secret scan: zero-alloc locate first; copy with tag headroom only on a
     * hit, so an expanding redaction never has to drop trailing bytes. */
    const char *to_scan = cur ? cur : result;
    size_t len = strlen(to_scan);
    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(to_scan, len, f, SCAN_MAX_FINDINGS);
    if (n > 0) {
        size_t cap = len + 1 + (size_t)n * 80;  /* redact tag is capped at 80 */
        char *scanned = malloc(cap);
        if (scanned) {
            memcpy(scanned, to_scan, len + 1);
            secret_scan_redact(scanned, &len, cap);
            free(cur);
            return scanned;
        }
    }

    return cur;
}
