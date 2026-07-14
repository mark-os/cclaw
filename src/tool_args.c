#define _GNU_SOURCE  /* explicit_bzero */
#include "tool_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run one single-row extraction query with (args, '$.'||key) bound.
 * Returns the malloc'd text of column 0, or NULL (absent / wrong type /
 * malformed JSON — json_* raise on invalid input, which fails the step). */
static char *extract_text(sqlite3 *db, const char *sql,
                          const char *args, const char *key) {
    if (!db || !args || !key) return NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    char path[128];
    snprintf(path, sizeof(path), "$.%s", key);
    sqlite3_bind_text(st, 1, args, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

int tool_args_valid_object(sqlite3 *db, const char *args) {
    if (!db || !args) return 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_valid(?1) AND json_type(?1)='object'",
            -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, args, -1, SQLITE_STATIC);
    int ok = (sqlite3_step(st) == SQLITE_ROW) && sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return ok;
}

char *tool_args_str(sqlite3 *db, const char *args, const char *key) {
    return extract_text(db,
        "SELECT CASE WHEN json_type(?1,?2)='text' THEN json_extract(?1,?2) END",
        args, key);
}

int tool_args_int(sqlite3 *db, const char *args, const char *key, int def) {
    char *v = extract_text(db,
        "SELECT CASE WHEN json_type(?1,?2) IN ('integer','real')"
        " THEN CAST(json_extract(?1,?2) AS INTEGER) END",
        args, key);
    if (!v) return def;
    int n = atoi(v);
    free(v);
    return n;
}

int tool_args_bool(sqlite3 *db, const char *args, const char *key, int def) {
    char *v = extract_text(db,
        "SELECT CASE WHEN json_type(?1,?2) IN ('true','false')"
        " THEN json_extract(?1,?2) END",
        args, key);
    if (!v) return def;
    int b = (v[0] == '1');
    free(v);
    return b;
}

char *tool_args_json(sqlite3 *db, const char *args, const char *key) {
    return extract_text(db,
        "SELECT CASE WHEN json_type(?1,?2) IN ('object','array')"
        " THEN json_extract(?1,?2) END",
        args, key);
}

/* ── Wire extraction ─────────────────────────────────────────────────── */

void tool_wire_args_free(ToolWireArg *a, size_t n) {
    if (!a) return;
    for (size_t i = 0; i < n; i++) {
        free(a[i].key);
        free(a[i].value);
        for (size_t j = 0; j < a[i].list_n; j++) free(a[i].list[j]);
        free(a[i].list);
    }
    free(a);
}

void tool_wire_args_wipe_free(ToolWireArg *a, size_t n) {
    if (!a) return;
    for (size_t i = 0; i < n; i++) {
        if (a[i].value) explicit_bzero(a[i].value, strlen(a[i].value));
        for (size_t j = 0; j < a[i].list_n; j++)
            if (a[i].list[j]) explicit_bzero(a[i].list[j], strlen(a[i].list[j]));
    }
    tool_wire_args_free(a, n);
}

/* file_edit shim: flatten $.edits (array of {oldText,newText}) into a
 * [old1,new1,old2,new2,...] string list. Returns -1 on any malformed or
 * non-string element — fail closed, never a partial edit list. */
static int flatten_edit_pairs(sqlite3 *db, const char *args, ToolWireArg *w) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT CASE WHEN json_type(j.value,'$.oldText')='text'"
            "            THEN json_extract(j.value,'$.oldText') END,"
            "       CASE WHEN json_type(j.value,'$.newText')='text'"
            "            THEN json_extract(j.value,'$.newText') END"
            " FROM json_each(?1,'$.edits') j ORDER BY j.id",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, args, -1, SQLITE_STATIC);
    char **list = NULL;
    size_t n = 0, cap = 0;
    int rc = 0, step;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const char *ot = (const char *)sqlite3_column_text(st, 0);
        const char *nt = (const char *)sqlite3_column_text(st, 1);
        if (!ot || !nt) { rc = -1; break; }
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 8;
            char **t = realloc(list, cap * sizeof(*list));
            if (!t) { rc = -1; break; }
            list = t;
        }
        list[n] = strdup(ot);
        list[n + 1] = strdup(nt);
        if (!list[n] || !list[n + 1]) { free(list[n]); free(list[n + 1]); rc = -1; break; }
        n += 2;
    }
    if (step != SQLITE_DONE && rc == 0) rc = -1;  /* malformed args aborts the scan */
    sqlite3_finalize(st);
    if (rc != 0) {
        for (size_t i = 0; i < n; i++) free(list[i]);
        free(list);
        return -1;
    }
    w->kind = TOOL_ARG_LIST;
    w->list = list;
    w->list_n = n;
    return 0;
}

int tool_args_extract(sqlite3 *db, const char *tool_name,
                      const char *schema_json, const char *args,
                      ToolWireArg **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!db || !args) return -1;
    if (!tool_args_valid_object(db, args)) return -1;

    /* One walk over the schema's declared properties: present params come
     * back with their json_type and extracted value (scalars unquoted,
     * bools as 1/0, sub-blobs as JSON text). Absent params are skipped.
     * NULL schema (no declared shape — tests, schemaless tools) walks the
     * args object itself: every top-level key ships. */
    sqlite3_stmt *st;
    const char *sql = schema_json
        ? "SELECT j.key, json_type(?2,'$.'||j.key), json_extract(?2,'$.'||j.key)"
          " FROM json_each(?1,'$.properties') j"
          " WHERE json_type(?2,'$.'||j.key) IS NOT NULL"
        : "SELECT j.key, json_type(?2,'$.'||j.key), json_extract(?2,'$.'||j.key)"
          " FROM json_each(?2) j";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (schema_json) sqlite3_bind_text(st, 1, schema_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, args, -1, SQLITE_STATIC);

    ToolWireArg *a = NULL;
    size_t n = 0, cap = 0;
    int rc = 0, step;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(st, 0);
        const char *type = (const char *)sqlite3_column_text(st, 1);
        const char *val = (const char *)sqlite3_column_text(st, 2);
        if (!key || !type) { rc = -1; break; }
        if (strcmp(type, "null") == 0) continue;  /* explicit null = absent */
        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            ToolWireArg *t = realloc(a, cap * sizeof(*a));
            if (!t) { rc = -1; break; }
            a = t;
        }
        ToolWireArg *w = &a[n];
        memset(w, 0, sizeof(*w));
        w->key = strdup(key);
        if (!w->key) { rc = -1; break; }
        n++;
        if (tool_name && strcmp(tool_name, "file_edit") == 0 &&
            strcmp(key, "edits") == 0) {
            if (flatten_edit_pairs(db, args, w) != 0) { rc = -1; break; }
            continue;
        }
        w->kind = (type[0] == 'o' || type[0] == 'a') ? TOOL_ARG_JSON
                                                     : TOOL_ARG_TEXT;
        w->value = val ? strdup(val) : strdup("");
        if (!w->value) { rc = -1; break; }
    }
    if (step != SQLITE_DONE && rc == 0) rc = -1;  /* malformed schema/args */
    sqlite3_finalize(st);
    if (rc != 0) {
        tool_wire_args_free(a, n);
        return -1;
    }
    *out = a;
    *out_n = n;
    return 0;
}
