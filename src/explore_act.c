/* explore_act.c — the data explorer's named write actions (specs/explorer.md
 * "Actions"). One function per action; each refuses with a sentence the
 * operator can act on, and otherwise calls the same code the CLI verb or the
 * agent-side path calls: agent_config_grant (grants), agent_definition_apply
 * (copying an agent), channel_pin_session / channel_pin_new_session (every
 * pin), memory_block_* (memory), db_secret_* (secrets), models_cache_* (a
 * provider's catalog), approval_decision_queue (approvals).
 *
 * Every write runs on the web server's own handle, whose actor is
 * "dashboard"; the change_log triggers record it with context
 * "act:<action>". Nothing here writes conversation history. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "explore_act.h"
#include "admin_api.h"
#include "agent_config.h"
#include "agent_define.h"
#include "approval.h"
#include "channel.h"
#include "config_registry.h"
#include "db.h"
#include "log.h"
#include "models_cache.h"
#include "validate.h"
#include "wake.h"
#include "web.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HEAP_MAX 64
#define MSG_MAX 1536

typedef struct {
    sqlite3 *db;
    const char *body;
    char *out;
    char *heap[HEAP_MAX];   /* strings this action owns; zeroed and freed after it */
    int nheap;
    int locked;
} Act;

static char *s_agents_dir;

void explore_set_agents_dir(const char *dir) {
    free(s_agents_dir);
    s_agents_dir = dir && dir[0] ? strdup(dir) : NULL;
}

static void lock(Act *a) { if (!a->locked) { web_db_lock(); a->locked = 1; } }
static void unlock(Act *a) { if (a->locked) { web_db_unlock(); a->locked = 0; } }

/* Own s for the rest of the action (NULL passes through). */
static char *keep(Act *a, char *s) {
    if (!s) return NULL;
    if (a->nheap < HEAP_MAX) a->heap[a->nheap++] = s;
    return s;
}

static void heap_free(Act *a) {
    for (int i = 0; i < a->nheap; i++) {
        explicit_bzero(a->heap[i], strlen(a->heap[i]));
        free(a->heap[i]);
    }
    a->nheap = 0;
}

/* ── answers ─────────────────────────────────────────────────────────────── */

/* {key: msg} with `extra` (a JSON object, or NULL) merged in; SQLite escapes. */
static int answer(Act *a, int status, const char *key, const char *msg, const char *extra) {
    sqlite3_stmt *st;
    free(a->out);
    a->out = NULL;
    if (sqlite3_prepare_v2(a->db,
            "SELECT json_patch(CASE WHEN ?1 = 'ok' THEN json_object('ok', json('true'), 'message', ?2)"
            "                  ELSE json_object('error', ?2) END, COALESCE(?3, '{}'))",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, msg, -1, SQLITE_STATIC);
        if (extra) sqlite3_bind_text(st, 3, extra, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
            a->out = strdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    if (!a->out) {
        a->out = strdup("{\"error\":\"internal error\"}");
        return 500;
    }
    return status;
}

static int vanswer(Act *a, int status, const char *key, const char *extra,
                   const char *fmt, va_list ap) {
    char msg[MSG_MAX];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    return answer(a, status, key, msg, extra);
}

__attribute__((format(printf, 3, 4)))
static int refuse(Act *a, int status, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int s = vanswer(a, status, "error", NULL, fmt, ap);
    va_end(ap);
    return s;
}

__attribute__((format(printf, 4, 5)))
static int refuse_x(Act *a, int status, const char *extra, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int s = vanswer(a, status, "error", extra, fmt, ap);
    va_end(ap);
    return s;
}

__attribute__((format(printf, 3, 4)))
static int done(Act *a, const char *extra, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int s = vanswer(a, 200, "ok", extra, fmt, ap);
    va_end(ap);
    return s;
}

static int busy(Act *a) {
    return refuse(a, 503, "The database is busy right now. Try again in a moment.");
}

/* ── the request's fields (SQLite is the parser) ─────────────────────────── */

/* One expression over the body (b) at key's path (p). Keys are literals. */
static char *body_expr(Act *a, const char *expr, const char *key) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT %s FROM (SELECT ?1 AS b, '$.\"' || ?2 || '\"' AS p)", expr);
    sqlite3_stmt *st;
    char *out = NULL;
    if (sqlite3_prepare_v2(a->db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, a->body, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        out = strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return keep(a, out);
}

/* JSON type of the field: "text", "integer", "array", "object", "null",
 * "true", "false", "real", or "" when absent. */
static const char *ftype(Act *a, const char *key) {
    char *t = body_expr(a, "json_type(b, p)", key);
    return t ? t : "";
}

static int fhas(Act *a, const char *key) { return ftype(a, key)[0] != '\0'; }
static int fis(Act *a, const char *key, const char *type) { return strcmp(ftype(a, key), type) == 0; }

/* A text field, trimmed; NULL when absent, null, or not text. */
static char *ftext(Act *a, const char *key) {
    if (!fis(a, key, "text")) return NULL;
    return body_expr(a, "trim(b ->> p)", key);
}

/* A text field exactly as sent (secret values and notes are not trimmed). */
static char *fraw_text(Act *a, const char *key) {
    if (!fis(a, key, "text")) return NULL;
    return body_expr(a, "b ->> p", key);
}

/* The field as JSON text (arrays, objects). */
static char *fjson(Act *a, const char *key) { return body_expr(a, "b -> p", key); }

/* An integer field; 1 when present as an integer (or an integer-valued
 * string, as a form sends), 0 otherwise. */
static int fint(Act *a, const char *key, long long *v) {
    const char *t = ftype(a, key);
    char *s = NULL;
    if (strcmp(t, "integer") == 0) s = body_expr(a, "b ->> p", key);
    else if (strcmp(t, "text") == 0)
        s = body_expr(a, "CASE WHEN trim(b ->> p) GLOB '[0-9]*' AND trim(b ->> p) NOT GLOB '*[^0-9]*'"
                         " THEN trim(b ->> p) END", key);
    if (!s) return 0;
    *v = atoll(s);
    return 1;
}

static int fbool(Act *a, const char *key, int dflt) {
    const char *t = ftype(a, key);
    if (strcmp(t, "true") == 0) return 1;
    if (strcmp(t, "false") == 0) return 0;
    if (strcmp(t, "integer") == 0) { long long v = 0; fint(a, key, &v); return v != 0; }
    return dflt;
}

/* The first key of the body not in `allowed` (a JSON array), or NULL. */
static char *unknown_key(Act *a, const char *allowed) {
    sqlite3_stmt *st;
    char *out = NULL;
    if (sqlite3_prepare_v2(a->db,
            "SELECT key FROM json_each(?1) WHERE key <> 'action'"
            " AND key NOT IN (SELECT value FROM json_each(?2)) LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, a->body, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, allowed, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        out = strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return keep(a, out);
}

/* ── queries ─────────────────────────────────────────────────────────────── */

static sqlite3_stmt *prep(Act *a, const char *sql, const char *p1, const char *p2,
                          const char *p3, const char *p4) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(a->db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN_("explorer action: prepare failed: %s", sqlite3_errmsg(a->db));
        return NULL;
    }
    const char *p[4] = { p1, p2, p3, p4 };
    int n = sqlite3_bind_parameter_count(st);
    for (int i = 0; i < 4 && i < n; i++)
        if (p[i]) sqlite3_bind_text(st, i + 1, p[i], -1, SQLITE_TRANSIENT);
    return st;
}

/* First column of the first row as text (heap, kept), NULL when none. */
static char *qs(Act *a, const char *sql, const char *p1, const char *p2, const char *p3) {
    sqlite3_stmt *st = prep(a, sql, p1, p2, p3, NULL);
    char *out = NULL;
    if (st && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        out = strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return keep(a, out);
}

/* First column as an integer; -1 when no row or NULL. */
static long long qi(Act *a, const char *sql, const char *p1, const char *p2, const char *p3) {
    sqlite3_stmt *st = prep(a, sql, p1, p2, p3, NULL);
    long long v = -1;
    if (st && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Run a write; the number of rows changed, or -1. */
static int qx(Act *a, const char *sql, const char *p1, const char *p2,
              const char *p3, const char *p4) {
    sqlite3_stmt *st = prep(a, sql, p1, p2, p3, p4);
    if (!st) return -1;
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN_("explorer action: write failed: %s", sqlite3_errmsg(a->db));
        return -1;
    }
    return sqlite3_changes(a->db);
}

static int tx_begin(Act *a) {
    return sqlite3_exec(a->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static int tx_end(Act *a, int ok) {
    if (ok && sqlite3_exec(a->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) return 0;
    sqlite3_exec(a->db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

static char *num(Act *a, long long v) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", v);
    return keep(a, strdup(b));
}

static int agent_exists(Act *a, const char *name) {
    return name && qi(a, "SELECT 1 FROM agents WHERE name=?1", name, NULL, NULL) == 1;
}

/* A host rule as grants and secret_hosts store it: "api.example.com",
 * ".example.com" (a suffix), optionally with a port. */
static int host_ok(const char *h) {
    if (!h || !h[0] || strlen(h) > 253) return 0;
    for (const char *p = h; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':' ||
              *p == '*' || *p == '_'))
            return 0;
    return 1;
}

static int url_ok(const char *u) {
    if (!u) return 0;
    if (strncmp(u, "https://", 8) != 0 && strncmp(u, "http://", 7) != 0) return 0;
    if (strlen(u) < 10) return 0;
    for (const char *p = u; *p; p++) if (*p <= ' ') return 0;
    return 1;
}

static const char *wire_name(const char *t) {
    return !t ? "?" : strcmp(t, "openai") == 0 ? "OpenAI-compatible"
         : strcmp(t, "gemini") == 0 ? "Google Gemini" : strcmp(t, "anthropic") == 0
         ? "Anthropic Messages" : t;
}

static int wire_ok(const char *t) {
    return t && (strcmp(t, "openai") == 0 || strcmp(t, "gemini") == 0 ||
                 strcmp(t, "anthropic") == 0);
}

/* A JSON array of tool names that all exist; refuses otherwise (status set). */
static int tools_list_ok(Act *a, const char *json, const char *what, int *status) {
    if (!json || qi(a, "SELECT json_type(?1) = 'array'", json, NULL, NULL) != 1) {
        *status = refuse(a, 400, "%s must be a list of tool names, or \"unrestricted\".", what);
        return 0;
    }
    if (qi(a, "SELECT json_array_length(?1)", json, NULL, NULL) == 0) {
        *status = refuse(a, 400, "%s is empty, which would allow no tools at all. Pick at least one"
                                 " tool, or choose \"unrestricted\".", what);
        return 0;
    }
    char *bad = qs(a, "SELECT CASE WHEN type = 'text' THEN value ELSE '(not a name)' END"
                      " FROM json_each(?1) WHERE type <> 'text'"
                      " OR value NOT IN (SELECT name FROM tools) LIMIT 1", json, NULL, NULL);
    if (bad) {
        *status = refuse(a, 400, "There is no tool called \"%s\". Pick tools from the list.", bad);
        return 0;
    }
    return 1;
}

/* ── grants ──────────────────────────────────────────────────────────────── */

static const char *const KINDS[] = { "tool", "host", "read_path", "write_path", "secret" };

static int kind_ok(const char *k) {
    for (size_t i = 0; k && i < sizeof(KINDS) / sizeof(KINDS[0]); i++)
        if (strcmp(k, KINDS[i]) == 0) return 1;
    return 0;
}

/* The value checks the CLI verbs and agent paths make for each kind. 0 = ok;
 * otherwise the refusal's status. */
static int grant_value_check(Act *a, const char *kind, const char *value) {
    if (strcmp(kind, "tool") == 0) {
        if (qi(a, "SELECT 1 FROM tools WHERE name=?1", value, NULL, NULL) != 1)
            return refuse(a, 400, "There is no tool called \"%s\". Pick one from the list.", value);
    } else if (strcmp(kind, "host") == 0) {
        if (!host_ok(value))
            return refuse(a, 400, "\"%s\" is not a host name. Give one like api.github.com, or"
                                  " .github.com for every subdomain (no https://, no path).", value);
    } else if (strcmp(kind, "secret") == 0) {
        if (!is_valid_secret_name(value))
            return refuse(a, 400, "\"%s\" is not a secret name (capital letters, digits and _,"
                                  " starting with a letter).", value);
        if (qi(a, "SELECT 1 FROM secrets WHERE name=?1 AND scope='system'", value, NULL, NULL) == 1)
            return refuse(a, 400, "%s is a system secret (a provider key): the daemon uses it, and"
                                  " no agent can be granted it.", value);
    } else {
        if (value[0] != '/')
            return refuse(a, 400, "A path grant needs an absolute path, starting with /.");
        if (grant_path_hits_db(a->db, kind, value))
            return refuse(a, 400, "%s would expose cclaw.db or its key to the agent's tools. The"
                                  " database is never reachable from a sandboxed tool, whatever the"
                                  " grant; agents read it with db_query.", value);
    }
    return 0;
}

/* The approval mode a new tool grant starts with when none is chosen: tools
 * listed in agent_approval_tools ask each time, as they do for a new agent. */
static const char *default_mode(Act *a, const char *tool) {
    char *list = keep(a, config_get(a->db, "agent_approval_tools"));
    if (list && qi(a, "SELECT 1 FROM json_each(?1) WHERE value=?2", list, tool, NULL) == 1)
        return "always";
    return "silent";
}

static int act_grant_add(Act *a) {
    char *agent = ftext(a, "agent"), *kind = ftext(a, "kind"), *value = ftext(a, "value");
    char *mode = ftext(a, "approval_mode");
    if (!agent_exists(a, agent)) return refuse(a, 404, "There is no agent called \"%s\".", agent ? agent : "");
    if (!kind_ok(kind))
        return refuse(a, 400, "The kind must be one of tool, host, read_path, write_path or secret.");
    if (!value || !value[0]) return refuse(a, 400, "Say what to grant: a %s.", kind);
    int s = grant_value_check(a, kind, value);
    if (s) return s;
    int is_tool = strcmp(kind, "tool") == 0;
    if (mode && !is_tool)
        return refuse(a, 400, "An approval mode applies to tool grants only.");
    if (mode && strcmp(mode, "silent") != 0 && strcmp(mode, "always") != 0 &&
        strcmp(mode, "tool_decides") != 0)
        return refuse(a, 400, "The approval mode must be silent, always or tool_decides.");
    long long exp = 0;
    if (fhas(a, "expires_at") && !fis(a, "expires_at", "null")) {
        if (!fint(a, "expires_at", &exp))
            return refuse(a, 400, "The expiry must be a time (unix seconds), or empty for never.");
        if (exp <= (long long)time(NULL))
            return refuse(a, 400, "The expiry is in the past. Pick a later time, or leave it empty.");
    }
    if (is_tool && !mode) mode = (char *)default_mode(a, value);

    if (tx_begin(a) != 0) return busy(a);
    long long live = qi(a, "SELECT expires_at IS NULL FROM grants WHERE agent_name=?1 AND kind=?2"
                           " AND value=?3 AND (expires_at IS NULL OR expires_at > unixepoch())",
                        agent, kind, value);
    char *cur_mode = qs(a, "SELECT approval_mode FROM grants WHERE agent_name=?1 AND kind=?2"
                           " AND value=?3", agent, kind, value);
    if (live == 1 && (!is_tool || (cur_mode && strcmp(cur_mode, mode) == 0)) && !exp) {
        tx_end(a, 0);
        return refuse(a, 409, "%s already has this grant, with no expiry.", agent);
    }
    int ok = agent_config_grant(a->db, agent, kind, value, exp, GRANT_ORIGIN("operator", 0)) == 0;
    if (ok && is_tool)
        ok = agent_config_set_tool_mode(a->db, agent, value, mode, GRANT_ORIGIN("operator", 0)) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The grant could not be saved.");
    static const char *MODE_WORDS[] = { "silent", "and runs it freely", "always",
                                        "and asks you each time", "tool_decides",
                                        "and the tool decides when to ask" };
    const char *mw = "";
    for (int i = 0; is_tool && i < 6; i += 2) if (strcmp(mode, MODE_WORDS[i]) == 0) mw = MODE_WORDS[i + 1];
    return done(a, NULL, "%s may now use %s %s%s%s%s.", agent,
                strcmp(kind, "tool") == 0 ? "the tool" : strcmp(kind, "host") == 0 ? "the host"
                : strcmp(kind, "secret") == 0 ? "the secret" : strcmp(kind, "read_path") == 0
                ? "(read) the path" : "(read and write) the path", value,
                is_tool ? " " : "", mw, exp ? ", until the expiry" : "");
}

static int act_grant_remove(Act *a) {
    char *agent = ftext(a, "agent"), *kind = ftext(a, "kind"), *value = ftext(a, "value");
    if (!agent_exists(a, agent)) return refuse(a, 404, "There is no agent called \"%s\".", agent ? agent : "");
    if (!kind_ok(kind) || !value)
        return refuse(a, 400, "Say which grant: its kind and value.");
    if (tx_begin(a) != 0) return busy(a);
    if (qi(a, "SELECT 1 FROM grants WHERE agent_name=?1 AND kind=?2 AND value=?3",
           agent, kind, value) != 1) {
        tx_end(a, 0);
        return refuse(a, 404, "%s has no %s grant for %s.", agent, kind, value);
    }
    int ok = agent_config_revoke(a->db, agent, kind, value) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The grant could not be removed.");
    return done(a, NULL, "%s may no longer use %s.", agent, value);
}

/* ── copy an agent ───────────────────────────────────────────────────────── */

static int act_agent_copy(Act *a) {
    char *from = ftext(a, "from"), *name = ftext(a, "name"), *desc = ftext(a, "description");
    int attach = fbool(a, "attach_memory", 0);
    if (!agent_exists(a, from)) return refuse(a, 404, "There is no agent called \"%s\".", from ? from : "");
    if (!name || !is_valid_agent_name(name))
        return refuse(a, 400, "The new name must be PascalCase: a capital letter first, then"
                              " letters and digits only (for example Researcher2).");
    if (agent_exists(a, name))
        return refuse(a, 409, "There is already an agent called %s. Pick another name.", name);
    /* The operator's clone_from (creator NULL: no creator caps). Memory is
     * not part of a clone; on request each of the source's blocks is shared
     * read-only, the default for any attachment (specs/memory.md). */
    char *def = NULL;
    sqlite3_stmt *st = prep(a,
        "SELECT json_patch(json_object('name', ?1, 'clone_from', ?2),"
        "  json_object('description', ?3, 'memory_blocks', CASE WHEN ?4 = '1' THEN"
        "    (SELECT json_group_array(json_object('attach', block_id, 'read_only', 1))"
        "       FROM agent_memory_blocks WHERE agent_name = ?2) END))",
        name, from, desc && desc[0] ? desc : NULL, attach ? "1" : "0");
    if (st && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        def = keep(a, strdup((const char *)sqlite3_column_text(st, 0)));
    sqlite3_finalize(st);
    if (!def) return refuse(a, 500, "Could not build the copy.");
    char *err = NULL;
    int rc = agent_definition_apply(a->db, def, NULL, s_agents_dir,
                                    GRANT_ORIGIN("agent_create", 0), &err);
    if (rc != 0) {
        int s = refuse(a, 400, "Could not copy %s: %s.", from, err ? err : "the definition was refused");
        free(err);
        return s;
    }
    free(err);
    char *extra = qs(a, "SELECT json_object('agent', ?1)", name, NULL, NULL);
    long long ng = qi(a, "SELECT count(*) FROM grants WHERE agent_name=?1", name, NULL, NULL);
    long long nm = qi(a, "SELECT count(*) FROM agent_models WHERE agent_name=?1", name, NULL, NULL);
    long long ne = qi(a, "SELECT count(*) FROM agent_extensions WHERE agent_name=?1", name, NULL, NULL);
    long long nb = qi(a, "SELECT count(*) FROM agent_memory_blocks WHERE agent_name=?1", name, NULL, NULL);
    return done(a, extra, "Created %s as a copy of %s: %lld grants, %lld model list entries,"
                " %lld extensions, %lld memory blocks shared read-only.", name, from, ng, nm, ne, nb);
}

/* ── model lists ─────────────────────────────────────────────────────────── */

static const char *purpose_word(const char *p) {
    return strcmp(p, "image") == 0 ? "images" : strcmp(p, "audio") == 0 ? "audio"
         : strcmp(p, "utility") == 0 ? "utility" : "chat";
}

static int act_models_set(Act *a) {
    char *agent = ftext(a, "agent"), *purpose = ftext(a, "purpose"), *list = fjson(a, "models");
    if (!agent_exists(a, agent)) return refuse(a, 404, "There is no agent called \"%s\".", agent ? agent : "");
    if (!purpose || (strcmp(purpose, "chat") != 0 && strcmp(purpose, "image") != 0 &&
                     strcmp(purpose, "audio") != 0 && strcmp(purpose, "utility") != 0))
        return refuse(a, 400, "The purpose must be chat, image, audio or utility.");
    if (!list || qi(a, "SELECT json_type(?1) = 'array'", list, NULL, NULL) != 1)
        return refuse(a, 400, "models must be a list of {model_id, reasoning_effort,"
                              " trim_threshold, prune_target, cache_lifetime} in order.");
    /* Media lists (image, audio) take only models with that capability and
     * no effort; chat and utility take any model, with an effort level. */
    int is_chat = strcmp(purpose, "chat") == 0;
    int is_media = strcmp(purpose, "image") == 0 || strcmp(purpose, "audio") == 0;
    if (is_chat && qi(a, "SELECT json_array_length(?1)", list, NULL, NULL) == 0)
        return refuse(a, 400, "An agent needs at least one chat model, or it cannot run. Add one"
                              " before removing the last.");
    if (qi(a, "SELECT 1 FROM json_each(?1) WHERE type <> 'object'"
              " OR json_type(value, '$.model_id') IS NOT 'text'"
              " OR EXISTS (SELECT 1 FROM json_each(value) k WHERE k.key NOT IN"
              "            ('model_id', 'reasoning_effort', 'trim_threshold', 'prune_target',"
              "             'cache_lifetime'))",
              list, NULL, NULL) == 1)
        return refuse(a, 400, "Each entry needs a model_id, and may have a reasoning_effort,"
                              " a trim_threshold, a prune_target and a cache_lifetime.");
    /* The trim numbers: tokens, or null for the default. An entry that does
     * not name one keeps what that model's row has now, so a list saved by a
     * page that does not show them does not reset them. */
    if (qi(a, "SELECT 1 FROM json_each(?1) j, json_each(j.value) k"
              " WHERE k.key IN ('trim_threshold', 'prune_target') AND k.type <> 'null'"
              "   AND (k.type <> 'integer' OR k.value < 1)", list, NULL, NULL) == 1)
        return refuse(a, 400, "trim_threshold and prune_target are token counts (whole numbers"
                              " above 0), or empty for the default.");
    if (qi(a, "SELECT 1 FROM json_each(?1) j, json_each(j.value) k"
              " WHERE k.key = 'cache_lifetime' AND k.type <> 'null'"
              "   AND (k.type <> 'integer' OR k.value < 1)", list, NULL, NULL) == 1)
        return refuse(a, 400, "cache_lifetime is a number of seconds (a whole number above 0),"
                              " or empty for the default.");
    if (!is_chat && qi(a, "SELECT 1 FROM json_each(?1) j, json_each(j.value) k"
                          " WHERE k.key IN ('trim_threshold', 'prune_target', 'cache_lifetime')"
                          "   AND k.type <> 'null'", list, NULL, NULL) == 1)
        return refuse(a, 400, "Trim numbers and the cache lifetime apply to the chat list only.");
    char *dup = qs(a, "SELECT value ->> 'model_id' FROM json_each(?1) GROUP BY value ->> 'model_id'"
                      " HAVING count(*) > 1", list, NULL, NULL);
    if (dup) return refuse(a, 400, "%s is in the list twice. Keep one.", dup);
    char *bad = qs(a, "SELECT value ->> 'model_id' FROM json_each(?1)"
                      " WHERE value ->> 'model_id' NOT IN (SELECT id FROM models)", list, NULL, NULL);
    if (bad) return refuse(a, 400, "There is no model called %s. Register it on its provider's page"
                                   " first.", bad);
    if (is_media && qi(a, "SELECT 1 FROM json_each(?1) WHERE value ->> 'reasoning_effort' IS NOT NULL",
                       list, NULL, NULL) == 1)
        return refuse(a, 400, "Reasoning effort applies to chat and utility models only.");
    bad = qs(a, "SELECT value ->> 'reasoning_effort' FROM json_each(?1)"
                " WHERE value ->> 'reasoning_effort' IS NOT NULL AND value ->> 'reasoning_effort'"
                " NOT IN ('off','minimal','low','medium','high')", list, NULL, NULL);
    if (bad) return refuse(a, 400, "\"%s\" is not an effort level: use off, minimal, low, medium or"
                                   " high, or leave it empty.", bad);
    if (is_media) {
        bad = qs(a, "SELECT j.value ->> 'model_id' FROM json_each(?1) j WHERE NOT EXISTS ("
                    " SELECT 1 FROM models m, json_each(CASE WHEN json_valid(m.capabilities)"
                    "   THEN m.capabilities ELSE '[]' END) c"
                    " WHERE m.id = j.value ->> 'model_id' AND c.value = ?2)", list, purpose, NULL);
        if (bad) return refuse(a, 400, "%s cannot be used for %s: its capabilities do not include %s."
                                       " If it can, add that capability on the model's page first.",
                               bad, purpose_word(purpose), purpose);
    }
    if (tx_begin(a) != 0) return busy(a);
    /* What is asked for, with the trim numbers an entry left out taken from
     * that model's current row. */
    list = qs(a, "SELECT json_group_array(json_object('model_id', j.value ->> 'model_id',"
                 " 'reasoning_effort', j.value ->> 'reasoning_effort',"
                 " 'trim_threshold', CASE WHEN json_type(j.value, '$.trim_threshold') IS NULL"
                 "   THEN c.trim_threshold ELSE j.value ->> 'trim_threshold' END,"
                 " 'prune_target', CASE WHEN json_type(j.value, '$.prune_target') IS NULL"
                 "   THEN c.prune_target ELSE j.value ->> 'prune_target' END,"
                 " 'cache_lifetime', CASE WHEN json_type(j.value, '$.cache_lifetime') IS NULL"
                 "   THEN c.cache_lifetime ELSE j.value ->> 'cache_lifetime' END) ORDER BY j.key)"
                 " FROM json_each(?1) j LEFT JOIN agent_models c ON c.agent_name=?2"
                 "  AND c.purpose=?3 AND c.model_id = j.value ->> 'model_id'",
              list, agent, purpose);
    if (!list) { tx_end(a, 0); return refuse(a, 500, "The list could not be saved."); }
    char *cur = qs(a, "SELECT json_group_array(json_object('model_id', model_id, 'reasoning_effort',"
                      " reasoning_effort, 'trim_threshold', trim_threshold, 'prune_target',"
                      " prune_target, 'cache_lifetime', cache_lifetime))"
                      " FROM (SELECT * FROM agent_models WHERE agent_name=?1"
                      " AND purpose=?2 ORDER BY pos)", agent, purpose, NULL);
    char *want = list;
    if (cur && want && strcmp(cur, want) == 0) {
        tx_end(a, 0);
        return done(a, NULL, "Nothing changed: that is already %s's %s list.", agent, purpose_word(purpose));
    }
    int ok = qx(a, "DELETE FROM agent_models WHERE agent_name=?1 AND purpose=?2",
                agent, purpose, NULL, NULL) >= 0;
    if (ok) {
        sqlite3_stmt *st = prep(a,
            "INSERT INTO agent_models(agent_name, purpose, model_id, pos, reasoning_effort,"
            " trim_threshold, prune_target, cache_lifetime)"
            " SELECT ?1, ?2, value ->> 'model_id', key, value ->> 'reasoning_effort',"
            " value ->> 'trim_threshold', value ->> 'prune_target', value ->> 'cache_lifetime'"
            " FROM json_each(?3)", agent, purpose, list, NULL);
        ok = st && sqlite3_step(st) == SQLITE_DONE;
        char err[256] = "";
        if (!ok) snprintf(err, sizeof(err), "%s", sqlite3_errmsg(a->db));
        sqlite3_finalize(st);
        if (!ok) {
            tx_end(a, 0);
            if (strstr(err, "capabilities do not include"))
                return refuse(a, 400, "One of these models cannot be used for %s: its capabilities"
                                      " do not include %s.", purpose_word(purpose), purpose);
            if (strstr(err, "trim_threshold is larger"))
                return refuse(a, 400, "A trim threshold is larger than its model's context"
                                      " window. Lower it, or leave it empty for half the window.");
            if (strstr(err, "cache_lifetime on an Anthropic"))
                return refuse(a, 400, "On an Anthropic model the cache lifetime is 300 (the"
                                      " 5-minute cache) or 3600 (the 1-hour cache, the default).");
            if (strstr(err, "CHECK constraint"))
                return refuse(a, 400, "A prune target must be below its trim threshold.");
            return refuse(a, 500, "The list could not be saved.");
        }
    }
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The list could not be saved.");
    long long n = qi(a, "SELECT json_array_length(?1)", list, NULL, NULL);
    if (n == 0)
        return done(a, NULL, is_media
                    ? "%s has no %s list now: the global pick by capability is used."
                    : "%s has no %s list now: scripts that ask for a utility model get the chat list.",
                    agent, purpose_word(purpose));
    return done(a, NULL, "Saved %s's %s list (%lld model%s). It applies from the next request.",
                agent, purpose_word(purpose), n, n == 1 ? "" : "s");
}

/* ── chats and sessions ──────────────────────────────────────────────────── */

typedef struct {
    int exists;
    long long session;      /* -1 = not routed */
    char *agent;            /* the pinned session's agent */
    char *label;            /* title, else the chat id */
} Chat;

static Chat chat_get(Act *a, const char *channel, const char *chat_id) {
    Chat c = { 0, -1, NULL, NULL };
    sqlite3_stmt *st = prep(a,
        "SELECT c.session_id, s.agent_name, COALESCE(c.title, c.chat_id) FROM channel_chats c"
        " LEFT JOIN sessions s ON s.id = c.session_id WHERE c.channel_name=?1 AND c.chat_id=?2",
        channel, chat_id, NULL, NULL);
    if (st && sqlite3_step(st) == SQLITE_ROW) {
        c.exists = 1;
        if (sqlite3_column_type(st, 0) != SQLITE_NULL) c.session = sqlite3_column_int64(st, 0);
        if (sqlite3_column_text(st, 1)) c.agent = keep(a, strdup((const char *)sqlite3_column_text(st, 1)));
        c.label = keep(a, strdup((const char *)sqlite3_column_text(st, 2)));
    }
    sqlite3_finalize(st);
    return c;
}

/* A chat that talks to another agent's session changes hands only when the
 * operator types the receiving agent's name: it is an authority change (the
 * chat's people get that agent and its grants), not a re-arrangement. 0 =
 * fine to go ahead; otherwise the refusal's status. */
static int handover_check(Act *a, const Chat *c, const char *agent) {
    if (c->session < 0 || !c->agent || strcmp(c->agent, agent) == 0) return 0;
    char *typed = ftext(a, "confirm_handover");
    if (typed && strcmp(typed, agent) == 0) return 0;
    char *extra = qs(a, "SELECT json_object('confirm', json_object('kind', 'handover',"
                        " 'from', ?1, 'to', ?2, 'type', ?2))", c->agent, agent, NULL);
    return refuse_x(a, 409, extra, "This chat talks to %s now (session #%lld). Going ahead hands it"
                    " to %s: its messages reach %s, with %s's grants. Type %s to confirm.",
                    c->agent, c->session, agent, agent, agent, agent);
}

static int chat_args(Act *a, char **channel, char **chat_id, Chat *c) {
    *channel = ftext(a, "channel");
    *chat_id = ftext(a, "chat_id");
    if (!*channel || !*chat_id) return refuse(a, 400, "Pick a channel and a chat.");
    *c = chat_get(a, *channel, *chat_id);
    if (!c->exists)
        return refuse(a, 404, "%s has no chat %s. A chat appears here once someone writes in it.",
                      *channel, *chat_id);
    return 0;
}

static int act_session_new_on_chat(Act *a) {
    char *agent = ftext(a, "agent"), *channel, *chat_id;
    Chat c;
    if (!agent_exists(a, agent)) return refuse(a, 404, "There is no agent called \"%s\".", agent ? agent : "");
    int s = chat_args(a, &channel, &chat_id, &c);
    if (s) return s;
    if ((s = handover_check(a, &c, agent))) return s;
    int64_t sid = channel_pin_new_session(a->db, channel, chat_id, agent, NULL);
    if (sid <= 0) return refuse(a, 500, "The new session could not be created.");
    char *extra = qs(a, "SELECT json_object('session_id', CAST(?1 AS INTEGER))", num(a, sid), NULL, NULL);
    if (c.session < 0)
        return done(a, extra, "%s now talks to %s in a new session, #%lld.", c.label, agent, (long long)sid);
    return done(a, extra, "%s now talks to %s in a new session, #%lld. Session #%lld (%s) is left"
                " behind with its history.", c.label, agent, (long long)sid, c.session,
                c.agent ? c.agent : "?");
}

static int act_session_move_to_chat(Act *a) {
    long long sid = 0;
    char *channel, *chat_id;
    Chat c;
    if (!fint(a, "session_id", &sid) || sid <= 0) return refuse(a, 400, "Say which session to move.");
    char *sidt = num(a, sid);
    char *agent = qs(a, "SELECT agent_name FROM sessions WHERE id=?1", sidt, NULL, NULL);
    if (!agent) return refuse(a, 404, "There is no session #%lld.", sid);
    if (qi(a, "SELECT COALESCE(parent_session_id, -1) > 0 FROM sessions WHERE id=?1", sidt, NULL, NULL) == 1)
        return refuse(a, 400, "Session #%lld belongs to a sub-agent's task. Only a top-level"
                              " session can talk to a chat.", sid);
    int s = chat_args(a, &channel, &chat_id, &c);
    if (s) return s;
    if (c.session == sid) return refuse(a, 409, "Session #%lld already talks to %s.", sid, c.label);
    if ((s = handover_check(a, &c, agent))) return s;
    char *old_ch = qs(a, "SELECT channel_name FROM channel_chats WHERE session_id=?1", sidt, NULL, NULL);
    char *old_chat = qs(a, "SELECT chat_id FROM channel_chats WHERE session_id=?1", sidt, NULL, NULL);
    if (channel_pin_session(a->db, channel, chat_id, sid) != 0)
        return refuse(a, 500, "The session could not be moved.");
    char msg[MSG_MAX];
    int n = snprintf(msg, sizeof(msg), "Session #%lld now talks to %s.", sid, c.label);
    if (c.session >= 0 && n > 0 && (size_t)n < sizeof(msg))
        n += snprintf(msg + n, sizeof(msg) - (size_t)n, " Session #%lld (%s) is left behind with"
                      " its history.", c.session, c.agent ? c.agent : "?");
    if (old_ch && old_chat && n > 0 && (size_t)n < sizeof(msg)) {
        long long fresh = qi(a, "SELECT session_id FROM channel_chats WHERE channel_name=?1"
                                " AND chat_id=?2", old_ch, old_chat, NULL);
        Chat oc = chat_get(a, old_ch, old_chat);
        snprintf(msg + n, sizeof(msg) - (size_t)n, " Its old chat, %s, goes on with a fresh"
                 " session for %s (#%lld).", oc.label ? oc.label : old_chat, agent, fresh);
    }
    return answer(a, 200, "ok", msg, NULL);
}

static int act_chat_settings(Act *a) {
    char *channel, *chat_id;
    Chat c;
    int s = chat_args(a, &channel, &chat_id, &c);
    if (s) return s;
    char *mode = ftext(a, "delivery_mode"), *filter = NULL;
    int set_filter = fhas(a, "tool_filter"), set_guidance = fhas(a, "guidance");
    if (fhas(a, "delivery_mode") && (!mode || (strcmp(mode, "auto") && strcmp(mode, "explicit"))))
        return refuse(a, 400, "Replies must be auto (the final message posts) or explicit (only"
                              " what the agent sends with channel_send).");
    if (set_filter) {
        char *t = ftext(a, "tool_filter");
        if (t && strcmp(t, "unrestricted") == 0) filter = NULL;
        else {
            filter = fjson(a, "tool_filter");
            if (!tools_list_ok(a, filter, "The tool filter", &s)) return s;
        }
    }
    char *guidance = set_guidance ? fraw_text(a, "guidance") : NULL;
    if (set_guidance && !fis(a, "guidance", "text") && !fis(a, "guidance", "null"))
        return refuse(a, 400, "Guidance is text.");
    if (!mode && !set_filter && !set_guidance) return refuse(a, 400, "Nothing to change.");
    if (tx_begin(a) != 0) return busy(a);
    int ok = !mode || qx(a, "UPDATE channel_chats SET delivery_mode=?3 WHERE channel_name=?1"
                            " AND chat_id=?2", channel, chat_id, mode, NULL) >= 0;
    if (ok && set_filter)
        ok = qx(a, "UPDATE channel_chats SET tool_filter = ?3 WHERE channel_name=?1 AND chat_id=?2",
                channel, chat_id, filter, NULL) >= 0;
    if (ok && set_guidance)
        ok = qx(a, "UPDATE channel_chats SET guidance = NULLIF(?3, '') WHERE channel_name=?1"
                   " AND chat_id=?2", channel, chat_id, guidance, NULL) >= 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The chat's settings could not be saved.");
    return done(a, NULL, "Saved. %s's sessions read these on their next turn.", c.label);
}

static int act_chat_unroute(Act *a) {
    char *channel, *chat_id;
    Chat c;
    int s = chat_args(a, &channel, &chat_id, &c);
    if (s) return s;
    if (c.session < 0) return refuse(a, 409, "%s is not routed.", c.label);
    if (tx_begin(a) != 0) return busy(a);
    /* `cclaw route rm`: the row stays (its sessions and schedules belong to
     * it); the next message meets the gate, and the operator is told again. */
    int ok = qx(a, "UPDATE channel_chats SET session_id=NULL, notified_at=NULL"
                   " WHERE channel_name=?1 AND chat_id=?2", channel, chat_id, NULL, NULL) == 1;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The route could not be removed.");
    char *da = qs(a, "SELECT default_agent FROM channels WHERE name=?1", channel, NULL, NULL);
    if (da)
        return done(a, NULL, "%s is not routed now. Session #%lld keeps its history. The channel's"
                    " default agent, %s, takes the chat's next message in a new session.",
                    c.label, c.session, da);
    return done(a, NULL, "%s is not routed now. Session #%lld keeps its history. The chat's next"
                " message is dropped, and you are told once.", c.label, c.session);
}

static int act_channel_defaults(Act *a) {
    char *channel = ftext(a, "channel");
    if (!channel || qi(a, "SELECT 1 FROM channels WHERE name=?1", channel, NULL, NULL) != 1)
        return refuse(a, 404, "There is no channel called \"%s\".", channel ? channel : "");
    int set_agent = fhas(a, "default_agent"), set_filter = fhas(a, "default_tool_filter"), s;
    char *agent = ftext(a, "default_agent"), *filter = NULL;
    if (set_agent && agent && agent[0] && !agent_exists(a, agent))
        return refuse(a, 404, "There is no agent called \"%s\".", agent);
    if (agent && !agent[0]) agent = NULL;
    if (set_filter) {
        char *t = ftext(a, "default_tool_filter");
        if (!(t && strcmp(t, "unrestricted") == 0)) {
            filter = fjson(a, "default_tool_filter");
            if (!tools_list_ok(a, filter, "The default tool filter", &s)) return s;
        }
    }
    if (!set_agent && !set_filter) return refuse(a, 400, "Nothing to change.");
    if (tx_begin(a) != 0) return busy(a);
    int ok = 1;
    if (set_agent)
        ok = qx(a, "UPDATE channels SET default_agent=?2 WHERE name=?1", channel, agent, NULL, NULL) == 1;
    if (ok && set_filter)
        ok = qx(a, "UPDATE channels SET default_tool_filter=?2 WHERE name=?1",
                channel, filter, NULL, NULL) == 1;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The channel's defaults could not be saved.");
    if (set_agent && agent)
        return done(a, NULL, "Chats nobody routed on %s now get a new session with %s.", channel, agent);
    if (set_agent)
        return done(a, NULL, "Chats nobody routed on %s are dropped now, and you are told once per chat.",
                    channel);
    return done(a, NULL, "Saved. New chats on %s start with this filter; existing chats keep theirs.",
                channel);
}

/* ── providers and models ────────────────────────────────────────────────── */

/* <NAME>_API_KEY, anything not [A-Z0-9] folded to '_' (admin_set_key's rule). */
static char *key_name_for(Act *a, const char *provider) {
    char b[96];
    size_t j = 0;
    for (const char *p = provider; *p && j < sizeof(b) - 9; p++) {
        unsigned char ch = (unsigned char)*p;
        b[j++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32)
               : ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) ? (char)ch : '_';
    }
    memcpy(b + j, "_API_KEY", 9);
    return keep(a, strdup(b));
}

/* Fetch the catalog with the lock released: a provider can take as long as
 * llm_probe_timeout allows, and an approval click must not wait for it. */
static int fetch_unlocked(Act *a, const char *url, const char *type, const char *key,
                          char **body, char *err, size_t errlen) {
    int timeout = models_cache_timeout(a->db);
    unlock(a);
    int rc = models_cache_fetch(url, type, key, timeout, body, err, errlen);
    lock(a);
    return rc;
}

static int act_provider_add(Act *a) {
    lock(a);
    char *name = ftext(a, "name"), *url = ftext(a, "base_url"), *type = ftext(a, "endpoint_type");
    char *key = fraw_text(a, "api_key");
    if (!name || !is_valid_name(name) || !((name[0] >= 'a' && name[0] <= 'z') ||
                                           (name[0] >= 'A' && name[0] <= 'Z')))
        return refuse(a, 400, "The name must start with a letter and use letters, digits, - and _"
                              " only (for example cerebras).");
    if (qi(a, "SELECT 1 FROM providers WHERE name=?1", name, NULL, NULL) == 1)
        return refuse(a, 409, "There is already a provider called %s.", name);
    if (!url_ok(url))
        return refuse(a, 400, "The API endpoint must be a URL starting with https:// (or http:// for"
                              " a gateway on your network), such as https://api.cerebras.ai/v1.");
    if (!wire_ok(type))
        return refuse(a, 400, "The wire format must be openai (OpenAI-compatible), gemini or anthropic.");
    char *key_name = key && key[0] ? key_name_for(a, name) : NULL;
    if (key_name && qi(a, "SELECT 1 FROM secrets WHERE name=?1", key_name, NULL, NULL) == 1)
        return refuse(a, 409, "A secret called %s already exists, and this provider's key would be"
                              " stored under that name. Remove it on the Secrets page, or choose"
                              " another provider name.", key_name);
    if (key_name && !db_secret_key_loaded())
        return refuse(a, 503, "The secret store's key is not loaded in this process, so the key"
                              " cannot be stored.");

    /* The key is checked before anything is written: a provider that cannot
     * list its models leaves nothing behind. */
    char err[512] = "", *body = NULL;
    if (fetch_unlocked(a, url, type, key, &body, err, sizeof(err)) != 0)
        return refuse(a, 400, "Nothing was saved: %s. Check the endpoint, the wire format and the"
                              " key, then try again.", err);
    keep(a, body);
    if (tx_begin(a) != 0) return busy(a);
    int ok = qi(a, "SELECT 1 FROM providers WHERE name=?1", name, NULL, NULL) != 1;
    if (ok && key_name) ok = db_secret_set(a->db, key_name, key, "operator", "system") == 0;
    if (ok)
        ok = qx(a, "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, priority)"
                   " VALUES(?1, ?2, ?3, COALESCE(?4, ''),"
                   " (SELECT COALESCE(MAX(priority), 0) + 1 FROM providers))",
                name, url, type, key_name) == 1;
    if (ok) ok = models_cache_ingest(a->db, name, body, err, sizeof(err)) == 0;
    if (tx_end(a, ok) != 0)
        return refuse(a, 400, "Nothing was saved: %s.", err[0] ? err : "the provider could not be written");
    long long n = qi(a, "SELECT count(*) FROM models_cache WHERE provider_name=?1", name, NULL, NULL);
    char *extra = qs(a, "SELECT json_object('provider', ?1, 'models_listed', CAST(?2 AS INTEGER))",
                     name, num(a, n), NULL);
    if (key_name)
        return done(a, extra, "Saved %s (%s). Its key is stored encrypted as the system secret %s."
                    " The provider lists %lld models: import the ones you want.", name,
                    wire_name(type), key_name, n);
    return done(a, extra, "Saved %s (%s), with no key. The provider lists %lld models: import the"
                " ones you want.", name, wire_name(type), n);
}

static int act_provider_edit(Act *a) {
    lock(a);
    char *name = ftext(a, "name");
    if (!name || qi(a, "SELECT 1 FROM providers WHERE name=?1", name, NULL, NULL) != 1)
        return refuse(a, 404, "There is no provider called \"%s\".", name ? name : "");
    char *url = ftext(a, "base_url"), *type = ftext(a, "endpoint_type");
    char *key = fraw_text(a, "api_key"), *extra = NULL;
    int set_extra = fhas(a, "request_extra");
    if (fhas(a, "base_url") && !url_ok(url))
        return refuse(a, 400, "The API endpoint must be a URL starting with https:// or http://.");
    if (fhas(a, "endpoint_type") && !wire_ok(type))
        return refuse(a, 400, "The wire format must be openai (OpenAI-compatible), gemini or anthropic.");
    if (fhas(a, "api_key") && (!key || !key[0]))
        return refuse(a, 400, "The new key is empty. Leave the field out to keep the current key.");
    if (set_extra) {
        if (fis(a, "request_extra", "object")) extra = fjson(a, "request_extra");
        else if (fis(a, "request_extra", "text")) {
            extra = ftext(a, "request_extra");
            if (extra && !extra[0]) extra = NULL;
            else if (qi(a, "SELECT json_valid(?1) AND json_type(?1) = 'object'", extra, NULL, NULL) != 1)
                return refuse(a, 400, "request_extra must be a JSON object, such as"
                                      " {\"provider\":{\"ignore\":[\"X\"]}}, or empty.");
        } else if (!fis(a, "request_extra", "null"))
            return refuse(a, 400, "request_extra must be a JSON object, or empty.");
        if (extra) extra = qs(a, "SELECT json(?1)", extra, NULL, NULL);
    }
    if (!url && !type && !key && !set_extra) return refuse(a, 400, "Nothing to change.");
    if (key && !db_secret_key_loaded())
        return refuse(a, 503, "The secret store's key is not loaded in this process, so the key"
                              " cannot be stored.");
    /* A change to where or how requests go is checked first, like a new
     * provider: if the provider cannot list its models with it, nothing is
     * saved. */
    if (url || type || key) {
        char *cur_url = qs(a, "SELECT base_url FROM providers WHERE name=?1", name, NULL, NULL);
        char *cur_type = qs(a, "SELECT endpoint_type FROM providers WHERE name=?1", name, NULL, NULL);
        char *env = qs(a, "SELECT api_key_env FROM providers WHERE name=?1", name, NULL, NULL);
        char *use_key = key ? key : keep(a, models_cache_key(a->db, env));
        char err[512] = "", *body = NULL;
        if (fetch_unlocked(a, url ? url : cur_url, type ? type : cur_type, use_key,
                           &body, err, sizeof(err)) != 0)
            return refuse(a, 400, "Nothing was saved: %s.", err);
        free(body);
    }
    if (tx_begin(a) != 0) return busy(a);
    int ok = 1;
    if (url) ok = qx(a, "UPDATE providers SET base_url=?2 WHERE name=?1", name, url, NULL, NULL) == 1;
    if (ok && type)
        ok = qx(a, "UPDATE providers SET endpoint_type=?2 WHERE name=?1", name, type, NULL, NULL) == 1;
    if (ok && set_extra)
        ok = qx(a, "UPDATE providers SET request_extra=?2 WHERE name=?1", name, extra, NULL, NULL) == 1;
    if (ok && key) ok = admin_set_key(a->db, name, key) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The provider could not be saved.");
    char *env = qs(a, "SELECT api_key_env FROM providers WHERE name=?1", name, NULL, NULL);
    return done(a, NULL, "Saved %s.%s%s%s It applies from the next request.", name,
                key ? " The new key is stored as the system secret " : "", key && env ? env : "",
                key ? "." : "");
}

static int act_provider_refresh_models(Act *a) {
    lock(a);
    char *name = ftext(a, "name");
    if (!name || qi(a, "SELECT 1 FROM providers WHERE name=?1", name, NULL, NULL) != 1)
        return refuse(a, 404, "There is no provider called \"%s\".", name ? name : "");
    char *url = qs(a, "SELECT base_url FROM providers WHERE name=?1", name, NULL, NULL);
    char *type = qs(a, "SELECT endpoint_type FROM providers WHERE name=?1", name, NULL, NULL);
    char *env = qs(a, "SELECT api_key_env FROM providers WHERE name=?1", name, NULL, NULL);
    char *key = keep(a, models_cache_key(a->db, env));
    char err[512] = "", *body = NULL;
    if (fetch_unlocked(a, url, type, key, &body, err, sizeof(err)) != 0 ||
        models_cache_ingest(a->db, name, keep(a, body), err, sizeof(err)) != 0) {
        long long at = qi(a, "SELECT MAX(synced_at) FROM models_cache WHERE provider_name=?1",
                          name, NULL, NULL);
        return refuse(a, 502, "Could not refresh %s's list: %s.%s", name, err,
                      at > 0 ? " The previous list is kept." : "");
    }
    long long n = qi(a, "SELECT count(*) FROM models_cache WHERE provider_name=?1", name, NULL, NULL);
    char *extra = qs(a, "SELECT json_object('models_listed', CAST(?1 AS INTEGER), 'synced_at',"
                        " (SELECT MAX(synced_at) FROM models_cache WHERE provider_name=?2))",
                     num(a, n), name, NULL);
    return done(a, extra, "%s lists %lld models.", name, n);
}

static int act_provider_remove(Act *a) {
    char *name = ftext(a, "name");
    if (!name || qi(a, "SELECT 1 FROM providers WHERE name=?1", name, NULL, NULL) != 1)
        return refuse(a, 404, "There is no provider called \"%s\".", name ? name : "");
    long long n = qi(a, "SELECT count(*) FROM models WHERE provider_name=?1", name, NULL, NULL);
    if (n > 0) {
        char *ids = qs(a, "SELECT group_concat(id, ', ') FROM (SELECT id FROM models"
                          " WHERE provider_name=?1 ORDER BY id LIMIT 5)", name, NULL, NULL);
        return refuse(a, 409, "%s still has %lld model%s registered (%s%s). Remove them first.",
                      name, n, n == 1 ? "" : "s", ids ? ids : "", n > 5 ? ", …" : "");
    }
    char *env = qs(a, "SELECT NULLIF(api_key_env, '') FROM providers WHERE name=?1", name, NULL, NULL);
    if (tx_begin(a) != 0) return busy(a);
    int ok = qx(a, "DELETE FROM models_cache WHERE provider_name=?1", name, NULL, NULL, NULL) >= 0 &&
             qx(a, "DELETE FROM providers WHERE name=?1", name, NULL, NULL, NULL) == 1;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The provider could not be removed.");
    if (env && qi(a, "SELECT 1 FROM secrets WHERE name=?1", env, NULL, NULL) == 1)
        return done(a, NULL, "Removed %s. Its key, the secret %s, is still stored: remove it on the"
                    " Secrets page if nothing else uses it.", name, env);
    return done(a, NULL, "Removed %s.", name);
}

/* A capabilities array: non-empty lowercase words. */
static int caps_ok(Act *a, const char *json) {
    return json && qi(a, "SELECT json_type(?1) = 'array' AND NOT EXISTS (SELECT 1 FROM json_each(?1)"
                         " WHERE type <> 'text' OR value = '' OR value GLOB '*[^a-z_]*')",
                      json, NULL, NULL) == 1;
}

/* models.effort_map's shape (schema.sql): {"format": ..., "levels": {...}}. */
static int effort_map_ok(Act *a, const char *json) {
    return qi(a,
        "SELECT json_valid(?1) AND json_type(?1) = 'object'"
        " AND NOT EXISTS (SELECT 1 FROM json_each(?1) WHERE key NOT IN ('format', 'levels'))"
        " AND json_extract(?1, '$.format') IN ('openrouter','openai','deepseek','gemini-level',"
        "     'gemini-budget')"
        " AND COALESCE(json_type(?1, '$.levels'), 'object') = 'object'"
        " AND NOT EXISTS (SELECT 1 FROM json_each(?1, '$.levels') WHERE key NOT IN"
        "     ('off','minimal','low','medium','high') OR type NOT IN ('text','integer','null'))",
        json, NULL, NULL) == 1;
}

/* A positive integer field, or null/empty to clear. 1 = set (*v, *clear). */
static int count_field(Act *a, const char *key, long long *v, int *clear, int *status) {
    *clear = 0;
    if (!fhas(a, key)) return 0;
    if (fis(a, key, "null") || (fis(a, key, "text") && !ftext(a, key)[0])) { *clear = 1; return 1; }
    if (!fint(a, key, v) || *v <= 0) {
        *status = refuse(a, 400, "%s must be a whole number of tokens, or empty for unknown.", key);
        return -1;
    }
    return 1;
}

static int act_model_import(Act *a) {
    char *prov = ftext(a, "provider"), *model = ftext(a, "model"), *caps = NULL;
    long long ctx = 0, mot = 0;
    int clear, s = 0;
    if (!prov || qi(a, "SELECT 1 FROM providers WHERE name=?1", prov, NULL, NULL) != 1)
        return refuse(a, 404, "There is no provider called \"%s\".", prov ? prov : "");
    if (!model || !model[0]) return refuse(a, 400, "Say which model: its name as the provider spells it.");
    for (const char *p = model; *p; p++)
        if (*p <= ' ') return refuse(a, 400, "A model name has no spaces.");
    if (count_field(a, "context_window", &ctx, &clear, &s) < 0) return s;
    if (count_field(a, "max_output_tokens", &mot, &clear, &s) < 0) return s;
    if (fhas(a, "capabilities")) {
        caps = fjson(a, "capabilities");
        if (!caps_ok(a, caps))
            return refuse(a, 400, "Capabilities are a list of words, such as [\"text\",\"image\"].");
    }
    char id[512];
    snprintf(id, sizeof(id), "%s/%s", prov, model);
    if (qi(a, "SELECT 1 FROM models WHERE id=?1", id, NULL, NULL) == 1)
        return refuse(a, 409, "%s is already registered.", id);
    if (tx_begin(a) != 0) return busy(a);
    sqlite3_stmt *st;
    int ok = sqlite3_prepare_v2(a->db,
        "INSERT INTO models(id, provider_name, model, capabilities, context_window, max_output_tokens)"
        " VALUES(?1, ?2, ?3, COALESCE(json(?4), '[]'), ?5, ?6)", -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, prov, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, model, -1, SQLITE_STATIC);
        if (caps) sqlite3_bind_text(st, 4, caps, -1, SQLITE_STATIC);
        if (ctx) sqlite3_bind_int64(st, 5, ctx);
        if (mot) sqlite3_bind_int64(st, 6, mot);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The model could not be registered.");
    char *extra = qs(a, "SELECT json_object('model_id', ?1)", id, NULL, NULL);
    return done(a, extra, "Registered %s. Put it on an agent's model list to use it.", id);
}

static int act_model_edit(Act *a) {
    char *id = ftext(a, "id"), *caps = NULL, *emap = NULL;
    long long ctx = 0, mot = 0;
    int cctx = 0, cmot = 0, s = 0;
    if (!id || qi(a, "SELECT 1 FROM models WHERE id=?1", id, NULL, NULL) != 1)
        return refuse(a, 404, "There is no model called \"%s\".", id ? id : "");
    int set_ctx = count_field(a, "context_window", &ctx, &cctx, &s);
    if (set_ctx < 0) return s;
    int set_mot = count_field(a, "max_output_tokens", &mot, &cmot, &s);
    if (set_mot < 0) return s;
    int set_caps = fhas(a, "capabilities"), set_emap = fhas(a, "effort_map");
    int set_enabled = fhas(a, "enabled"), enabled = fbool(a, "enabled", 1);
    if (set_caps) {
        caps = fjson(a, "capabilities");
        if (!caps_ok(a, caps))
            return refuse(a, 400, "Capabilities are a list of words, such as [\"text\",\"image\"].");
        /* The capability trigger guards inserts into a media list; taking a
         * capability away from a listed model is refused here instead. */
        char *who = qs(a, "SELECT group_concat(agent_name || ' (' || purpose || ')', ', ')"
                          " FROM agent_models WHERE model_id=?1 AND purpose IN ('image','audio')"
                          " AND purpose NOT IN (SELECT value FROM json_each(?2))", id, caps, NULL);
        if (who) return refuse(a, 409, "%s is on media lists that need the capability you are"
                                       " removing: %s. Take it off those lists first.", id, who);
    }
    if (set_emap) {
        if (fis(a, "effort_map", "object")) emap = fjson(a, "effort_map");
        else if (fis(a, "effort_map", "text")) emap = ftext(a, "effort_map");
        if (emap && !emap[0]) emap = NULL;
        if (emap && !effort_map_ok(a, emap))
            return refuse(a, 400, "The effort map must look like {\"format\": \"openai\", \"levels\":"
                                  " {\"low\": \"low\", \"high\": \"high\"}}; format is one of"
                                  " openrouter, openai, deepseek, gemini-level, gemini-budget, and"
                                  " levels are off, minimal, low, medium, high. Empty means the"
                                  " endpoint's default.");
        if (emap) emap = qs(a, "SELECT json(?1)", emap, NULL, NULL);
    }
    if (!set_ctx && !set_mot && !set_caps && !set_emap && !set_enabled)
        return refuse(a, 400, "Nothing to change.");
    if (tx_begin(a) != 0) return busy(a);
    int ok = 1;
    if (set_ctx) ok = qx(a, "UPDATE models SET context_window=CAST(?2 AS INTEGER) WHERE id=?1",
                         id, cctx ? NULL : num(a, ctx), NULL, NULL) == 1;
    if (ok && set_mot) ok = qx(a, "UPDATE models SET max_output_tokens=CAST(?2 AS INTEGER) WHERE id=?1",
                               id, cmot ? NULL : num(a, mot), NULL, NULL) == 1;
    if (ok && set_caps) ok = qx(a, "UPDATE models SET capabilities=json(?2) WHERE id=?1",
                                id, caps, NULL, NULL) == 1;
    if (ok && set_emap) ok = qx(a, "UPDATE models SET effort_map=?2 WHERE id=?1",
                                id, emap, NULL, NULL) == 1;
    if (ok && set_enabled)
        ok = qx(a, "UPDATE models SET status = CASE WHEN ?2 = '1' THEN"
                   " (CASE WHEN status = 'disabled' THEN 'healthy' ELSE status END)"
                   " ELSE 'disabled' END WHERE id=?1", id, enabled ? "1" : "0", NULL, NULL) == 1;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The model could not be saved.");
    /* Disabling the only chat model an agent has stops that agent. */
    char *stuck = set_enabled && !enabled ? qs(a,
        "SELECT group_concat(agent_name, ', ') FROM (SELECT DISTINCT am.agent_name FROM agent_models am"
        " WHERE am.purpose = 'chat' AND am.model_id = ?1 AND NOT EXISTS (SELECT 1 FROM agent_models o"
        "  JOIN models m ON m.id = o.model_id WHERE o.agent_name = am.agent_name AND o.purpose = 'chat'"
        "  AND m.status <> 'disabled'))", id, NULL, NULL) : NULL;
    if (stuck)
        return done(a, NULL, "Saved %s. Warning: it was the only enabled chat model of %s, which"
                    " cannot run until it is enabled again or another model is added.", id, stuck);
    return done(a, NULL, "Saved %s. It applies from the next request.", id);
}

static int act_model_remove(Act *a) {
    char *id = ftext(a, "id");
    if (!id || qi(a, "SELECT 1 FROM models WHERE id=?1", id, NULL, NULL) != 1)
        return refuse(a, 404, "There is no model called \"%s\".", id ? id : "");
    char *who = qs(a, "SELECT group_concat(agent_name || ' (' || purpose || ')', ', ')"
                      " FROM agent_models WHERE model_id=?1", id, NULL, NULL);
    if (who) return refuse(a, 409, "%s is on these model lists: %s. Take it off them first (Assign"
                                   " models, on each agent's page), or disable it instead.", id, who);
    if (tx_begin(a) != 0) return busy(a);
    int ok = qx(a, "DELETE FROM models WHERE id=?1", id, NULL, NULL, NULL) == 1;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The model could not be removed.");
    return done(a, NULL, "Removed %s.", id);
}

/* ── memory ──────────────────────────────────────────────────────────────── */

static long long block_arg(Act *a, int *status) {
    long long id = 0;
    if (!fint(a, "block_id", &id) || id <= 0) { *status = refuse(a, 400, "Say which block."); return -1; }
    if (qi(a, "SELECT 1 FROM memory_blocks WHERE id=?1", num(a, id), NULL, NULL) != 1) {
        *status = refuse(a, 404, "There is no memory block #%lld. It may have been deleted.", id);
        return -1;
    }
    return id;
}

/* The label clash memory_block_attach refuses, as a sentence. */
static int label_clash(Act *a, const char *agent, long long block) {
    char *other = qs(a, "SELECT b2.label || ' (#' || b2.id || ')' FROM agent_memory_blocks a2"
                        " JOIN memory_blocks b2 ON b2.id = a2.block_id JOIN memory_blocks b ON b.id = ?2"
                        " WHERE a2.agent_name = ?1 AND b2.label = b.label AND b2.id <> b.id",
                     agent, num(a, block), NULL);
    if (!other) return 0;
    return refuse(a, 409, "%s already has a block called %s. An agent addresses blocks by label,"
                          " so it cannot hold two with the same one. Rename or detach that one first.",
                  agent, other);
}

static int act_memory_block_create(Act *a) {
    char *label = ftext(a, "label"), *desc = ftext(a, "description");
    char *placement = ftext(a, "placement"), *agent = ftext(a, "agent");
    long long limit = 5000;
    int ro = fbool(a, "read_only", 0);
    if (!label || !is_valid_name(label))
        return refuse(a, 400, "A label uses letters, digits, - and _ only, up to 63 characters,"
                              " no spaces (for example team-notes).");
    if (placement && strcmp(placement, "system") && strcmp(placement, "context"))
        return refuse(a, 400, "Placement must be system (in the system prompt) or context (in"
                              " each turn's context).");
    if (fhas(a, "char_limit") && (!fint(a, "char_limit", &limit) || limit <= 0))
        return refuse(a, 400, "The character limit must be a positive whole number.");
    if (agent && !agent[0]) agent = NULL;
    if (agent && !agent_exists(a, agent))
        return refuse(a, 404, "There is no agent called \"%s\".", agent);
    if (agent && qi(a, "SELECT 1 FROM agent_memory_blocks x JOIN memory_blocks b ON b.id = x.block_id"
                       " WHERE x.agent_name=?1 AND b.label=?2", agent, label, NULL) == 1)
        return refuse(a, 409, "%s already has a block called %s.", agent, label);
    if (tx_begin(a) != 0) return busy(a);
    /* The operator made it: created_by stays NULL (it is history, not a
     * permission). Attached read-write to the agent it was made for. */
    int64_t id = memory_block_create(a->db, NULL, label, desc ? desc : "", (int)limit,
                                     placement ? placement : "context");
    int ok = id > 0;
    if (ok && ro) ok = memory_block_set_limits(a->db, id, (int)limit, 1) == 0;
    if (ok && agent) ok = memory_block_attach(a->db, agent, id, 0) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The block could not be created.");
    char *extra = qs(a, "SELECT json_object('block_id', CAST(?1 AS INTEGER))", num(a, id), NULL, NULL);
    if (agent)
        return done(a, extra, "Created %s (#%lld), attached to %s, who can read and write it.",
                    label, (long long)id, agent);
    return done(a, extra, "Created %s (#%lld). No agent sees it until you attach it.", label, (long long)id);
}

static int act_memory_block_edit(Act *a) {
    int s = 0;
    long long id = block_arg(a, &s);
    if (id < 0) return s;
    char *desc = fhas(a, "description") ? fraw_text(a, "description") : NULL;
    char *placement = ftext(a, "placement");
    long long limit = qi(a, "SELECT char_limit FROM memory_blocks WHERE id=?1", num(a, id), NULL, NULL);
    int ro = (int)qi(a, "SELECT read_only FROM memory_blocks WHERE id=?1", num(a, id), NULL, NULL);
    if (fhas(a, "description") && !desc && !fis(a, "description", "null"))
        return refuse(a, 400, "The description is text.");
    if (fis(a, "description", "null")) desc = keep(a, strdup(""));
    if (placement && strcmp(placement, "system") && strcmp(placement, "context"))
        return refuse(a, 400, "Placement must be system or context.");
    if (fhas(a, "char_limit") && (!fint(a, "char_limit", &limit) || limit <= 0))
        return refuse(a, 400, "The character limit must be a positive whole number.");
    ro = fbool(a, "read_only", ro);
    if (tx_begin(a) != 0) return busy(a);
    int ok = (!desc && !placement) || memory_block_update(a->db, id, desc, placement) == 0;
    if (ok) ok = memory_block_set_limits(a->db, id, (int)limit, ro) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The block could not be saved.");
    return done(a, NULL, "Saved. A context block shows at each agent's next turn, a system block at"
                " its next session.");
}

static int act_memory_block_delete(Act *a) {
    int s = 0;
    long long id = block_arg(a, &s);
    if (id < 0) return s;
    char *ids = num(a, id);
    char *label = qs(a, "SELECT label FROM memory_blocks WHERE id=?1", ids, NULL, NULL);
    char *who = qs(a, "SELECT group_concat(agent_name, ', ') FROM agent_memory_blocks WHERE block_id=?1",
                   ids, NULL, NULL);
    long long notes = qi(a, "SELECT count(*) FROM memory_notes WHERE block_id=?1", ids, NULL, NULL);
    if (tx_begin(a) != 0) return busy(a);
    int ok = memory_block_delete(a->db, id) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The block could not be deleted.");
    return done(a, NULL, "Deleted %s with its %lld note%s.%s%s", label, notes, notes == 1 ? "" : "s",
                who ? " It was detached from " : "", who ? who : "");
}

static int act_memory_attach(Act *a) {
    int s = 0;
    long long id = block_arg(a, &s);
    if (id < 0) return s;
    char *agent = ftext(a, "agent");
    int ro = fbool(a, "read_only", 1);
    if (!agent_exists(a, agent)) return refuse(a, 404, "There is no agent called \"%s\".", agent ? agent : "");
    if ((s = label_clash(a, agent, id))) return s;
    long long had = qi(a, "SELECT read_only FROM agent_memory_blocks WHERE agent_name=?1 AND block_id=?2",
                       agent, num(a, id), NULL);
    if (had == ro) return refuse(a, 409, "%s already has this block, %s.", agent,
                                 ro ? "read only" : "readable and writable");
    if (tx_begin(a) != 0) return busy(a);
    int ok = memory_block_attach(a->db, agent, id, ro) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The block could not be attached.");
    long long others = qi(a, "SELECT count(*) FROM agent_memory_blocks WHERE block_id=?1 AND agent_name<>?2",
                          num(a, id), agent, NULL);
    return done(a, NULL, "%s %s this block%s.%s", agent, ro ? "can now read" : "can now read and write",
                had < 0 ? "" : " (changed)",
                !ro && others > 0 ? " It is shared, so what this agent writes reaches the others'"
                                    " prompts." : "");
}

static int act_memory_detach(Act *a) {
    int s = 0;
    long long id = block_arg(a, &s);
    if (id < 0) return s;
    char *agent = ftext(a, "agent");
    if (!agent || qi(a, "SELECT 1 FROM agent_memory_blocks WHERE agent_name=?1 AND block_id=?2",
                     agent, num(a, id), NULL) != 1)
        return refuse(a, 404, "%s does not have this block.", agent ? agent : "That agent");
    if (tx_begin(a) != 0) return busy(a);
    int ok = memory_block_detach(a->db, agent, id) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The block could not be detached.");
    return done(a, NULL, "%s no longer sees this block. The block and its notes are kept.", agent);
}

static int act_memory_note(Act *a, int which) {   /* 0 add, 1 edit, 2 delete */
    int s = 0;
    long long id = block_arg(a, &s), note = 0;
    if (id < 0) return s;
    char *text = which < 2 ? fraw_text(a, "text") : NULL;
    if (which < 2 && (!text || !text[strspn(text, " \t\r\n")]))
        return refuse(a, 400, "The note is empty. Write something, or delete the note instead.");
    if (which > 0 && (!fint(a, "note_id", &note) || note <= 0)) return refuse(a, 400, "Say which note.");
    if (tx_begin(a) != 0) return busy(a);
    int ok;
    if (which == 0) ok = memory_note_add(a->db, id, text) > 0;
    else if (which == 1) ok = memory_note_set(a->db, id, note, text) == 0;
    else ok = memory_note_delete(a->db, id, note) == 0;
    if (tx_end(a, ok) != 0) {
        if (which > 0) return refuse(a, 404, "Block #%lld has no note #%lld.", id, note);
        return refuse(a, 500, "The note could not be saved.");
    }
    long long used = qi(a, "SELECT COALESCE(SUM(length(text)), 0) FROM memory_notes WHERE block_id=?1",
                        num(a, id), NULL, NULL);
    long long limit = qi(a, "SELECT char_limit FROM memory_blocks WHERE id=?1", num(a, id), NULL, NULL);
    const char *what = which == 0 ? "Added." : which == 1 ? "Saved." : "Deleted.";
    if (used > limit)
        return done(a, NULL, "%s The notes now hold %lld characters, over the block's limit of %lld:"
                    " agents cannot add to it until it is shorter.", what, used, limit);
    return done(a, NULL, "%s", what);
}

static int act_memory_note_add(Act *a) { return act_memory_note(a, 0); }
static int act_memory_note_edit(Act *a) { return act_memory_note(a, 1); }
static int act_memory_note_delete(Act *a) { return act_memory_note(a, 2); }

/* ── approvals ───────────────────────────────────────────────────────────── */

static int act_approval_decide(Act *a) {
    long long id = 0;
    char *decision = ftext(a, "decision");
    if (!fint(a, "approval_id", &id) || id <= 0) return refuse(a, 400, "Say which approval.");
    char *ids = num(a, id);
    char *state = qs(a, "SELECT state FROM approvals WHERE id=?1", ids, NULL, NULL);
    if (!state) return refuse(a, 404, "There is no approval #%lld.", id);
    if (strcmp(state, "pending") != 0)
        return refuse(a, 409, "Approval #%lld is %s already: there is nothing to decide.", id, state);
    ApprovalDecision d;
    if (decision && strcmp(decision, "once") == 0) d = APPROVAL_ONCE;
    else if (decision && strcmp(decision, "always") == 0) d = APPROVAL_ALWAYS;
    else if (decision && strcmp(decision, "deny") == 0) d = APPROVAL_DENY;
    else return refuse(a, 400, "The decision must be once, always or deny.");
    /* What the event loop accepts (resolve.c): an applied request (a
     * configuration document, a new agent) has no single-use form, and a
     * sensitive target is never allowed for good. */
    if (d == APPROVAL_ONCE &&
        qi(a, "SELECT resolve = 'apply' FROM approvals WHERE id=?1", ids, NULL, NULL) == 1)
        return refuse(a, 400, "Approval #%lld is a request applied as a whole, which has no"
                              " \"once\": approve it or deny it.", id);
    if (d == APPROVAL_ALWAYS &&
        qi(a, "SELECT park_reason = 'sensitive_target' FROM approvals WHERE id=?1", ids, NULL, NULL) == 1)
        return refuse(a, 400, "Approval #%lld reaches a sensitive target, which is allowed one call"
                              " at a time: approve it once or deny it.", id);
    char *queued = qs(a, "SELECT decision FROM approval_decisions WHERE approval_id=?1", ids, NULL, NULL);
    if (queued) return refuse(a, 409, "A decision (%s) for approval #%lld is already on its way.", queued, id);
    /* Recorded here, carried out by the event loop: resolving runs tools and
     * advances a session, which this thread must not do. */
    if (approval_decision_queue(a->db, id, d, "dashboard") != 0)
        return refuse(a, 500, "The decision could not be recorded.");
    wake_session(0);
    int apply = qi(a, "SELECT resolve = 'apply' FROM approvals WHERE id=?1", ids, NULL, NULL) == 1;
    return done(a, NULL, "%s. The daemon carries it out in a moment.",
                d == APPROVAL_DENY ? "Denied" : d == APPROVAL_ONCE ? "Approved once"
                : apply ? "Approved" : "Approved, and allowed from now on");
}

static int act_approval_grant_from_history(Act *a) {
    long long id = 0;
    if (!fint(a, "approval_id", &id) || id <= 0) return refuse(a, 400, "Say which approval.");
    char *ids = num(a, id);
    char *state = qs(a, "SELECT state FROM approvals WHERE id=?1", ids, NULL, NULL);
    if (!state) return refuse(a, 404, "There is no approval #%lld.", id);
    if (strcmp(state, "denied") != 0)
        return refuse(a, 409, "Approval #%lld was not denied (it is %s).", id, state);
    if (qi(a, "SELECT tool_name = 'request_config' FROM approvals WHERE id=?1", ids, NULL, NULL) != 1)
        return refuse(a, 400, "Only a configuration request (request_config) can be granted from"
                              " history; other requests are re-asked by the agent.");
    if (admin_grant_from_history(a->db, id, "dashboard") != 0)
        return refuse(a, 400, "The request could not be applied (it may name something that no"
                              " longer exists). Nothing was changed.");
    long long nid = qi(a, "SELECT MAX(id) FROM approvals WHERE decided_via='dashboard'"
                          " AND state='approved'", NULL, NULL, NULL);
    return done(a, NULL, "Granted what approval #%lld asked for, recorded as approval #%lld.", id, nid);
}

/* ── secrets ─────────────────────────────────────────────────────────────── */

static int act_secret_add(Act *a) {
    char *name = ftext(a, "name"), *value = fraw_text(a, "value"), *scope = ftext(a, "scope");
    int replace = fbool(a, "replace", 0);
    if (!is_valid_secret_name(name))
        return refuse(a, 400, "A secret name uses capital letters, digits and _, starting with a"
                              " letter (for example GITHUB_TOKEN).");
    if (!value || !value[0]) return refuse(a, 400, "The value is empty.");
    if (strlen(value) > 65536) return refuse(a, 400, "The value is longer than 64 KB.");
    if (!scope) scope = "agent";
    if (strcmp(scope, "agent") && strcmp(scope, "system"))
        return refuse(a, 400, "The scope must be agent (usable by agents you grant it to) or system"
                              " (a provider key, used by the daemon only).");
    char *cur_scope = qs(a, "SELECT scope FROM secrets WHERE name=?1", name, NULL, NULL);
    if (cur_scope && !replace)
        return refuse(a, 409, "%s already exists. Use \"Replace value\" to change it.", name);
    if (cur_scope && strcmp(cur_scope, scope) != 0)
        return refuse(a, 409, "%s is a%s %s secret. Remove it first to change its scope.", name,
                      cur_scope[0] == 'a' ? "n" : "", cur_scope);
    if (!db_secret_key_loaded())
        return refuse(a, 503, "The secret store's key is not loaded in this process, so the value"
                              " cannot be stored.");
    if (tx_begin(a) != 0) return busy(a);
    int ok = db_secret_set(a->db, name, value, "operator", scope) == 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The secret could not be stored.");
    if (cur_scope) return done(a, NULL, "Replaced the value of %s. Who may use it is unchanged.", name);
    if (strcmp(scope, "system") == 0)
        return done(a, NULL, "Stored %s as a system secret: only the daemon uses it.", name);
    return done(a, NULL, "Stored %s. No agent may use it until you grant it, and it may be sent only"
                " to the hosts you allow.", name);
}

static int act_secret_hosts_set(Act *a) {
    char *name = ftext(a, "name"), *hosts = fjson(a, "hosts");
    if (!is_valid_secret_name(name)) return refuse(a, 400, "Say which secret.");
    if (qi(a, "SELECT 1 FROM secrets WHERE name=?1 AND scope='system'", name, NULL, NULL) == 1)
        return refuse(a, 400, "%s is a system secret: the daemon sends it only to its provider, so"
                              " allowed hosts do not apply.", name);
    if (!hosts || qi(a, "SELECT json_type(?1) = 'array'", hosts, NULL, NULL) != 1)
        return refuse(a, 400, "hosts must be a list of host names.");
    sqlite3_stmt *st = prep(a, "SELECT value FROM json_each(?1)", hosts, NULL, NULL, NULL);
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        const char *h = (const char *)sqlite3_column_text(st, 0);
        if (sqlite3_column_type(st, 0) != SQLITE_TEXT || !host_ok(h)) {
            char bad[256];
            snprintf(bad, sizeof(bad), "%s", h ? h : "");
            sqlite3_finalize(st);
            return refuse(a, 400, "\"%s\" is not a host name. Give one like api.github.com, or"
                                  " .github.com for every subdomain.", bad);
        }
    }
    sqlite3_finalize(st);
    if (tx_begin(a) != 0) return busy(a);
    int ok = qx(a, "DELETE FROM secret_hosts WHERE secret_name=?1 AND host NOT IN"
                   " (SELECT value FROM json_each(?2))", name, hosts, NULL, NULL) >= 0 &&
             qx(a, "INSERT OR IGNORE INTO secret_hosts(secret_name, host)"
                   " SELECT ?1, value FROM json_each(?2)", name, hosts, NULL, NULL) >= 0;
    if (tx_end(a, ok) != 0) return refuse(a, 500, "The hosts could not be saved.");
    long long n = qi(a, "SELECT count(*) FROM secret_hosts WHERE secret_name=?1", name, NULL, NULL);
    if (n == 0)
        return done(a, NULL, "%s may be sent nowhere now: a tool call that uses it is refused.", name);
    return done(a, NULL, "%s may be sent to %lld host%s.", name, n, n == 1 ? "" : "s");
}

static int act_secret_remove(Act *a) {
    char *name = ftext(a, "name");
    if (!is_valid_secret_name(name)) return refuse(a, 400, "Say which secret.");
    char *prov = qs(a, "SELECT group_concat(name, ', ') FROM providers WHERE api_key_env=?1",
                    name, NULL, NULL);
    if (prov && qi(a, "SELECT 1 FROM secrets WHERE name=?1", name, NULL, NULL) == 1)
        return refuse(a, 409, "%s is the API key of %s. Replace that provider's key, or remove the"
                              " provider, first.", name, prov);
    int n[3];
    if (db_secret_rm(a->db, name, n) != 0) return refuse(a, 500, "The secret could not be removed.");
    if (n[0] + n[1] + n[2] == 0) return refuse(a, 404, "There is no secret called %s.", name);
    return done(a, NULL, "Removed %s: %d stored value, %d allowed host%s, %d grant%s.", name,
                n[0], n[1], n[1] == 1 ? "" : "s", n[2], n[2] == 1 ? "" : "s");
}

/* ── dispatch ────────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *keys;      /* JSON array of the fields it takes */
    int (*fn)(Act *);
    int network;           /* manages the lock itself */
} ActDef;

static const ActDef ACTIONS[] = {
    { "grant_add", "[\"agent\",\"kind\",\"value\",\"approval_mode\",\"expires_at\"]", act_grant_add, 0 },
    { "grant_remove", "[\"agent\",\"kind\",\"value\"]", act_grant_remove, 0 },
    { "agent_copy", "[\"from\",\"name\",\"description\",\"attach_memory\"]", act_agent_copy, 0 },
    { "models_set", "[\"agent\",\"purpose\",\"models\"]", act_models_set, 0 },
    { "session_new_on_chat", "[\"agent\",\"channel\",\"chat_id\",\"confirm_handover\"]",
      act_session_new_on_chat, 0 },
    { "session_move_to_chat", "[\"session_id\",\"channel\",\"chat_id\",\"confirm_handover\"]",
      act_session_move_to_chat, 0 },
    { "chat_settings", "[\"channel\",\"chat_id\",\"delivery_mode\",\"tool_filter\",\"guidance\"]",
      act_chat_settings, 0 },
    { "chat_unroute", "[\"channel\",\"chat_id\"]", act_chat_unroute, 0 },
    { "channel_defaults", "[\"channel\",\"default_agent\",\"default_tool_filter\"]",
      act_channel_defaults, 0 },
    { "provider_add", "[\"name\",\"base_url\",\"endpoint_type\",\"api_key\"]", act_provider_add, 1 },
    { "provider_edit", "[\"name\",\"base_url\",\"endpoint_type\",\"request_extra\",\"api_key\"]",
      act_provider_edit, 1 },
    { "provider_refresh_models", "[\"name\"]", act_provider_refresh_models, 1 },
    { "provider_remove", "[\"name\"]", act_provider_remove, 0 },
    { "model_import", "[\"provider\",\"model\",\"context_window\",\"max_output_tokens\",\"capabilities\"]",
      act_model_import, 0 },
    { "model_edit", "[\"id\",\"context_window\",\"max_output_tokens\",\"capabilities\",\"effort_map\","
      "\"enabled\"]", act_model_edit, 0 },
    { "model_remove", "[\"id\"]", act_model_remove, 0 },
    { "memory_block_create", "[\"label\",\"description\",\"placement\",\"char_limit\",\"read_only\","
      "\"agent\"]", act_memory_block_create, 0 },
    { "memory_block_edit", "[\"block_id\",\"description\",\"placement\",\"char_limit\",\"read_only\"]",
      act_memory_block_edit, 0 },
    { "memory_block_delete", "[\"block_id\"]", act_memory_block_delete, 0 },
    { "memory_attach", "[\"block_id\",\"agent\",\"read_only\"]", act_memory_attach, 0 },
    { "memory_detach", "[\"block_id\",\"agent\"]", act_memory_detach, 0 },
    { "memory_note_add", "[\"block_id\",\"text\"]", act_memory_note_add, 0 },
    { "memory_note_edit", "[\"block_id\",\"note_id\",\"text\"]", act_memory_note_edit, 0 },
    { "memory_note_delete", "[\"block_id\",\"note_id\"]", act_memory_note_delete, 0 },
    { "approval_decide", "[\"approval_id\",\"decision\"]", act_approval_decide, 0 },
    { "approval_grant_from_history", "[\"approval_id\"]", act_approval_grant_from_history, 0 },
    { "secret_add", "[\"name\",\"value\",\"scope\",\"replace\"]", act_secret_add, 0 },
    { "secret_hosts_set", "[\"name\",\"hosts\"]", act_secret_hosts_set, 0 },
    { "secret_remove", "[\"name\"]", act_secret_remove, 0 },
};

int explore_act(sqlite3 *db, const char *body, char **out) {
    Act a;
    memset(&a, 0, sizeof(a));
    a.db = db;
    a.body = body ? body : "";
    lock(&a);
    int status;
    char *action = qi(&a, "SELECT json_valid(?1) AND json_type(?1) = 'object'", a.body, NULL, NULL) == 1
                   ? ftext(&a, "action") : NULL;
    const ActDef *def = NULL;
    for (size_t i = 0; action && i < sizeof(ACTIONS) / sizeof(ACTIONS[0]); i++)
        if (strcmp(action, ACTIONS[i].name) == 0) def = &ACTIONS[i];
    char *bad = def ? unknown_key(&a, def->keys) : NULL;
    if (!action) {
        status = refuse(&a, 400, "The request must be a JSON object naming an action.");
    } else if (!def) {
        status = refuse(&a, 404, "There is no action called \"%s\".", action);
    } else if (bad) {
        status = refuse(&a, 400, "%s does not take a field called \"%s\".", def->name, bad);
    } else {
        /* The change log names the handle (dashboard) and, through the
         * context, which action wrote each row. */
        char label[64];
        snprintf(label, sizeof(label), "act:%s", def->name);
        db_set_context(db, label);
        if (def->network) unlock(&a);
        status = def->fn(&a);
        lock(&a);
        if (!sqlite3_get_autocommit(db)) {
            LOG_WARN_("explorer action %s left a transaction open; rolled back", def->name);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        }
        db_set_context(db, NULL);
    }
    heap_free(&a);
    unlock(&a);
    *out = a.out ? a.out : strdup("{\"error\":\"internal error\"}");
    return a.out ? status : 500;
}

int explore_approval_card(sqlite3 *db, const char *id, char **out) {
    Act a;
    memset(&a, 0, sizeof(a));
    a.db = db;
    a.body = "{}";
    int status;
    Approval ap;
    memset(&ap, 0, sizeof(ap));
    sqlite3_stmt *st = prep(&a, "SELECT id, session_id, tool_call_id, tool_name, park_reason,"
                                " args_json, resolve, state, decided_via FROM approvals WHERE id=?1",
                            id ? id : "", NULL, NULL, NULL);
    if (st && sqlite3_step(st) == SQLITE_ROW) {
        ap.id = sqlite3_column_int64(st, 0);
        ap.session_id = sqlite3_column_int64(st, 1);
        char **f[7] = { &ap.tool_call_id, &ap.tool_name, &ap.park_reason, &ap.args_json,
                        &ap.resolve, &ap.state, &ap.decided_via };
        for (int i = 0; i < 7; i++)
            if (sqlite3_column_text(st, i + 2))
                *f[i] = strdup((const char *)sqlite3_column_text(st, i + 2));
    }
    sqlite3_finalize(st);
    if (!ap.id) {
        status = refuse(&a, 404, "There is no approval #%s.", id ? id : "");
    } else {
        char *summary = approval_format_summary(db, &ap);
        sqlite3_stmt *js = prep(&a, "SELECT json_object('id', CAST(?1 AS INTEGER), 'state', ?2,"
                                    " 'summary', ?3, 'resolve', resolve, 'park_reason', park_reason,"
                                    " 'tool_name', tool_name) FROM approvals WHERE id = CAST(?1 AS INTEGER)",
                                num(&a, ap.id), ap.state, summary, NULL);
        if (js && sqlite3_step(js) == SQLITE_ROW)
            a.out = strdup((const char *)sqlite3_column_text(js, 0));
        sqlite3_finalize(js);
        free(summary);
        status = a.out ? 200 : refuse(&a, 500, "The card could not be drawn.");
        free(ap.tool_call_id); free(ap.tool_name); free(ap.park_reason); free(ap.args_json);
        free(ap.resolve); free(ap.state); free(ap.decided_via);
    }
    heap_free(&a);
    *out = a.out;
    return status;
}
