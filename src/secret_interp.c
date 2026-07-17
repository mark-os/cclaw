#define _POSIX_C_SOURCE 200809L
#include "secret_interp.h"
#include "buf.h"
#include "log.h"
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

    Buf b = {0};
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
                    buf_append_str(&b, value);
                    p = end + SUFFIX_LEN;
                    continue;
                }
                /* No matching secret — placeholder passes through literally */
                char miss_name[128];
                if (name_len < sizeof(miss_name)) {
                    memcpy(miss_name, name_start, name_len);
                    miss_name[name_len] = '\0';
                    LOG_DEBUG_("secret_interp miss name=%s", miss_name);
                }
            }
        }
        buf_append_char(&b, *p++);
    }

    /* OOM: fall back to uninterpolated copy (matches old behavior) */
    char *result = buf_take(&b);
    if (!result) return strdup(text);
    return result;
}

char **secret_placeholder_names(const char *text, size_t *out_count) {
    *out_count = 0;
    if (!text) return NULL;
    char **names = NULL;
    size_t count = 0, cap = 0;
    const char *p = text;
    while ((p = strstr(p, PREFIX)) != NULL) {
        const char *name_start = p + PREFIX_LEN;
        const char *end = strstr(name_start, SUFFIX);
        if (!end) break;
        size_t name_len = (size_t)(end - name_start);
        p = end + SUFFIX_LEN;
        if (name_len == 0 || name_len >= 128) continue;
        int dup = 0;
        for (size_t i = 0; i < count; i++)
            if (strlen(names[i]) == name_len &&
                strncmp(names[i], name_start, name_len) == 0) { dup = 1; break; }
        if (dup) continue;
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            char **tmp = realloc(names, ncap * sizeof(char *));
            if (!tmp) { secret_names_free(names, count); *out_count = 0; return NULL; }
            names = tmp; cap = ncap;
        }
        names[count] = strndup(name_start, name_len);
        if (!names[count]) { secret_names_free(names, count); *out_count = 0; return NULL; }
        count++;
    }
    *out_count = count;
    return names;
}

void secret_names_free(char **names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

static const ShellSecret *s_deinterp_secrets; /* qsort context */

static int cmp_secret_len_desc(const void *a, const void *b) {
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    size_t la = strlen(s_deinterp_secrets[ia].value);
    size_t lb = strlen(s_deinterp_secrets[ib].value);
    return (la < lb) - (la > lb); /* descending */
}

/* Base64-encode src into dst. Returns bytes written (no NUL), 0 if too small. */
static size_t b64_encode(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = ((src_len + 2) / 3) * 4;
    if (needed > dst_cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        unsigned char a = (unsigned char)src[i];
        unsigned char b = (i + 1 < src_len) ? (unsigned char)src[i + 1] : 0;
        unsigned char c = (i + 2 < src_len) ? (unsigned char)src[i + 2] : 0;
        dst[o++] = t[a >> 2];
        dst[o++] = t[((a & 3) << 4) | (b >> 4)];
        dst[o++] = (i + 1 < src_len) ? t[((b & 0xf) << 2) | (c >> 6)] : '=';
        dst[o++] = (i + 2 < src_len) ? t[c & 0x3f] : '=';
    }
    return o;
}

/* URL-encode src into dst. Returns bytes written (no NUL), 0 if too small. */
static size_t url_encode(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char ch = (unsigned char)src[i];
        int safe = ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                    ch == '.' || ch == '~');
        if (safe) {
            if (o + 1 > dst_cap) return 0;
            dst[o++] = (char)ch;
        } else {
            if (o + 3 > dst_cap) return 0;
            dst[o++] = '%';
            dst[o++] = hex[ch >> 4];
            dst[o++] = hex[ch & 0xf];
        }
    }
    return o;
}

/* Replace every occurrence of needle with ph in cur. Returns a fresh string
 * (caller frees the old cur). On OOM leaves cur unchanged and returns it. */
static char *replace_all(char *cur, const char *needle, size_t nlen,
                         const char *ph, size_t ph_len) {
    if (!needle || nlen == 0) return cur;
    size_t cur_len = strlen(cur);
    size_t out_cap = cur_len + 1;
    if (ph_len > nlen)
        out_cap += (cur_len / nlen + 1) * (ph_len - nlen);
    char *out = malloc(out_cap);
    if (!out) return cur;
    size_t oi = 0;
    const char *scan = cur, *pos;
    while ((pos = strstr(scan, needle)) != NULL) {
        size_t chunk = (size_t)(pos - scan);
        memcpy(out + oi, scan, chunk); oi += chunk;
        memcpy(out + oi, ph, ph_len); oi += ph_len;
        scan = pos + nlen;
    }
    size_t tail = cur_len - (size_t)(scan - cur) + 1;
    memcpy(out + oi, scan, tail);
    free(cur);
    return out;
}

char *secret_deinterpolate(const char *text, const ShellSecret *secrets, size_t count) {
    if (!text) return NULL;
    if (!secrets || count == 0) return strdup(text);

    /* Sort indices by value length descending (longest first) */
    size_t *order = malloc(count * sizeof(size_t));
    if (!order) return strdup(text);
    for (size_t i = 0; i < count; i++) order[i] = i;
    s_deinterp_secrets = secrets;
    qsort(order, count, sizeof(size_t), cmp_secret_len_desc);

    char *cur = strdup(text);
    if (!cur) { free(order); return NULL; }

    for (size_t idx = 0; idx < count; idx++) {
        const ShellSecret *s = &secrets[order[idx]];
        if (!s->value || !s->value[0]) continue;
        size_t vlen = strlen(s->value);

        char ph[256];
        int phl = snprintf(ph, sizeof(ph), "{{SECRET:%s}}", s->name);
        if (phl <= 0 || phl >= (int)sizeof(ph)) continue;
        size_t ph_len = (size_t)phl;

        /* Raw value, then its base64 and URL-encoded variants. Encoded forms
         * are longer than the raw value, so masking them after the raw pass
         * can never re-hit an already-substituted region. */
        cur = replace_all(cur, s->value, vlen, ph, ph_len);

        char b64[1024];
        size_t b64len = b64_encode(b64, sizeof(b64) - 1, s->value, vlen);
        if (b64len > 0 && b64len != vlen) {
            b64[b64len] = '\0';  /* replace_all uses strstr — needs termination */
            cur = replace_all(cur, b64, b64len, ph, ph_len);
        }

        char urlenc[2048];
        size_t urllen = url_encode(urlenc, sizeof(urlenc) - 1, s->value, vlen);
        if (urllen > 0 && urllen != vlen) {
            urlenc[urllen] = '\0';
            cur = replace_all(cur, urlenc, urllen, ph, ph_len);
        }
    }
    free(order);
    return cur;
}

/* Appended once after any redaction so the agent knows the mangling was
 * deliberate and what the sanctioned capture paths are. */
#define SCAN_REDACT_HINT \
    "\n[cclaw: credential-like text was redacted. To capture a credential " \
    "for use, pass save_secret on the retrieving call; an operator can " \
    "store one with `cclaw secret set <NAME>`.]"

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
        size_t cap = len + 1 + (size_t)n * 80   /* redact tag is capped at 80 */
                     + sizeof(SCAN_REDACT_HINT);
        char *scanned = malloc(cap);
        if (!scanned) {
            /* Fail closed: callers treat a NULL return as "keep the original
             * text", which here would pass the un-redacted findings through
             * to the context. Withhold the result instead. */
            free(cur);
            return strdup("[tool result withheld: secret scan found matches "
                          "but redaction ran out of memory]");
        }
        memcpy(scanned, to_scan, len + 1);
        secret_scan_redact(scanned, &len, cap);
        size_t hlen = sizeof(SCAN_REDACT_HINT) - 1;
        if (len + hlen < cap) {
            memcpy(scanned + len, SCAN_REDACT_HINT, hlen + 1);
            len += hlen;
        }
        free(cur);
        return scanned;
    }

    return cur;
}
