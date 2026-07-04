#define _POSIX_C_SOURCE 200809L
#include "secret_quarantine.h"
#include "secret_interp.h"
#include "secret_scan.h"
#include "db.h"
#include "buf.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define QUARANTINE_PENDING_CAP 32

/* rule_id ("aws-access-token") -> "AWS_ACCESS_TOKEN": upcase, non-[A-Z0-9]
 * bytes become '_'. out must hold at least 32 bytes. */
static void sanitize_rule_name(const char *rule_id, char *out, size_t out_cap) {
    size_t o = 0;
    for (size_t i = 0; rule_id[i] && o + 1 < out_cap; i++) {
        char c = rule_id[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            out[o++] = c;
        else
            out[o++] = '_';
    }
    out[o] = '\0';
    if (o == 0) snprintf(out, out_cap, "UNKNOWN");
}

/* Fallback shred: identical to tool_result_postprocess's redact path. */
static char *shred(const char *to_scan, size_t len, int nf) {
    size_t cap = len + 1 + (size_t)nf * 80;
    char *scanned = malloc(cap);
    if (!scanned) return NULL;
    memcpy(scanned, to_scan, len + 1);
    secret_scan_redact(scanned, &len, cap);
    return scanned;
}

char *tool_result_postprocess_q(sqlite3 *db, const char *result,
                                const ShellSecret *secrets, size_t count) {
    if (!result) return NULL;
    char *cur = NULL;

    if (secrets && count > 0) {
        cur = secret_deinterpolate(result, secrets, count);
        if (cur && strcmp(cur, result) == 0) { free(cur); cur = NULL; }
    }

    const char *to_scan = cur ? cur : result;
    size_t len = strlen(to_scan);
    ScanFinding f[SCAN_MAX_FINDINGS];
    int nf = secret_scan(to_scan, len, f, SCAN_MAX_FINDINGS);
    if (nf <= 0) return cur;

    int pending = db ? db_secret_pending_count(db) : QUARANTINE_PENDING_CAP;
    if (!db || pending >= QUARANTINE_PENDING_CAP) {
        LOG_WARN_("secret quarantine: pending=%d >= cap=%d, falling back to redact",
                  pending, QUARANTINE_PENDING_CAP);
        char *scanned = shred(to_scan, len, nf);
        if (!scanned) return cur;
        free(cur);
        return scanned;
    }

    /* Per-call dedup: a value repeated within this one result reuses the
     * first name minted for it (mints are also visible to db_secret_exists
     * for cross-call collision avoidance on the naming suffix). */
    char *local_values[SCAN_MAX_FINDINGS];
    char *local_names[SCAN_MAX_FINDINGS];
    int local_n = 0;

    Buf out = {0};
    size_t pos = 0;
    for (int i = 0; i < nf; i++) {
        size_t off = (size_t)f[i].offset;
        size_t mlen = (size_t)f[i].match_len;
        if (off < pos || off + mlen > len) continue;  /* overlap/OOB safety */
        buf_append(&out, to_scan + pos, off - pos);

        char *value = strndup(to_scan + off, mlen);
        const char *reuse_name = NULL;
        for (size_t j = 0; j < count && !reuse_name; j++)
            if (strcmp(secrets[j].value, value) == 0) reuse_name = secrets[j].name;
        for (int j = 0; j < local_n && !reuse_name; j++)
            if (strcmp(local_values[j], value) == 0) reuse_name = local_names[j];

        char name_buf[80];
        if (reuse_name) {
            snprintf(name_buf, sizeof(name_buf), "%s", reuse_name);
        } else {
            char rule_up[40];
            sanitize_rule_name(f[i].rule_id ? f[i].rule_id : "unknown", rule_up, sizeof(rule_up));
            int suffix = 0, taken;
            do {
                snprintf(name_buf, sizeof(name_buf), "PENDING_%s_%d", rule_up, suffix++);
                taken = db_secret_exists(db, name_buf);
                for (int j = 0; !taken && j < local_n; j++)
                    if (strcmp(local_names[j], name_buf) == 0) taken = 1;
            } while (taken && suffix < 10000);
            db_secret_set(db, name_buf, value, "quarantine", "pending");
            if (local_n < SCAN_MAX_FINDINGS) {
                local_values[local_n] = strdup(value);
                local_names[local_n] = strdup(name_buf);
                local_n++;
            }
            LOG_INFO_("secret quarantine hit rule=%s name=%s", f[i].rule_id ? f[i].rule_id : "?", name_buf);
        }

        char ph[96];
        snprintf(ph, sizeof(ph), "{{SECRET:%s}}", name_buf);
        buf_append_str(&out, ph);
        free(value);
        pos = off + mlen;
    }
    buf_append_str(&out, to_scan + pos);

    for (int j = 0; j < local_n; j++) { free(local_values[j]); free(local_names[j]); }
    free(cur);

    char *result_out = buf_take(&out);
    return result_out ? result_out : strdup(to_scan);
}
