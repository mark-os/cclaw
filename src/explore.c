/* explore.c — the data explorer's read API (specs/explorer.md).
 *
 * Three endpoints, all generic over tables: meta (what the tables are, what
 * each column means, how rows link), rows (a keyset page of one table), row
 * (one row in full, its outgoing links and the count of rows pointing at it).
 * The response JSON is the result of one SQL statement each; C only chooses
 * which schema names go into that statement's text.
 *
 * Request names never reach SQL text. A table name is looked up in
 * pragma_table_list and a column name in that table's pragma_table_info; the
 * spelling that goes into the query is the one the schema returned, quoted as
 * an identifier. Values (keys, filters, cursors) are always bound. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "explore.h"
#include "buf.h"
#include "change_log.h"
#include "civetweb.h"
#include "config_registry.h"
#include "dashboard.h"
#include "explore_act.h"
#include "web.h"
#include "templates.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NAME_MAX_LEN 64
#define MAX_KEY 8
#define MAX_FILTERS 16
#define MAX_LINKS 48
#define LIST_PREVIEW 200          /* characters of a long value in a list */
#define ROW_PREVIEW 1000000       /* characters of a long value on a row page */
#define COUNT_CAP 1000            /* "1000+" rather than a full scan */

/* ── hand lists: what the schema itself does not say ─────────────────────── */

/* Home page, in this order. */
static const char HOME_JSON[] =
    "[{\"title\":\"Agents\",\"tables\":[\"agents\"],"
    "\"doc\":\"Who does the work: each agent's prompt, limits and sandbox. Its models,"
    " grants, memory and sessions hang off it.\"},"
    "{\"title\":\"Providers and models\",\"tables\":[\"providers\",\"models\"],"
    "\"doc\":\"Where the thinking comes from: the APIs, and the models registered on them.\"},"
    "{\"title\":\"Channels and chats\",\"tables\":[\"channels\",\"channel_chats\"],"
    "\"doc\":\"Discord, Telegram, WhatsApp, and every chat seen on them, routed or not.\"},"
    "{\"title\":\"Sessions\",\"tables\":[\"sessions\"],"
    "\"doc\":\"Conversations: one agent, its history, and the chat it belongs to.\"},"
    "{\"title\":\"Memory blocks\",\"tables\":[\"memory_blocks\"],"
    "\"doc\":\"Named notes in an agent's prompt. A block attached to several agents is shared.\"},"
    "{\"title\":\"Extensions and tools\",\"tables\":[\"extensions\",\"tools\"],"
    "\"doc\":\"What agents can run, and the extensions it comes from.\"},"
    "{\"title\":\"Schedules\",\"tables\":[\"cron_jobs\"],"
    "\"doc\":\"Jobs that wake an agent at a time or on a period.\"},"
    "{\"title\":\"Secrets\",\"tables\":[\"secrets\"],"
    "\"doc\":\"Names, scopes and the hosts each may be sent to. Values never leave the server.\"},"
    "{\"title\":\"Approvals\",\"tables\":[\"approvals\"],"
    "\"doc\":\"Requests an agent made that needed a yes or no.\"}]";

/* Numeric codes, as the code that writes them names them: Role and
 * StopReason (types.h), llm_worker.c's job types, the outbox's degradation
 * ladder (schema.sql). */
static const char CODES_JSON[] =
    "{\"entries.role\":{\"0\":\"system\",\"1\":\"user\",\"2\":\"assistant\","
    "\"3\":\"tool\",\"4\":\"compaction\"},"
    "\"entries.stop_reason\":{\"0\":\"none\",\"1\":\"stop\",\"2\":\"length\","
    "\"3\":\"tool use\",\"4\":\"error\",\"5\":\"aborted\"},"
    "\"llm_jobs.job_type\":{\"0\":\"turn\",\"1\":\"compaction\"},"
    "\"channel_outbox.deliver_mode\":{\"0\":\"formatted\",\"1\":\"plain\"}}";

/* 0/1 columns. */
static const char BOOLS_JSON[] =
    "[\"config.secret\",\"config.required\",\"extensions.published\","
    "\"extensions.enabled\",\"agent_extensions.enabled\",\"tools.enabled\","
    "\"hooks.enabled\",\"memory_blocks.read_only\",\"agent_memory_blocks.read_only\","
    "\"inbox.consumed\",\"entries.is_error\",\"channel_chats.is_admin\","
    "\"delivery_edges.one_shot\",\"llm_jobs.recall\",\"cron_jobs.enabled\"]";

/* Links the schema does not declare (plan v0.8 decision 19 parks the
 * conversation tables' FKs): [from, [cols], to, [cols], when_col, when_val].
 * A row of `from` points at the `to` row whose cols equal its cols — and,
 * with a when, only if from.when_col = when_val. */
static const char HAND_LINKS_JSON[] =
    "["
    "[\"entries\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"entries\",[\"parent_id\"],\"entries\",[\"id\"],null,null],"
    "[\"entries\",[\"original_parent_id\"],\"entries\",[\"id\"],null,null],"
    "[\"tool_calls\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"tool_calls\",[\"entry_id\"],\"entries\",[\"id\"],null,null],"
    "[\"tool_calls\",[\"result_entry_id\"],\"entries\",[\"id\"],null,null],"
    "[\"llm_responses\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"inbox\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"approvals\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"channel_outbox\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"channel_outbox\",[\"channel_name\"],\"channels\",[\"name\"],null,null],"
    "[\"channel_events\",[\"channel_name\"],\"channels\",[\"name\"],null,null],"
    "[\"hook_directives\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"hook_directives\",[\"entry_id\"],\"entries\",[\"id\"],null,null],"
    "[\"media_jobs\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"llm_jobs\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"cron_jobs\",[\"session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"sessions\",[\"parent_session_id\"],\"sessions\",[\"id\"],null,null],"
    "[\"sessions\",[\"owner_instance\"],\"processes\",[\"instance_id\"],null,null],"
    "[\"secret_hosts\",[\"secret_name\"],\"secrets\",[\"name\"],null,null],"
    "[\"providers\",[\"api_key_env\"],\"secrets\",[\"name\"],null,null],"
    "[\"models_cache\",[\"provider_name\"],\"providers\",[\"name\"],null,null],"
    "[\"grants\",[\"value\"],\"tools\",[\"name\"],\"kind\",\"tool\"],"
    "[\"grants\",[\"value\"],\"secrets\",[\"name\"],\"kind\",\"secret\"]"
    "]";

/* Values that never leave the server. Every COL_SECRET column of the change
 * log list (secrets.value) is hidden in every row; these are the rest.
 * `when` is an SQL condition on the row (alias t): hidden only where true. */
typedef struct { const char *table, *column, *when; } Hidden;
static const Hidden HIDDEN[] = {
    /* The dashboard's own credential, and config keys declared secret (those
     * resolve from env or the secrets table, so value should be NULL — hidden
     * regardless). */
    { "config", "value",
      "(t.\"secret\" <> 0 OR t.\"key\" = 'web_admin_token')" },
    { "config", "default_value",
      "(t.\"secret\" <> 0 OR t.\"key\" = 'web_admin_token')" },
    /* A share token is a bearer capability: whoever has it reads the file. */
    { "shares", "token", NULL },
};

static const Hidden *hidden_find(const char *table, const char *column) {
    for (size_t i = 0; i < sizeof(HIDDEN) / sizeof(HIDDEN[0]); i++)
        if (strcmp(HIDDEN[i].table, table) == 0 && strcmp(HIDDEN[i].column, column) == 0)
            return &HIDDEN[i];
    return NULL;
}

int explore_hidden(const char *table, const char *column) {
    if (change_log_class(table, column) == COL_SECRET) return 1;
    const Hidden *h = hidden_find(table, column);
    return h ? (h->when ? 2 : 1) : 0;
}

/* ── small SQL helpers ───────────────────────────────────────────────────── */

static void sql_id(Buf *b, const char *name) {
    buf_append_char(b, '"');
    for (const char *p = name; *p; p++) {
        if (*p == '"') buf_append_char(b, '"');
        buf_append_char(b, *p);
    }
    buf_append_char(b, '"');
}

static void sql_str(Buf *b, const char *s) {
    buf_append_char(b, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') buf_append_char(b, '\'');
        buf_append_char(b, *p);
    }
    buf_append_char(b, '\'');
}

static void bind_text(sqlite3_stmt *st, const char *name, const char *v) {
    int i = sqlite3_bind_parameter_index(st, name);
    if (i > 0) sqlite3_bind_text(st, i, v, -1, SQLITE_TRANSIENT);
}

static void bind_int(sqlite3_stmt *st, const char *name, int v) {
    int i = sqlite3_bind_parameter_index(st, name);
    if (i > 0) sqlite3_bind_int(st, i, v);
}

/* {"error": msg[: detail]}, escaped by SQLite. */
static int fail(sqlite3 *db, int status, const char *msg, const char *detail, char **out) {
    sqlite3_stmt *st;
    *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT json_object('error', ?1 || COALESCE(': ' || ?2, ''))",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, msg, -1, SQLITE_STATIC);
        if (detail) sqlite3_bind_text(st, 2, detail, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            *out = strdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    if (!*out) *out = strdup("{\"error\":\"internal error\"}");
    return status;
}

/* Step a prepared statement whose one column is the response; copy it out
 * (the caller writes it after releasing the web lock). 404 when no row. */
static int take_json(sqlite3 *db, sqlite3_stmt *st, char **out) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW && sqlite3_column_text(st, 0)) {
        *out = strdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        return *out ? 200 : fail(db, 500, "out of memory", NULL, out);
    }
    if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
        sqlite3_finalize(st);
        return fail(db, 404, "no such row", NULL, out);
    }
    char *err = strdup(sqlite3_errmsg(db));
    sqlite3_finalize(st);
    int s = fail(db, 500, "query failed", err, out);
    free(err);
    return s;
}

/* ── JSON arrays of string triples, each element made by SQLite ──────────── */

typedef struct {
    sqlite3_stmt *st;
    Buf out;
    int n;
} Triples;

static int triples_open(sqlite3 *db, Triples *t) {
    memset(t, 0, sizeof(*t));
    return sqlite3_prepare_v2(db, "SELECT json_array(?1, ?2, ?3)", -1, &t->st, NULL)
           == SQLITE_OK ? 0 : -1;
}

static void triples_add(Triples *t, const char *a, const char *b, const char *c) {
    sqlite3_bind_text(t->st, 1, a, -1, SQLITE_TRANSIENT);
    if (b) sqlite3_bind_text(t->st, 2, b, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(t->st, 2);
    sqlite3_bind_text(t->st, 3, c, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(t->st) == SQLITE_ROW) {
        buf_append_char(&t->out, t->n++ ? ',' : '[');
        buf_append_str(&t->out, (const char *)sqlite3_column_text(t->st, 0));
    }
    sqlite3_reset(t->st);
}

static char *triples_close(Triples *t) {
    sqlite3_finalize(t->st);
    buf_append_str(&t->out, t->n ? "]" : "[]");
    return buf_take(&t->out);
}

/* ── column explanations from the comments in templates/schema.sql ────────
 * The file's conventions: comment lines right above a CREATE TABLE explain
 * the table; inside it a column's comment trails its line and continues on
 * deeper-indented comment-only lines, while comment lines at the column
 * indent explain the column that follows them. */

typedef struct {
    Buf b;
    int prev;     /* indent of the previous line's text; -1 after a blank line */
} DocText;

/* Add one comment line's text (after the "--"). Wrapped prose joins with a
 * space; indented lines are list items and start a new line; a blank
 * comment line is a paragraph break. */
static void doc_line(DocText *d, const char *s, size_t n) {
    if (n && s[0] == ' ') { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r')) n--;
    size_t lead = 0;
    while (lead < n && s[lead] == ' ') lead++;
    if (lead == n) { if (d->b.len) d->prev = -1; return; }
    int cont = lead > 0 && d->prev > 0 && (int)lead > d->prev;
    if (d->b.len) {
        if (d->prev < 0) buf_append_str(&d->b, "\n\n");
        else if (cont) buf_append_char(&d->b, ' ');
        else if (lead > 0 || d->prev > 0) buf_append_char(&d->b, '\n');
        else buf_append_char(&d->b, ' ');
    }
    buf_append(&d->b, s + lead, n - lead);
    if (!cont) d->prev = (int)lead;
}

static char *doc_take(DocText *d) {
    char *s = buf_take(&d->b);
    d->prev = 0;
    return s;
}

typedef struct {
    Triples *sink;
    DocText tdoc, cdoc, pend;
    char header[96];
    char table[NAME_MAX_LEN], col[NAME_MAX_LEN];
    int in_table, base;
} DocParse;

static void doc_emit(DocParse *p, const char *col, DocText *d) {
    char *s = doc_take(d);
    if (s && s[0]) triples_add(p->sink, p->table, col, s);
    free(s);
}

static void col_flush(DocParse *p) {
    if (p->col[0]) doc_emit(p, p->col, &p->cdoc);
    else buf_free(&p->cdoc.b);
    p->col[0] = '\0';
}

/* Identifier at s (copied into out), or 0 when s does not start with one. */
static size_t ident(const char *s, size_t n, char *out) {
    size_t i = 0;
    while (i < n && i < NAME_MAX_LEN - 1 &&
           (s[i] == '_' || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (i > 0 && s[i] >= '0' && s[i] <= '9')))
        i++;
    memcpy(out, s, i);
    out[i] = '\0';
    return i;
}

/* Offset of a trailing "--" outside quotes, or -1. */
static long comment_at(const char *s, size_t n) {
    int q = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] == '\'') q = !q;
        else if (!q && s[i] == '-' && s[i + 1] == '-') return (long)i;
    }
    return -1;
}

static void doc_comment(DocParse *p, size_t indent, const char *s, size_t n) {
    if (!p->in_table) {
        const char *h = strstr(s, "\xe2\x95\x90");   /* ═ : a section header */
        if (h && (size_t)(h - s) < n) {
            const char *a = s;
            size_t m = n;
            while (m && (*a == ' ' || (unsigned char)*a >= 0x80)) { a++; m--; }
            while (m && (a[m - 1] == ' ' || (unsigned char)a[m - 1] >= 0x80 || a[m - 1] == '\r')) m--;
            snprintf(p->header, sizeof(p->header), "%.*s", (int)m, a);
            return;
        }
        doc_line(&p->tdoc, s, n);
        return;
    }
    if (p->base >= 0 && (int)indent > p->base && p->col[0]) doc_line(&p->cdoc, s, n);
    else doc_line(&p->pend, s, n);
}

static void doc_table_start(DocParse *p, const char *s, size_t n) {
    static const char kw[] = "CREATE TABLE IF NOT EXISTS ";
    if (ident(s + sizeof(kw) - 1, n - (sizeof(kw) - 1), p->table) == 0) return;
    p->in_table = 1;
    p->base = -1;
    p->col[0] = '\0';
    if (p->tdoc.b.len) {
        doc_emit(p, NULL, &p->tdoc);
    } else if (p->header[0]) {
        triples_add(p->sink, p->table, NULL, p->header);
    }
    p->header[0] = '\0';
}

static void doc_body_line(DocParse *p, size_t indent, const char *s, size_t n) {
    if (p->base < 0) p->base = (int)indent;
    if ((int)indent == p->base) {
        col_flush(p);
        char word[NAME_MAX_LEN];
        size_t w = ident(s, n, word);
        static const char *const CONSTRAINT[] = { "PRIMARY", "FOREIGN", "UNIQUE",
                                                  "CHECK", "CONSTRAINT" };
        int is_con = 0;
        for (size_t i = 0; i < sizeof(CONSTRAINT) / sizeof(CONSTRAINT[0]); i++)
            if (strcasecmp(word, CONSTRAINT[i]) == 0) is_con = 1;
        if (w && !is_con) {
            memcpy(p->col, word, sizeof(word));
            p->cdoc = p->pend;            /* comments above it explain it */
            memset(&p->pend, 0, sizeof(p->pend));
        } else {
            buf_free(&p->pend.b);
            p->pend.prev = 0;
        }
    }
    long c = comment_at(s, n);
    if (c >= 0 && p->col[0]) doc_line(&p->cdoc, s + c + 2, n - (size_t)c - 2);
}

static void doc_parse_line(DocParse *p, const char *l, size_t n) {
    size_t indent = 0;
    while (indent < n && l[indent] == ' ') indent++;
    const char *s = l + indent;
    size_t m = n - indent;
    if (m >= 2 && s[0] == '-' && s[1] == '-') {
        doc_comment(p, indent, s + 2, m - 2);
        return;
    }
    if (!p->in_table) {
        if (m > 27 && strncmp(s, "CREATE TABLE IF NOT EXISTS ", 27) == 0) {
            doc_table_start(p, s, m);
        } else {
            buf_free(&p->tdoc.b);
            p->tdoc.prev = 0;
            p->header[0] = '\0';
        }
        return;
    }
    if (m && s[0] == ')') {
        col_flush(p);
        buf_free(&p->pend.b);
        p->in_table = 0;
        return;
    }
    if (m) doc_body_line(p, indent, s, m);
}

/* [[table, column|null, explanation], ...] for every commented table and
 * column of the schema this binary was built with. */
static char *schema_doc_json(sqlite3 *db) {
    Triples t;
    if (triples_open(db, &t) != 0) return NULL;
    DocParse p;
    memset(&p, 0, sizeof(p));
    p.sink = &t;
    for (const char *l = TPL_SCHEMA_SQL; *l;) {
        const char *e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l) : strlen(l);
        doc_parse_line(&p, l, n);
        l += n + (e ? 1 : 0);
    }
    buf_free(&p.tdoc.b);
    buf_free(&p.cdoc.b);
    buf_free(&p.pend.b);
    return triples_close(&t);
}

/* [[table, column, setting|measurement|secret], ...] — the change log's list. */
static char *class_json(sqlite3 *db) {
    static const char *const NAMES[] = { "setting", "measurement", "secret" };
    Triples t;
    if (triples_open(db, &t) != 0) return NULL;
    for (size_t i = 0; i < CHANGE_LOG_COLUMN_COUNT; i++)
        triples_add(&t, CHANGE_LOG_COLUMNS[i].table, CHANGE_LOG_COLUMNS[i].column,
                    NAMES[CHANGE_LOG_COLUMNS[i].cls]);
    return triples_close(&t);
}

/* [[table, column, always|some], ...] */
static char *hidden_json(sqlite3 *db) {
    Triples t;
    if (triples_open(db, &t) != 0) return NULL;
    for (size_t i = 0; i < CHANGE_LOG_COLUMN_COUNT; i++)
        if (CHANGE_LOG_COLUMNS[i].cls == COL_SECRET)
            triples_add(&t, CHANGE_LOG_COLUMNS[i].table, CHANGE_LOG_COLUMNS[i].column, "always");
    for (size_t i = 0; i < sizeof(HIDDEN) / sizeof(HIDDEN[0]); i++)
        triples_add(&t, HIDDEN[i].table, HIDDEN[i].column, HIDDEN[i].when ? "some" : "always");
    return triples_close(&t);
}

/* ── the tables a request may name ───────────────────────────────────────── */

/* Ordinary tables of the main schema: no sqlite_* internals, no FTS index
 * (virtual) or its shadow tables. */
#define TABLES_SQL \
    "SELECT name FROM pragma_table_list WHERE schema = 'main' AND type = 'table'" \
    " AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\'"

/* Every link, one row per column pair: declared FKs, then the hand list.
 * lid groups the pairs of one link. */
#define LINKS_CTE \
    "tbl(name) AS (" TABLES_SQL "), " \
    "links(lid, ft, tt, seq, fcol, tcol, declared, wcol, wval) AS (" \
    " SELECT 'd:' || m.name || ':' || f.id, m.name, f.\"table\", f.seq, f.\"from\"," \
    "  COALESCE(f.\"to\", (SELECT p.name FROM pragma_table_info(f.\"table\") p" \
    "                      WHERE p.pk = f.seq + 1)), 1, NULL, NULL" \
    "  FROM tbl m, pragma_foreign_key_list(m.name) f" \
    " UNION ALL" \
    " SELECT 'h:' || printf('%02d', h.key), h.value ->> 0, h.value ->> 2, c.key, c.value," \
    "  json_extract(h.value, '$[3][' || c.key || ']'), 0, h.value ->> 4, h.value ->> 5" \
    "  FROM json_each(:hand) h, json_each(h.value, '$[1]') c" \
    "  WHERE h.value ->> 0 IN tbl AND h.value ->> 2 IN tbl)"

typedef struct {
    char name[NAME_MAX_LEN];
    char type[32];
    int pk;
    int notnull;
    int hidden;           /* 0, 1 always, 2 when */
    const char *when;
} XCol;

typedef struct {
    char name[NAME_MAX_LEN];
    XCol *c;
    int n;
    int key[MAX_KEY];     /* column indexes in key order; unused when rowid */
    int nkey;
    int rowid;            /* addressed by rowid: no key, or a hidden one */
    int desc;             /* integer key: newest first */
} XTable;

static void table_free(XTable *t) { free(t->c); t->c = NULL; t->n = 0; }

static int col_find(const XTable *t, const char *name) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->c[i].name, name) == 0) return i;
    return -1;
}

static void table_key(XTable *t) {
    t->nkey = 0;
    for (int p = 1; p <= t->n && t->nkey < MAX_KEY; p++)
        for (int i = 0; i < t->n; i++)
            if (t->c[i].pk == p) t->key[t->nkey++] = i;
    t->rowid = t->nkey == 0;
    for (int k = 0; k < t->nkey; k++)
        if (t->c[t->key[k]].hidden) t->rowid = 1;
    t->desc = t->rowid ||
              (t->nkey == 1 && strcasecmp(t->c[t->key[0]].type, "INTEGER") == 0);
}

/* 0 = loaded; -1 = no such table (or not one this API serves). */
static int table_load(sqlite3 *db, const char *name, XTable *t) {
    memset(t, 0, sizeof(*t));
    if (!name || strlen(name) >= NAME_MAX_LEN) return -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT ti.name, ti.type, ti.pk, ti.\"notnull\" FROM (" TABLES_SQL ") tl,"
            " pragma_table_info(tl.name) ti WHERE tl.name = ?1 ORDER BY ti.cid",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *cn = (const char *)sqlite3_column_text(st, 0);
        const char *ty = (const char *)sqlite3_column_text(st, 1);
        if (!cn || strlen(cn) >= NAME_MAX_LEN) continue;
        XCol *nc = realloc(t->c, (size_t)(t->n + 1) * sizeof(*nc));
        if (!nc) break;
        t->c = nc;
        XCol *c = &t->c[t->n++];
        memset(c, 0, sizeof(*c));
        snprintf(c->name, sizeof(c->name), "%s", cn);
        snprintf(c->type, sizeof(c->type), "%s", ty ? ty : "");
        c->pk = sqlite3_column_int(st, 2);
        c->notnull = sqlite3_column_int(st, 3);
        c->hidden = explore_hidden(name, cn);
        const Hidden *h = hidden_find(name, cn);
        c->when = h ? h->when : NULL;
    }
    sqlite3_finalize(st);
    if (t->n == 0) { table_free(t); return -1; }
    snprintf(t->name, sizeof(t->name), "%s", name);
    table_key(t);
    return 0;
}

/* The key column (or rowid) k, read through alias a. */
static void key_ref(Buf *b, const XTable *t, int k, const char *a) {
    buf_appendf(b, "%s.", a);
    if (t->rowid) buf_append_str(b, "rowid");
    else sql_id(b, t->c[t->key[k]].name);
}

static int key_count(const XTable *t) { return t->rowid ? 1 : t->nkey; }

/* One column's value as JSON: numbers and short text as themselves, long
 * text as {"preview","length"}, JSONB as its text, other blobs as {"blob":
 * bytes}, hidden values as {"hidden":true} — never the value. */
static void val_expr(Buf *b, const XCol *c, long cap) {
    if (c->hidden == 1) { buf_append_str(b, "json('{\"hidden\":true}')"); return; }
    if (c->when) buf_appendf(b, "CASE WHEN %s THEN json('{\"hidden\":true}') ELSE ", c->when);
    Buf v = {0};
    buf_append_str(&v, "t.");
    sql_id(&v, c->name);
    char *V = buf_take(&v);
    if (!V) { b->oom = 1; return; }
    buf_appendf(b,
        "json(CASE typeof(%s)"
        " WHEN 'blob' THEN CASE WHEN json_valid(%s, 8) THEN"
        "  CASE WHEN length(json(%s)) > %ld"
        "   THEN json_object('preview', substr(json(%s), 1, %ld), 'length', length(json(%s)))"
        "   ELSE json_quote(json(%s) || '') END"
        "  ELSE json_object('blob', length(%s)) END"
        " WHEN 'text' THEN CASE WHEN length(%s) > %ld"
        "  THEN json_object('preview', substr(%s, 1, %ld), 'length', length(%s))"
        "  ELSE json_quote(%s) END"
        " ELSE json_quote(%s) END)",
        V, V, V, cap, V, cap, V, V, V, V, cap, V, cap, V, V, V);
    free(V);
    if (c->when) buf_append_str(b, " END");
}

/* json_object('key', json_array(...), 'values', json_object(...)) for alias t */
static void row_object(Buf *b, const XTable *t, long cap) {
    buf_append_str(b, "json_object('key', json_array(");
    for (int k = 0; k < key_count(t); k++) {
        if (k) buf_append_str(b, ", ");
        key_ref(b, t, k, "t");
    }
    buf_append_str(b, "), 'values', json_object(");
    for (int i = 0; i < t->n; i++) {
        if (i) buf_append_str(b, ", ");
        sql_str(b, t->c[i].name);
        buf_append_str(b, ", ");
        val_expr(b, &t->c[i], cap);
    }
    buf_append_str(b, "))");
}

/* ── meta ────────────────────────────────────────────────────────────────── */

/* {"table": rows, ...} — one count(*) per table, in one statement. */
static char *counts_json(sqlite3 *db) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, TABLES_SQL " ORDER BY name", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    Buf q = {0};
    buf_append_str(&q, "SELECT json_group_object(t, n) FROM (");
    int any = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(st, 0);
        buf_append_str(&q, any++ ? " UNION ALL SELECT " : "SELECT ");
        sql_str(&q, n);
        buf_append_str(&q, " AS t, (SELECT count(*) FROM ");
        sql_id(&q, n);
        buf_append_str(&q, ") AS n");
    }
    sqlite3_finalize(st);
    buf_append_str(&q, any ? ")" : "SELECT NULL AS t, NULL AS n WHERE 0)");
    char *sql = buf_take(&q);
    char *out = NULL;
    if (sql && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
            out = strdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    free(sql);
    return out;
}

static const char META_SQL[] =
    "WITH " LINKS_CTE ", "
    "doc(t, c, d) AS (SELECT value ->> 0, value ->> 1, value ->> 2 FROM json_each(:doc)), "
    "cls(t, c, k) AS (SELECT value ->> 0, value ->> 1, value ->> 2 FROM json_each(:cls)), "
    "hid(t, c, h) AS (SELECT value ->> 0, value ->> 1, value ->> 2 FROM json_each(:hid)), "
    "linkobj(lid, ft, tt, o) AS (SELECT lid, ft, tt, json_object("
    "  'from', ft, 'from_columns', json_group_array(fcol ORDER BY seq),"
    "  'to', tt, 'to_columns', json_group_array(tcol ORDER BY seq),"
    "  'declared', json(iif(max(declared), 'true', 'false')),"
    "  'when', CASE WHEN max(wcol) IS NULL THEN NULL ELSE json_object(max(wcol), max(wval)) END)"
    "  FROM links GROUP BY lid) "
    "SELECT json_object("
    " 'tables', (SELECT json_group_array(json_object("
    "   'name', m.name,"
    "   'doc', (SELECT d FROM doc WHERE t = m.name AND c IS NULL),"
    "   'count', json_extract(:cnt, '$.\"' || m.name || '\"'),"
    "   'key', CASE WHEN NOT EXISTS (SELECT 1 FROM pragma_table_info(m.name) WHERE pk > 0)"
    "            OR EXISTS (SELECT 1 FROM pragma_table_info(m.name) p, hid"
    "                       WHERE p.pk > 0 AND hid.t = m.name AND hid.c = p.name)"
    "          THEN json_array('rowid')"
    "          ELSE (SELECT json_group_array(name ORDER BY pk) FROM pragma_table_info(m.name)"
    "                WHERE pk > 0) END,"
    "   'columns', (SELECT json_group_array(json_object("
    "     'name', ci.name, 'type', ci.type, 'pk', ci.pk, 'notnull', ci.\"notnull\","
    "     'doc', (SELECT d FROM doc WHERE t = m.name AND c = ci.name),"
    "     'class', (SELECT k FROM cls WHERE t = m.name AND c = ci.name),"
    "     'hidden', (SELECT h FROM hid WHERE t = m.name AND c = ci.name),"
    "     'display', CASE"
    "        WHEN (json(:codes) -> (m.name || '.' || ci.name)) IS NOT NULL THEN 'code'"
    "        WHEN (m.name || '.' || ci.name) IN (SELECT value FROM json_each(:bools)) THEN 'bool'"
    "        WHEN ci.name = 'at' OR ci.name LIKE '%\\_at' ESCAPE '\\'"
    "          OR ci.name LIKE '%\\_until' ESCAPE '\\' THEN 'time' END"
    "     ) ORDER BY ci.cid) FROM pragma_table_info(m.name) ci),"
    "   'links_out', (SELECT json_group_array(json(o) ORDER BY lid) FROM linkobj WHERE ft = m.name),"
    "   'links_in', (SELECT json_group_array(json(o) ORDER BY lid) FROM linkobj WHERE tt = m.name)"
    "  ) ORDER BY m.name) FROM tbl m),"
    " 'home', json(:home),"
    " 'codes', json(:codes),"
    " 'attention', json_object("
    "   'approvals_pending', (SELECT count(*) FROM approvals WHERE state = 'pending'),"
    "   'sessions_active', (SELECT count(*) FROM sessions WHERE state <> 'idle'),"
    "   'chats_unrouted', (SELECT count(*) FROM channel_chats WHERE session_id IS NULL),"
    "   'memory_shared', (SELECT count(*) FROM (SELECT block_id FROM agent_memory_blocks"
    "                      GROUP BY block_id HAVING count(*) > 1))))";

int explore_meta(sqlite3 *db, char **out) {
    char *doc = schema_doc_json(db), *cls = class_json(db);
    char *hid = hidden_json(db), *cnt = counts_json(db);
    int status;
    sqlite3_stmt *st;
    if (!doc || !cls || !hid || !cnt) {
        status = fail(db, 500, "could not describe the schema", NULL, out);
    } else if (sqlite3_prepare_v2(db, META_SQL, -1, &st, NULL) != SQLITE_OK) {
        status = fail(db, 500, "query failed", sqlite3_errmsg(db), out);
    } else {
        bind_text(st, ":doc", doc);
        bind_text(st, ":cls", cls);
        bind_text(st, ":hid", hid);
        bind_text(st, ":cnt", cnt);
        bind_text(st, ":home", HOME_JSON);
        bind_text(st, ":codes", CODES_JSON);
        bind_text(st, ":bools", BOOLS_JSON);
        bind_text(st, ":hand", HAND_LINKS_JSON);
        status = take_json(db, st, out);
    }
    free(doc); free(cls); free(hid); free(cnt);
    return status;
}

/* ── rows ────────────────────────────────────────────────────────────────── */

typedef struct {
    int col;
    int kind;             /* 0 =, 1 IS NULL, 2 IN (array) */
    sqlite3_value *v;
} Filter;

static void filters_free(Filter *f, int n) {
    for (int i = 0; i < n; i++) sqlite3_value_free(f[i].v);
}

/* 1 when s is valid JSON of type `type` ("object", "array"); for an array,
 * of exactly n elements (n < 0: any). */
static int json_is(sqlite3 *db, const char *s, const char *type, int n) {
    sqlite3_stmt *st;
    int ok = 0;
    if (!s || sqlite3_prepare_v2(db,
            "SELECT json_valid(?1) AND json_type(?1) = ?2"
            " AND (?3 < 0 OR json_array_length(?1) = ?3)",
            -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, s, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, type, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, n);
    if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return ok;
}

/* Parse {"col": value | null | [values]} against t. 0 ok; else *err set. */
static int filters_parse(sqlite3 *db, const XTable *t, const char *json,
                         Filter *f, int *n, const char **err) {
    *n = 0;
    if (!json || !json[0]) return 0;
    if (!json_is(db, json, "object", -1)) { *err = "the filter is not a JSON object"; return -1; }
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT key, value, type FROM json_each(?1)",
            -1, &st, NULL) != SQLITE_OK) { *err = "bad filter"; return -1; }
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    int rc, ok = 1;
    while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0);
        const char *ty = (const char *)sqlite3_column_text(st, 2);
        int ci = k ? col_find(t, k) : -1;
        if (ci < 0) { *err = "no such column"; ok = 0; break; }
        if (t->c[ci].hidden) { *err = "this column's values are never shown, so it cannot be filtered"; ok = 0; break; }
        if (strcmp(ty, "object") == 0) { *err = "a filter value is a value, null, or a list"; ok = 0; break; }
        if (*n >= MAX_FILTERS) { *err = "too many filters"; ok = 0; break; }
        f[*n].col = ci;
        f[*n].kind = strcmp(ty, "null") == 0 ? 1 : strcmp(ty, "array") == 0 ? 2 : 0;
        f[*n].v = sqlite3_value_dup(sqlite3_column_value(st, 1));
        (*n)++;
    }
    if (ok && rc != SQLITE_DONE) { *err = "the filter is not a JSON object"; ok = 0; }
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

static void order_by(Buf *b, const XTable *t, const char *a) {
    for (int k = 0; k < key_count(t); k++) {
        if (k) buf_append_str(b, ", ");
        if (a) key_ref(b, t, k, a);
        else buf_appendf(b, "k%d", k);
        buf_append_str(b, t->desc ? " DESC" : " ASC");
    }
}

/* Key columns a search (?q=) looks in: the text ones not hidden. */
static int search_col(const XTable *t, int k) {
    if (t->rowid) return 0;
    const XCol *c = &t->c[t->key[k]];
    return !c->hidden && strcasecmp(c->type, "TEXT") == 0;
}

static void rows_where(Buf *b, const XTable *t, const Filter *f, int nf, int after, int search) {
    buf_append_str(b, " WHERE 1");
    if (search) {
        int any = 0;
        buf_append_str(b, " AND (");
        for (int k = 0; k < key_count(t); k++) {
            if (!search_col(t, k)) continue;
            buf_append_str(b, any++ ? " OR instr(lower(t." : "instr(lower(t.");
            sql_id(b, t->c[t->key[k]].name);
            buf_append_str(b, "), lower(:q)) > 0");
        }
        buf_append_str(b, any ? ")" : "0)");
    }
    for (int i = 0; i < nf; i++) {
        buf_append_str(b, " AND t.");
        sql_id(b, t->c[f[i].col].name);
        if (f[i].kind == 1) buf_append_str(b, " IS NULL");
        else if (f[i].kind == 2) buf_appendf(b, " IN (SELECT value FROM json_each(:f%d))", i);
        else buf_appendf(b, " = :f%d", i);
    }
    if (!after) return;
    buf_append_str(b, " AND (");
    for (int k = 0; k < key_count(t); k++) {
        if (k) buf_append_str(b, ", ");
        key_ref(b, t, k, "t");
    }
    buf_appendf(b, ") %s (", t->desc ? "<" : ">");
    for (int k = 0; k < key_count(t); k++)
        buf_appendf(b, "%sjson_extract(:after, '$[%d]')", k ? ", " : "", k);
    buf_append_str(b, ")");
}

/* page0: the rowids of one page plus one (is there more?), chosen by the key
 * index; page: those rows rendered. Only page0 scans. */
static char *rows_sql(const XTable *t, const Filter *f, int nf, int after, int search, int full) {
    Buf b = {0};
    buf_append_str(&b, "WITH page0 AS (SELECT t.rowid AS rid FROM ");
    sql_id(&b, t->name);
    buf_append_str(&b, " AS t");
    rows_where(&b, t, f, nf, after, search);
    buf_append_str(&b, " ORDER BY ");
    order_by(&b, t, "t");
    buf_append_str(&b, " LIMIT :lim + 1), page AS MATERIALIZED (SELECT ");
    for (int k = 0; k < key_count(t); k++) {
        key_ref(&b, t, k, "t");
        buf_appendf(&b, " AS k%d, ", k);
    }
    row_object(&b, t, full ? ROW_PREVIEW : LIST_PREVIEW);
    buf_append_str(&b, " AS r FROM page0 JOIN ");
    sql_id(&b, t->name);
    buf_append_str(&b, " AS t ON t.rowid = page0.rid) SELECT json_object('table', :t,"
                       " 'key', json(:keycols), 'limit', :lim, 'rows', COALESCE((SELECT"
                       " json_group_array(json(r)) FROM (SELECT r FROM page ORDER BY ");
    order_by(&b, t, NULL);
    buf_append_str(&b, " LIMIT :lim)), json('[]')), 'next', CASE WHEN (SELECT count(*)"
                       " FROM page) > :lim THEN json((SELECT json_array(");
    for (int k = 0; k < key_count(t); k++) buf_appendf(&b, "%sk%d", k ? ", " : "", k);
    buf_append_str(&b, ") FROM page ORDER BY ");
    order_by(&b, t, NULL);
    buf_append_str(&b, " LIMIT 1 OFFSET :lim - 1)) END)");
    return buf_take(&b);
}

static char *keycols_json(const XTable *t) {
    Buf b = {0};
    buf_append_char(&b, '[');
    for (int k = 0; k < key_count(t); k++) {
        if (k) buf_append_char(&b, ',');
        /* Schema names are identifiers ([A-Za-z0-9_]): no escaping needed,
         * and table_load refused anything longer than NAME_MAX_LEN. */
        buf_appendf(&b, "\"%s\"", t->rowid ? "rowid" : t->c[t->key[k]].name);
    }
    buf_append_char(&b, ']');
    return buf_take(&b);
}

static int rows_run(sqlite3 *db, const XTable *t, const ExploreQuery *q,
                    const Filter *f, int nf, char **out) {
    int after = q->after && q->after[0];
    if (after && !json_is(db, q->after, "array", key_count(t)))
        return fail(db, 400, "after must be the \"next\" of the previous page", NULL, out);
    int search = q->search && q->search[0];
    if (search) {
        int k, can = 0;
        for (k = 0; k < key_count(t); k++) can |= search_col(t, k);
        if (!can) return fail(db, 400, "this table cannot be searched", NULL, out);
    }
    char *sql = rows_sql(t, f, nf, after, search, q->full);
    char *kc = keycols_json(t);
    sqlite3_stmt *st;
    int status;
    if (!sql || !kc) {
        status = fail(db, 500, "out of memory", NULL, out);
    } else if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        status = fail(db, 500, "query failed", sqlite3_errmsg(db), out);
    } else {
        int lim = q->limit > 0 ? q->limit : EXPLORE_DEFAULT_LIMIT;
        if (lim > EXPLORE_MAX_LIMIT) lim = EXPLORE_MAX_LIMIT;
        bind_text(st, ":t", t->name);
        bind_text(st, ":keycols", kc);
        bind_int(st, ":lim", lim);
        if (after) bind_text(st, ":after", q->after);
        if (search) bind_text(st, ":q", q->search);
        for (int i = 0; i < nf; i++) {
            if (f[i].kind == 1) continue;
            char nm[16];
            snprintf(nm, sizeof(nm), ":f%d", i);
            int ix = sqlite3_bind_parameter_index(st, nm);
            if (ix > 0) sqlite3_bind_value(st, ix, f[i].v);
        }
        status = take_json(db, st, out);
    }
    free(sql);
    free(kc);
    return status;
}

int explore_rows(sqlite3 *db, const ExploreQuery *q, char **out) {
    XTable t;
    if (table_load(db, q->table, &t) != 0)
        return fail(db, 404, "no such table", q->table, out);
    Filter f[MAX_FILTERS];
    int nf = 0;
    const char *err = NULL;
    int status;
    if (filters_parse(db, &t, q->filter, f, &nf, &err) != 0)
        status = fail(db, 400, err, NULL, out);
    else
        status = rows_run(db, &t, q, f, nf, out);
    filters_free(f, nf);
    table_free(&t);
    return status;
}

/* ── row ─────────────────────────────────────────────────────────────────── */

typedef struct {
    char ft[NAME_MAX_LEN], tt[NAME_MAX_LEN];
    char fc[MAX_KEY][NAME_MAX_LEN], tc[MAX_KEY][NAME_MAX_LEN];
    int n;
    int declared;
    char wcol[NAME_MAX_LEN], wval[64];
} Link;

static void copy_name(char *dst, const unsigned char *src) {
    snprintf(dst, NAME_MAX_LEN, "%s", src ? (const char *)src : "");
}

/* A link whose values would expose a hidden column is not shown at all. */
static int link_hidden(const Link *l) {
    for (int i = 0; i < l->n; i++)
        if (explore_hidden(l->ft, l->fc[i]) || explore_hidden(l->tt, l->tc[i])) return 1;
    return 0;
}

/* Every link touching table `name`, as from-side or to-side. */
static int links_load(sqlite3 *db, const char *name, Link *L, int max) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "WITH " LINKS_CTE " SELECT lid, ft, tt, fcol, tcol, declared, wcol, wval"
            " FROM links WHERE ft = :t OR tt = :t ORDER BY lid, seq",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    bind_text(st, ":hand", HAND_LINKS_JSON);
    bind_text(st, ":t", name);
    int n = 0;
    char last[96] = "";
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *lid = (const char *)sqlite3_column_text(st, 0);
        if (strcmp(lid, last) != 0) {
            if (n >= max) break;
            snprintf(last, sizeof(last), "%s", lid);
            Link *l = &L[n++];
            memset(l, 0, sizeof(*l));
            copy_name(l->ft, sqlite3_column_text(st, 1));
            copy_name(l->tt, sqlite3_column_text(st, 2));
            l->declared = sqlite3_column_int(st, 5);
            copy_name(l->wcol, sqlite3_column_text(st, 6));
            snprintf(l->wval, sizeof(l->wval), "%s",
                     sqlite3_column_text(st, 7) ? (const char *)sqlite3_column_text(st, 7) : "");
        }
        Link *l = &L[n - 1];
        if (l->n < MAX_KEY) {
            copy_name(l->fc[l->n], sqlite3_column_text(st, 3));
            copy_name(l->tc[l->n], sqlite3_column_text(st, 4));
            l->n++;
        }
    }
    sqlite3_finalize(st);
    int kept = 0;
    for (int i = 0; i < n; i++)
        if (!link_hidden(&L[i])) L[kept++] = L[i];
    return kept;
}

/* 'from', .., 'from_columns', .., 'to', .., 'to_columns', .., 'declared', .., 'when', .. */
static void link_fields(Buf *b, const Link *l) {
    buf_append_str(b, "'from', ");
    sql_str(b, l->ft);
    buf_append_str(b, ", 'from_columns', json_array(");
    for (int i = 0; i < l->n; i++) { if (i) buf_append_str(b, ", "); sql_str(b, l->fc[i]); }
    buf_append_str(b, "), 'to', ");
    sql_str(b, l->tt);
    buf_append_str(b, ", 'to_columns', json_array(");
    for (int i = 0; i < l->n; i++) { if (i) buf_append_str(b, ", "); sql_str(b, l->tc[i]); }
    buf_appendf(b, "), 'declared', json('%s'), 'when', ", l->declared ? "true" : "false");
    if (l->wcol[0]) {
        buf_append_str(b, "json_object(");
        sql_str(b, l->wcol);
        buf_append_str(b, ", ");
        sql_str(b, l->wval);
        buf_append_str(b, ")");
    } else {
        buf_append_str(b, "NULL");
    }
}

/* x.<to col> = t.<from col> AND ... (side 0: x is the to-table) or the
 * reverse (side 1: x is the from-table, t the to-table). */
static void link_match(Buf *b, const Link *l, int side) {
    for (int i = 0; i < l->n; i++) {
        buf_append_str(b, i ? " AND x." : "x.");
        sql_id(b, side ? l->fc[i] : l->tc[i]);
        buf_append_str(b, " = t.");
        sql_id(b, side ? l->tc[i] : l->fc[i]);
    }
    if (l->wcol[0]) {
        buf_append_str(b, side ? " AND x." : " AND t.");
        sql_id(b, l->wcol);
        buf_append_str(b, " = ");
        sql_str(b, l->wval);
    }
}

static void link_out(Buf *b, const Link *l) {
    buf_append_str(b, "CASE WHEN 1");
    for (int i = 0; i < l->n; i++) {
        buf_append_str(b, " AND t.");
        sql_id(b, l->fc[i]);
        buf_append_str(b, " IS NOT NULL");
    }
    if (l->wcol[0]) {
        buf_append_str(b, " AND t.");
        sql_id(b, l->wcol);
        buf_append_str(b, " = ");
        sql_str(b, l->wval);
    }
    buf_append_str(b, " THEN json_object(");
    link_fields(b, l);
    buf_append_str(b, ", 'values', json_array(");
    for (int i = 0; i < l->n; i++) {
        if (i) buf_append_str(b, ", ");
        buf_append_str(b, "t.");
        sql_id(b, l->fc[i]);
    }
    buf_append_str(b, "), 'exists', json(iif(EXISTS (SELECT 1 FROM ");
    sql_id(b, l->tt);
    buf_append_str(b, " AS x WHERE ");
    link_match(b, l, 0);
    buf_append_str(b, "), 'true', 'false'))) END");
}

static void link_in(Buf *b, const Link *l) {
    buf_append_str(b, "json_object(");
    link_fields(b, l);
    buf_append_str(b, ", 'filter', json_object(");
    for (int i = 0; i < l->n; i++) {
        if (i) buf_append_str(b, ", ");
        sql_str(b, l->fc[i]);
        buf_append_str(b, ", t.");
        sql_id(b, l->tc[i]);
    }
    if (l->wcol[0]) {
        buf_append_str(b, ", ");
        sql_str(b, l->wcol);
        buf_append_str(b, ", ");
        sql_str(b, l->wval);
    }
    buf_appendf(b, "), 'count', (SELECT count(*) FROM (SELECT 1 FROM ");
    sql_id(b, l->ft);
    buf_append_str(b, " AS x WHERE ");
    link_match(b, l, 1);
    buf_appendf(b, " LIMIT %d)), 'count_capped', %d)", COUNT_CAP + 1, COUNT_CAP);
}

static char *row_sql(const XTable *t, const Link *L, int nl) {
    Buf b = {0};
    buf_append_str(&b, "SELECT json_object('table', :t, 'key', json(:keycols), 'row', ");
    row_object(&b, t, ROW_PREVIEW);
    buf_append_str(&b, ", 'links_out', json_array(");
    int first = 1;
    for (int i = 0; i < nl; i++)
        if (strcmp(L[i].ft, t->name) == 0) {
            if (!first) buf_append_str(&b, ", ");
            link_out(&b, &L[i]);
            first = 0;
        }
    buf_append_str(&b, "), 'links_in', json_array(");
    first = 1;
    for (int i = 0; i < nl; i++)
        if (strcmp(L[i].tt, t->name) == 0) {
            if (!first) buf_append_str(&b, ", ");
            link_in(&b, &L[i]);
            first = 0;
        }
    buf_append_str(&b, ")) FROM ");
    sql_id(&b, t->name);
    buf_append_str(&b, " AS t WHERE 1");
    for (int k = 0; k < key_count(t); k++) {
        buf_append_str(&b, " AND ");
        key_ref(&b, t, k, "t");
        buf_appendf(&b, " IS json_extract(:key, '$[%d]')", k);
    }
    return buf_take(&b);
}

int explore_row(sqlite3 *db, const ExploreQuery *q, char **out) {
    XTable t;
    if (table_load(db, q->table, &t) != 0)
        return fail(db, 404, "no such table", q->table, out);
    int status;
    Link *L = calloc(MAX_LINKS, sizeof(*L));
    int nl = L ? links_load(db, t.name, L, MAX_LINKS) : -1;
    char *sql = NULL, *kc = keycols_json(&t);
    sqlite3_stmt *st;
    if (!json_is(db, q->key, "array", key_count(&t))) {
        status = fail(db, 400, "key must be a JSON array of the key values", NULL, out);
    } else if (nl < 0 || !kc || !(sql = row_sql(&t, L, nl))) {
        status = fail(db, 500, "could not build the query", NULL, out);
    } else if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        status = fail(db, 500, "query failed", sqlite3_errmsg(db), out);
    } else {
        bind_text(st, ":t", t.name);
        bind_text(st, ":keycols", kc);
        bind_text(st, ":key", q->key);
        status = take_json(db, st, out);
    }
    free(sql);
    free(kc);
    free(L);
    table_free(&t);
    return status;
}

/* ── HTTP ────────────────────────────────────────────────────────────────── */

static const char *status_text(int s) {
    switch (s) {
    case 200: return "OK";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
    }
}

static int send_body(struct mg_connection *conn, int status, const char *type,
                     const char *body) {
    size_t n = strlen(body);
    mg_printf(conn,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n%s\r\n",
        status, status_text(status), type, n,
        strncmp(type, "text/html", 9) == 0
            ? "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline';"
              " style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:;"
              " base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n"
            : "");
    mg_write(conn, body, n);
    return status;
}

static const char NOT_SIGNED_IN_HTML[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width, initial-scale=1\">"
    "<title>cclaw</title>"
    "<style>body{font:16px/1.6 system-ui,sans-serif;max-width:36em;margin:3em auto;"
    "padding:0 16px}code{font-size:.9em}"
    "@media (prefers-color-scheme:dark){body{background:#12151b;color:#e6e9ef}}</style>"
    "<h1>Not signed in</h1>"
    "<p>This page needs the dashboard's admin token. On the machine running cclaw, run"
    " <code>cclaw dashboard</code>: it prints a link with the token. Open it once in this"
    " browser, then come back to <code>/admin</code> &mdash; or add"
    " <code>?token=&hellip;</code> to this address yourself.</p>"
    "<p>A link that worked before stops working when the token changes.</p>";

static int serve_page(struct mg_connection *conn, sqlite3 *db) {
    web_db_lock();
    int auth = dashboard_auth_check(conn, 1);
    char *tok = auth == 2 ? config_get(db, "web_admin_token") : NULL;
    web_db_unlock();
    if (auth == 0)
        return send_body(conn, 403, "text/html; charset=utf-8", NOT_SIGNED_IN_HTML);
    if (auth == 2) {
        /* The token moves into the cookie and leaves the URL (address bar,
         * history, referrers). */
        mg_printf(conn, "HTTP/1.1 303 See Other\r\nLocation: /admin\r\n"
                        "Cache-Control: no-store\r\n"
                        "Set-Cookie: cclaw_admin=%s; Path=/; HttpOnly; SameSite=Strict\r\n"
                        "Content-Length: 0\r\n\r\n", tok ? tok : "");
        free(tok);
        return 303;
    }
    return send_body(conn, 200, "text/html; charset=utf-8", TPL_EXPLORER_HTML);
}

/* /admin/explore was the page's address while the old dashboard still held
 * /admin; a bookmark (with or without ?token=) lands on the page. */
static int redirect_old(struct mg_connection *conn) {
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *qs = ri->query_string;
    mg_printf(conn, "HTTP/1.1 303 See Other\r\nLocation: /admin%s%.1024s\r\n"
                    "Cache-Control: no-store\r\nContent-Length: 0\r\n\r\n",
              qs && qs[0] ? "?" : "", qs && qs[0] ? qs : "");
    return 303;
}

/* A query parameter, URL-decoded, heap; NULL when absent. */
static char *param(const char *qs, const char *name) {
    if (!qs) return NULL;
    size_t n = strlen(qs);
    char *v = malloc(n + 1);
    if (!v) return NULL;
    if (mg_get_var(qs, n, name, v, n + 1) < 0) { free(v); return NULL; }
    return v;
}

static int api_dispatch(sqlite3 *db, const char *what, const char *qs, char **out) {
    if (strcmp(what, "meta") == 0) return explore_meta(db, out);
    if (strcmp(what, "approval") == 0) {
        char *id = param(qs, "id");
        int s = explore_approval_card(db, id, out);
        free(id);
        return s;
    }
    int is_rows = strcmp(what, "rows") == 0;
    if (!is_rows && strcmp(what, "row") != 0)
        return fail(db, 404, "no such endpoint", NULL, out);
    ExploreQuery q = {0};
    char *table = param(qs, "table"), *key = param(qs, "key");
    char *filter = param(qs, "filter"), *after = param(qs, "after");
    char *limit = param(qs, "limit"), *search = param(qs, "q");
    char *full = param(qs, "full");
    q.table = table;
    q.key = key;
    q.filter = filter;
    q.after = after;
    q.search = search;
    q.full = full && strcmp(full, "1") == 0;
    q.limit = limit ? atoi(limit) : 0;
    int status = !table ? fail(db, 400, "table is required", NULL, out)
               : is_rows ? explore_rows(db, &q, out)
                         : explore_row(db, &q, out);
    free(table); free(key); free(filter); free(after); free(limit); free(search); free(full);
    return status;
}

/* ── POST /admin/api/act: the actions (explore_act.c) ──────────────────────
 * The admin cookie alone must not be enough for another site's page to
 * trigger an action: SameSite=Strict already keeps the cookie off a
 * cross-site request in current browsers, and on top of that a request must
 *  - say Content-Type: application/json and carry X-Cclaw-Action: 1 — a
 *    cross-site HTML form can send neither, and a cross-site fetch() that
 *    sets them is preflighted, which this server never approves (no CORS
 *    headers are ever sent);
 *  - when the browser says where it comes from (Origin, Sec-Fetch-Site),
 *    come from this page's own origin. */

#define ACT_BODY_MAX (1024 * 1024)

static int header_is(struct mg_connection *conn, const char *name, const char *want, size_t n) {
    const char *v = mg_get_header(conn, name);
    return v && (n ? strncasecmp(v, want, n) == 0 : strcasecmp(v, want) == 0);
}

/* NULL when the request may act; otherwise why not. */
static const char *act_refusal(struct mg_connection *conn) {
    if (!header_is(conn, "Content-Type", "application/json", 16))
        return "an action is a JSON request (Content-Type: application/json)";
    if (!header_is(conn, "X-Cclaw-Action", "1", 0))
        return "an action must come from the cclaw page (X-Cclaw-Action header missing)";
    const char *site = mg_get_header(conn, "Sec-Fetch-Site");
    if (site && strcasecmp(site, "same-origin") != 0)
        return "cross-site requests are not accepted";
    const char *origin = mg_get_header(conn, "Origin");
    const char *host = mg_get_header(conn, "Host");
    if (origin) {
        const char *o = strstr(origin, "://");
        if (!host || !o || strcasecmp(o + 3, host) != 0)
            return "cross-site requests are not accepted";
    }
    return NULL;
}

/* The body, read whole: one mg_read returns what has arrived, not all of it. */
static char *read_body(struct mg_connection *conn, int *too_big) {
    const struct mg_request_info *ri = mg_get_request_info(conn);
    *too_big = ri->content_length > ACT_BODY_MAX;
    if (*too_big) return NULL;
    char *b = malloc(ACT_BODY_MAX + 1);
    if (!b) return NULL;
    size_t len = 0;
    int got;
    while (len < ACT_BODY_MAX && (got = mg_read(conn, b + len, ACT_BODY_MAX - len)) > 0)
        len += (size_t)got;
    if (len >= ACT_BODY_MAX && mg_read(conn, b, 1) > 0) { *too_big = 1; free(b); return NULL; }
    b[len] = '\0';
    return b;
}

static int handle_act(struct mg_connection *conn, sqlite3 *db) {
    char *out = NULL;
    int status;
    web_db_lock();
    int auth = dashboard_auth_check(conn, 0);
    const char *why = auth == 1 ? act_refusal(conn) : NULL;
    if (auth != 1)
        status = fail(db, 403, "not signed in",
                      "open the link `cclaw dashboard` prints, then reload", &out);
    else if (why)
        status = fail(db, 403, "refused", why, &out);
    else
        status = 0;
    web_db_unlock();
    if (status == 0) {
        int too_big = 0;
        char *body = read_body(conn, &too_big);
        if (!body) {
            web_db_lock();
            status = fail(db, too_big ? 413 : 500, too_big ? "the request is too large" : "out of memory",
                          NULL, &out);
            web_db_unlock();
        } else {
            status = explore_act(db, body, &out);
            /* The body may hold a secret value or an API key. */
            explicit_bzero(body, strlen(body));
            free(body);
        }
    }
    status = send_body(conn, status, "application/json; charset=utf-8",
                       out ? out : "{\"error\":\"out of memory\"}");
    free(out);
    return status;
}

int explore_handle(struct mg_connection *conn, sqlite3 *db) {
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri ? ri->local_uri : "";
    const char *method = ri->request_method ? ri->request_method : "";
    int is_api = strncmp(uri, "/admin/api/", 11) == 0;
    if (strcmp(uri, "/admin/api/act") == 0) {
        if (strcmp(method, "POST") == 0) return handle_act(conn, db);
        return send_body(conn, 405, "application/json",
                         "{\"error\":\"actions are POST requests\"}");
    }
    if (strcmp(method, "GET") != 0)
        return send_body(conn, 405, "application/json",
                         "{\"error\":\"only GET here; actions are POST /admin/api/act\"}");
    if (strcmp(uri, "/admin") == 0 || strcmp(uri, "/admin/") == 0)
        return serve_page(conn, db);
    if (strcmp(uri, "/admin/explore") == 0 || strcmp(uri, "/admin/explore/") == 0)
        return redirect_old(conn);
    if (!is_api)
        return send_body(conn, 404, "application/json", "{\"error\":\"not found\"}");

    /* The lock covers the database work only; the copy is written after it
     * is released, so a slow client cannot hold up an approval click. */
    char *out = NULL;
    int status;
    web_db_lock();
    if (dashboard_auth_check(conn, 0) != 1)
        status = fail(db, 403, "not signed in",
                      "open the link `cclaw dashboard` prints, then reload", &out);
    else
        status = api_dispatch(db, uri + strlen("/admin/api/"), ri->query_string, &out);
    web_db_unlock();
    status = send_body(conn, status, "application/json; charset=utf-8",
                       out ? out : "{\"error\":\"out of memory\"}");
    free(out);
    return status;
}
