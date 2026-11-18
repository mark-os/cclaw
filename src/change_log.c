/* change_log.c — the setting/measurement list and the triggers built from it
 * (specs/operations.md "Change log"; plan v0.8 decisions 14 and 15).
 *
 * Why the list lives here and the triggers are generated, rather than written
 * out in templates/schema.sql: a trigger names every setting column twice
 * (UPDATE OF, WHEN) and again in each JSON object, so a hand-written copy
 * would restate the classification in a dozen places and drift. Generating
 * them from one array means a fresh database (db_ensure_schema) and an
 * upgraded one (the v60 patch, then every startup) carry byte-identical
 * triggers, and adding a table is one edit here. */
#define _POSIX_C_SOURCE 200809L
#include "change_log.h"
#include "buf.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every column of every logged table, classified by who writes it — not by
 * its name. Measurements are what the running system records about itself;
 * a setting is something a person or an agent decided.
 *  - models: status is a setting (decision 15: operator 'disabled' and the
 *    health machinery's healthy/degraded flips are both worth a line; the
 *    WHEN clause drops the no-op 'healthy' rewrite every success does).
 *    degraded_until, consec_failures, total_*, last_*_at are written by
 *    llm_proc.c on every request; synced_at has no writer today.
 *  - agents: hold_until/hold_holder are the rename lease (db.c).
 *  - created_at everywhere: stamped once by a DEFAULT, the log row's own
 *    time already says when. */
const ChangeLogColumn CHANGE_LOG_COLUMNS[] = {
    { "agents", "name",              COL_SETTING },
    { "agents", "system_prompt",     COL_SETTING },
    { "agents", "description",       COL_SETTING },
    { "agents", "max_iterations",    COL_SETTING },
    { "agents", "max_output_tokens", COL_SETTING },
    { "agents", "shell_timeout",     COL_SETTING },
    { "agents", "shell_path",        COL_SETTING },
    { "agents", "sandbox_profile",   COL_SETTING },
    { "agents", "created_by",        COL_SETTING },
    { "agents", "hold_until",        COL_MEASUREMENT },
    { "agents", "hold_holder",       COL_MEASUREMENT },
    { "agents", "created_at",        COL_MEASUREMENT },

    { "grants", "agent_name",        COL_SETTING },
    { "grants", "kind",              COL_SETTING },
    { "grants", "value",             COL_SETTING },
    { "grants", "approval_mode",     COL_SETTING },
    { "grants", "expires_at",        COL_SETTING },
    { "grants", "created_at",        COL_MEASUREMENT },
    { "grants", "source",            COL_SETTING },
    { "grants", "approval_id",       COL_SETTING },

    { "agent_models", "agent_name",       COL_SETTING },
    { "agent_models", "purpose",          COL_SETTING },
    { "agent_models", "model_id",         COL_SETTING },
    { "agent_models", "pos",              COL_SETTING },
    { "agent_models", "reasoning_effort", COL_SETTING },
    { "agent_models", "trim_threshold",   COL_SETTING },
    { "agent_models", "prune_target",     COL_SETTING },

    { "providers", "name",           COL_SETTING },
    { "providers", "base_url",       COL_SETTING },
    { "providers", "endpoint_type",  COL_SETTING },
    { "providers", "api_key_env",    COL_SETTING },
    { "providers", "default_model",  COL_SETTING },
    { "providers", "priority",       COL_SETTING },
    { "providers", "request_extra",  COL_SETTING },

    { "models", "id",                COL_SETTING },
    { "models", "provider_name",     COL_SETTING },
    { "models", "model",             COL_SETTING },
    { "models", "context_window",    COL_SETTING },
    { "models", "max_output_tokens", COL_SETTING },
    { "models", "capabilities",      COL_SETTING },
    { "models", "effort_map",        COL_SETTING },
    { "models", "status",            COL_SETTING },
    { "models", "degraded_until",    COL_MEASUREMENT },
    { "models", "total_requests",    COL_MEASUREMENT },
    { "models", "total_tokens_in",   COL_MEASUREMENT },
    { "models", "total_tokens_out",  COL_MEASUREMENT },
    { "models", "total_cost_nano",   COL_MEASUREMENT },
    { "models", "consec_failures",   COL_MEASUREMENT },
    { "models", "last_success_at",   COL_MEASUREMENT },
    { "models", "last_error_at",     COL_MEASUREMENT },
    { "models", "synced_at",         COL_MEASUREMENT },
    { "models", "created_at",        COL_MEASUREMENT },

    /* The value is ciphertext, but ciphertext is still the secret under the
     * key on the same disk — it never enters the log in any form. */
    { "secrets", "name",             COL_SETTING },
    { "secrets", "value",            COL_SECRET },
    { "secrets", "source",           COL_SETTING },
    { "secrets", "scope",            COL_SETTING },
    { "secrets", "created_at",       COL_MEASUREMENT },

    /* channel_chats: the pin and the chat's three settings are set by the
     * operator (route verbs, dashboard), an approval (request_config routes)
     * or an admin's /new and /sessions — session_id moving on /new is a
     * setting change worth a line. Everything else is observed from traffic:
     * is_admin is recomputed from each message's envelope, title and kind come
     * from it, and the timestamps record what happened. */
    { "channel_chats", "channel_name",    COL_SETTING },
    { "channel_chats", "chat_id",         COL_SETTING },
    { "channel_chats", "session_id",      COL_SETTING },
    { "channel_chats", "delivery_mode",   COL_SETTING },
    { "channel_chats", "tool_filter",     COL_SETTING },
    { "channel_chats", "guidance",        COL_SETTING },
    { "channel_chats", "title",           COL_MEASUREMENT },
    { "channel_chats", "is_admin",        COL_MEASUREMENT },
    { "channel_chats", "error_notice_at", COL_MEASUREMENT },
    { "channel_chats", "kind",            COL_MEASUREMENT },
    { "channel_chats", "first_seen_at",   COL_MEASUREMENT },
    { "channel_chats", "last_message_at", COL_MEASUREMENT },
    { "channel_chats", "notified_at",     COL_MEASUREMENT },
};
const size_t CHANGE_LOG_COLUMN_COUNT =
    sizeof(CHANGE_LOG_COLUMNS) / sizeof(CHANGE_LOG_COLUMNS[0]);

int change_log_class(const char *table, const char *column) {
    for (size_t i = 0; i < CHANGE_LOG_COLUMN_COUNT; i++)
        if (strcmp(CHANGE_LOG_COLUMNS[i].table, table) == 0 &&
            strcmp(CHANGE_LOG_COLUMNS[i].column, column) == 0)
            return (int)CHANGE_LOG_COLUMNS[i].cls;
    return -1;
}

static int table_listed(const char *table) {
    for (size_t i = 0; i < CHANGE_LOG_COLUMN_COUNT; i++)
        if (strcmp(CHANGE_LOG_COLUMNS[i].table, table) == 0) return 1;
    return 0;
}

/* One table's columns in declaration order, so generated text is stable. */
typedef struct {
    char *name;
    int cls;      /* ColumnClass; an unlisted column counts as a measurement */
    int pk;       /* position in the primary key, 0 = not part of it */
} Col;

static void cols_free(Col *c, size_t n) {
    for (size_t i = 0; i < n; i++) free(c[i].name);
    free(c);
}

static Col *table_cols(sqlite3 *db, const char *table, size_t *out_n) {
    *out_n = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT name, pk FROM pragma_table_info(?1) ORDER BY cid",
                           -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
    size_t n = 0, cap = 0;
    Col *cols = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(st, 0);
        if (!nm) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            Col *t = realloc(cols, nc * sizeof(*cols));
            if (!t) { cols_free(cols, n); sqlite3_finalize(st); return NULL; }
            cols = t; cap = nc;
        }
        int cls = change_log_class(table, nm);
        if (cls < 0) {
            /* The audit test refuses this at build time; at runtime an
             * unclassified column is left out of the log rather than risk
             * logging something that should never be written down. */
            LOG_WARN_("change_log: %s.%s is not classified — not logged", table, nm);
            cls = COL_MEASUREMENT;
        }
        cols[n].name = strdup(nm);
        cols[n].cls = cls;
        cols[n].pk = sqlite3_column_int(st, 1);
        if (!cols[n].name) { cols_free(cols, n); sqlite3_finalize(st); return NULL; }
        n++;
    }
    sqlite3_finalize(st);
    *out_n = n;
    return cols;
}

/* json_object('c', REF."c", ...) over the columns selected by `want`:
 * 'k' = primary key; 'v' = visible settings; 'i' / 'u' = visible settings
 * plus a marker for each secret column, for an insert / an update. A secret
 * column contributes only the fact of a change, never its content. */
static void json_cols(Buf *b, const Col *c, size_t n, char want, const char *ref) {
    buf_append_str(b, "json_object(");
    int first = 1;
    if (want == 'k') {
        /* Key order is the PRIMARY KEY's, not the table's. */
        for (int p = 1; p <= (int)n; p++)
            for (size_t i = 0; i < n; i++)
                if (c[i].pk == p) {
                    buf_appendf(b, "%s'%s', %s.\"%s\"", first ? "" : ", ",
                                c[i].name, ref, c[i].name);
                    first = 0;
                }
    } else {
        for (size_t i = 0; i < n; i++)
            if (c[i].cls == COL_SETTING) {
                buf_appendf(b, "%s'%s', %s.\"%s\"", first ? "" : ", ",
                            c[i].name, ref, c[i].name);
                first = 0;
            }
    }
    /* Secret columns: the fact, never the content. */
    if (want == 'i' || want == 'u') {
        for (size_t i = 0; i < n; i++)
            if (c[i].cls == COL_SECRET) {
                if (want == 'i')
                    buf_appendf(b, "%s'%s', '(set)'", first ? "" : ", ", c[i].name);
                else
                    buf_appendf(b, "%s'%s', CASE WHEN OLD.\"%s\" IS NOT NEW.\"%s\""
                                   " THEN '(changed)' ELSE '(unchanged)' END",
                                first ? "" : ", ", c[i].name, c[i].name, c[i].name);
                first = 0;
            }
    }
    buf_append_char(b, ')');
}

static char *trigger_sql(const char *table, const Col *c, size_t n, const char *act) {
    int any_setting = 0;
    for (size_t i = 0; i < n; i++)
        if (c[i].cls != COL_MEASUREMENT) any_setting = 1;
    if (!any_setting) return NULL;

    Buf b = {0};
    const char *ins = "INSERT INTO change_log(actor, context, table_name, row_key,"
                      " action, old_values, new_values) VALUES (cclaw_actor(),"
                      " cclaw_context(), ";
    if (strcmp(act, "ins") == 0) {
        buf_appendf(&b, "CREATE TRIGGER log_%s_ins AFTER INSERT ON \"%s\" BEGIN %s'%s', ",
                    table, table, ins, table);
        json_cols(&b, c, n, 'k', "NEW");
        buf_append_str(&b, ", 'insert', NULL, ");
        json_cols(&b, c, n, 'i', "NEW");
        buf_append_str(&b, "); END");
    } else if (strcmp(act, "upd") == 0) {
        buf_appendf(&b, "CREATE TRIGGER log_%s_upd AFTER UPDATE OF ", table);
        int first = 1;
        for (size_t i = 0; i < n; i++)
            if (c[i].cls != COL_MEASUREMENT) {
                buf_appendf(&b, "%s\"%s\"", first ? "" : ", ", c[i].name);
                first = 0;
            }
        buf_appendf(&b, " ON \"%s\" WHEN ", table);
        first = 1;
        for (size_t i = 0; i < n; i++)
            if (c[i].cls != COL_MEASUREMENT) {
                buf_appendf(&b, "%sOLD.\"%s\" IS NOT NEW.\"%s\"", first ? "" : " OR ",
                            c[i].name, c[i].name);
                first = 0;
            }
        buf_appendf(&b, " BEGIN %s'%s', ", ins, table);
        json_cols(&b, c, n, 'k', "NEW");
        buf_append_str(&b, ", 'update', ");
        json_cols(&b, c, n, 'v', "OLD");
        buf_append_str(&b, ", ");
        json_cols(&b, c, n, 'u', "NEW");
        buf_append_str(&b, "); END");
    } else {
        buf_appendf(&b, "CREATE TRIGGER log_%s_del AFTER DELETE ON \"%s\" BEGIN %s'%s', ",
                    table, table, ins, table);
        json_cols(&b, c, n, 'k', "OLD");
        buf_append_str(&b, ", 'delete', ");
        json_cols(&b, c, n, 'v', "OLD");
        buf_append_str(&b, ", NULL); END");
    }
    return buf_take(&b);
}

char *change_log_trigger_sql(sqlite3 *db, const char *table, const char *action) {
    if (!db || !table || !action || !table_listed(table)) return NULL;
    size_t n = 0;
    Col *c = table_cols(db, table, &n);
    if (!c || n == 0) { cols_free(c, n); return NULL; }
    char *sql = trigger_sql(table, c, n, action);
    cols_free(c, n);
    return sql;
}

static char *installed_sql(sqlite3 *db, const char *name) {
    sqlite3_stmt *st;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE type='trigger'"
                               " AND name=?1", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

/* The statements that would bring this database in line with the list, in
 * order; an empty buffer means it already is. */
static int plan_sync(sqlite3 *db, Buf *plan) {
    static const char *ACTS[] = { "ins", "upd", "del" };
    /* Distinct listed tables, in list order. */
    for (size_t i = 0; i < CHANGE_LOG_COLUMN_COUNT; i++) {
        const char *t = CHANGE_LOG_COLUMNS[i].table;
        int seen = 0;
        for (size_t j = 0; j < i; j++)
            if (strcmp(CHANGE_LOG_COLUMNS[j].table, t) == 0) { seen = 1; break; }
        if (seen) continue;
        for (size_t a = 0; a < 3; a++) {
            char name[96];
            snprintf(name, sizeof(name), "log_%s_%s", t, ACTS[a]);
            char *want = change_log_trigger_sql(db, t, ACTS[a]);
            char *have = installed_sql(db, name);
            if (!want && have)
                buf_appendf(plan, "DROP TRIGGER \"%s\";", name);
            else if (want && (!have || strcmp(want, have) != 0)) {
                if (have) buf_appendf(plan, "DROP TRIGGER \"%s\";", name);
                buf_appendf(plan, "%s;", want);
            }
            free(want);
            free(have);
        }
    }
    /* Triggers left behind by a table that is no longer listed. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT name, tbl_name FROM sqlite_master WHERE type='trigger'"
            " AND name LIKE 'log\\_%' ESCAPE '\\' ORDER BY name",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(st, 0);
        const char *tb = (const char *)sqlite3_column_text(st, 1);
        if (nm && tb && !table_listed(tb))
            buf_appendf(plan, "DROP TRIGGER \"%s\";", nm);
    }
    sqlite3_finalize(st);
    return 0;
}

int change_log_sync(sqlite3 *db) {
    if (!db) return -1;
    Buf plan = {0};
    if (plan_sync(db, &plan) != 0) { free(buf_take(&plan)); return -1; }
    char *sql = buf_take(&plan);
    if (!sql || !sql[0]) { free(sql); return 0; }
    int own_txn = sqlite3_get_autocommit(db);
    char *err = NULL;
    int rc = own_txn ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) : SQLITE_OK;
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (own_txn) {
        if (rc == SQLITE_OK) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &err);
        if (rc != SQLITE_OK) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    if (rc != SQLITE_OK)
        LOG_ERROR_("change_log: trigger sync failed: %s", err ? err : "?");
    else
        LOG_INFO_("change_log: triggers brought in line with the column list");
    sqlite3_free(err);
    free(sql);
    return rc == SQLITE_OK ? 0 : -1;
}
