#define _POSIX_C_SOURCE 200809L
#include "secret_interp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PREFIX "{{SECRET:"
#define PREFIX_LEN 9
#define SUFFIX "}}"
#define SUFFIX_LEN 2

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

        /* Replace all occurrences */
        char *pos;
        while ((pos = strstr(cur, s->value)) != NULL) {
            size_t cur_len = strlen(cur);
            size_t offset = (size_t)(pos - cur);
            size_t new_len = cur_len - vlen + (size_t)ph_len;
            char *next = malloc(new_len + 1);
            if (!next) break;
            memcpy(next, cur, offset);
            memcpy(next + offset, ph, (size_t)ph_len);
            memcpy(next + offset + (size_t)ph_len, pos + vlen, cur_len - offset - vlen + 1);
            free(cur);
            cur = next;
        }
    }
    return cur;
}
