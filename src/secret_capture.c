#define _GNU_SOURCE
#include "secret_capture.h"
#include "tool_parse.h"
#include "secret_interp.h"
#include "tool_shell.h"
#include "validate.h"
#include "db.h"
#include "buf.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Big enough for any real credential (PEM private keys included); small
 * enough to refuse "save the whole page" mistakes. */
#define CAPTURE_MAX_VALUE 8192

/* Whole-result mode: trimmed copy of the raw result. */
static char *value_from_result(const char *result) {
    const char *s = result;
    while (*s && isspace((unsigned char)*s)) s++;
    const char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    return strndup(s, (size_t)(e - s));
}

/* JSON-path mode: json_extract over the raw result. Scalars come back as
 * text; a non-JSON result or missing path returns NULL. */
static char *value_from_path(sqlite3 *db, const char *result, const char *path) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT json_extract(?1, ?2)", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, result, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v && v[0]) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

static char *with_note(const char *result, const char *fmt, const char *arg) {
    Buf b = {0};
    buf_append_str(&b, result);
    buf_append_str(&b, "\n[cclaw: ");
    buf_appendf(&b, fmt, arg);
    buf_append_str(&b, "]");
    char *out = buf_take(&b);
    return out ? out : strdup(result);
}

char *secret_capture_apply(sqlite3 *db, const char *tool_args, const char *raw_result) {
    if (!db || !tool_args || !raw_result) return NULL;
    if (!strstr(tool_args, "save_secret")) return NULL;  /* cheap pre-check */

    ToolArgs ta;
    if (tool_parse(tool_args, &ta) != 0) return NULL;
    const char *name = targ_str(&ta, "save_secret");
    const char *path = targ_str(&ta, "save_secret_path");
    if (!name) { tool_parse_free(&ta); return NULL; }

    char *out = NULL;
    char *value = NULL;

    if (!is_valid_secret_name(name)) {
        out = with_note(raw_result,
            "save_secret failed: invalid name %s (expected ^[A-Z][A-Z0-9_]*$)", name);
        goto done;
    }
    if (db_secret_exists(db, name)) {
        out = with_note(raw_result,
            "save_secret failed: secret %s already exists — pick a new name "
            "or have the operator `cclaw secret rm` the old one", name);
        goto done;
    }
    /* An error result carries no credential — don't capture the error text. */
    if (strncmp(raw_result, "error:", 6) == 0) {
        out = with_note(raw_result, "save_secret skipped: tool returned an error%s", "");
        goto done;
    }

    value = path ? value_from_path(db, raw_result, path)
                 : value_from_result(raw_result);
    if (!value || !value[0]) {
        out = with_note(raw_result, path
            ? "save_secret failed: nothing at json path %s (result not JSON, "
              "or path missing)"
            : "save_secret failed: empty result%s", path ? path : "");
        goto done;
    }
    if (strlen(value) > CAPTURE_MAX_VALUE) {
        out = with_note(raw_result,
            "save_secret failed: extracted value exceeds 8KB — use "
            "save_secret_path to select the credential field%s", "");
        goto done;
    }

    if (db_secret_set(db, name, value, "captured", "agent") != 0) {
        out = with_note(raw_result, "save_secret failed: store error%s", "");
        goto done;
    }
    LOG_INFO_("secret captured name=%s len=%zu via=save_secret", name, strlen(value));

    /* Mask every occurrence (raw + base64/url-encoded variants) so the
     * value never enters the context window. */
    ShellSecret s = { .name = (char *)name, .value = value };
    char *masked = secret_deinterpolate(raw_result, &s, 1);
    char note[192];
    snprintf(note, sizeof(note),
             "saved {{SECRET:%s}} (%zu chars). No host bindings yet — first "
             "use parks for approval; ALWAYS-approve a url-bearing call to bind it",
             name, strlen(value));
    out = with_note(masked ? masked : raw_result, "%s", note);
    free(masked);

done:
    if (value) { explicit_bzero(value, strlen(value)); free(value); }
    tool_parse_free(&ta);
    return out;
}
