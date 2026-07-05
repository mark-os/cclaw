/* Hook dispatch — fresh QuickJS context per invocation.
 * Long-lived runtime (in ExtensionCtx->rt), fresh context per hook call.
 * Command hooks (specs/hooks.md): the hook sees a small `input` JSON object and
 * returns targeted commands; C validates and applies them via SQL. Hooks never
 * see or rebuild the full messages array. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hook_dispatch.h"
#include "db.h"
#include "qjs_helpers.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* Inject caps (spec §9): per-dispatch flood guard. */
#define HOOK_INJECT_MAX_ENTRIES 3
#define HOOK_INJECT_MAX_CHARS   2048

/* Create a fresh hooks context on the session runtime. */
static JSContext *hook_ctx_new(ExtensionCtx *ext_ctx) {
    if (!ext_ctx || !ext_ctx->rt || !ext_ctx->rt->heap) return NULL;
    QjsRuntime *qrt = (QjsRuntime *)ext_ctx->rt->heap;
    return qjs_context_create(qrt, QJS_PROFILE_HOOKS);
}

/* Run one row of a bound (?1=session_id, ?2=json) write statement. */
static int exec_sid_json(sqlite3 *db, const char *sql, int64_t session_id,
                         const char *json) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(s, 1, session_id);
    sqlite3_bind_text(s, 2, json, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Single-row single-column text query with ?1=session_id [, ?2=entry_id]. */
static char *query_json(sqlite3 *db, const char *sql, int64_t session_id,
                        int64_t entry_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(s, 1, session_id);
    if (entry_id > 0) sqlite3_bind_int64(s, 2, entry_id);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r;
}

/* ── Chained command dispatch ──────────────────────────────────────
 * Shared runner: inject `input` + empty `__hook_cmds`, call each hook in load
 * order with input as the argument, merge its return into __hook_cmds via the
 * event-specific merge_js fragment. A throwing hook is skipped. Returns the
 * accumulated commands JSON, or NULL when empty / no hooks. */
static char *run_command_hooks(ExtensionCtx *ext_ctx, HookEvent ev,
                               const char *input_json, const char *merge_js) {
    if (!ext_ctx || !ext_ctx->rt) return NULL;
    HookList *hl = &ext_ctx->hooks[ev];
    if (hl->count == 0) return NULL;

    JSContext *ctx = hook_ctx_new(ext_ctx);
    if (!ctx) return NULL;
    if (qjs_set_global_json(ctx, "input", input_json) != 0 ||
        qjs_set_global_json(ctx, "__hook_cmds", "{}") != 0) {
        JS_FreeContext(ctx);
        return NULL;
    }

    for (size_t i = 0; i < hl->count; i++) {
        size_t cl = strlen(hl->fns[i]) + strlen(merge_js) + 160;
        char *code = malloc(cl);
        if (!code) continue;
        snprintf(code, cl,
                 "(function(){"
                 "var __fn = %s;"
                 "var __r = __fn(input);"
                 "if (!__r || typeof __r !== 'object') return;"
                 "%s"
                 "})()",
                 hl->fns[i], merge_js);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_TYPE_GLOBAL);
        free(code);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            JS_FreeValue(ctx, v);
        }
    }

    char *cmds = qjs_get_global_json(ctx, "__hook_cmds");
    JS_FreeContext(ctx);
    if (cmds && strcmp(cmds, "{}") == 0) { free(cmds); cmds = NULL; }
    return cmds;
}

/* ── preAdvance ────────────────────────────────────────────────── */

static const char SQL_PRE_ADVANCE_INPUT[] =
    "WITH RECURSIVE branch(id, parent_id, token_estimate) AS ("
    "  SELECT id, parent_id, token_estimate FROM entries"
    "    WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1) AND session_id=?1"
    "  UNION ALL"
    "  SELECT e.id, e.parent_id, e.token_estimate"
    "    FROM entries e JOIN branch b ON e.id=b.parent_id"
    ") SELECT json_object("
    "  'session_id', ?1,"
    "  'agent', (SELECT COALESCE(agent_name,'') FROM sessions WHERE id=?1),"
    "  'entry_count', (SELECT COUNT(*) FROM branch),"
    "  'last_role', COALESCE((SELECT CASE role WHEN 0 THEN 'system'"
    "     WHEN 1 THEN 'user' WHEN 2 THEN 'assistant' WHEN 3 THEN 'tool'"
    "     ELSE 'compaction' END FROM entries"
    "     WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1)),''),"
    "  'last_entry_id', (SELECT COALESCE(leaf_id,-1) FROM sessions WHERE id=?1),"
    "  'token_estimate', (SELECT COALESCE(SUM(token_estimate),0) FROM branch),"
    "  'turn_number', (SELECT COALESCE(MAX(turn_id),0) FROM entries WHERE session_id=?1),"
    "  'pending_tool_calls', (SELECT COUNT(*) FROM tool_calls"
    "     WHERE session_id=?1 AND status='pending'));";

/* Commands accumulate across hooks: arrays concat, set_data merges by key. */
static const char MERGE_PRE_ADVANCE[] =
    "if (Array.isArray(__r.inject))"
    "  __hook_cmds.inject = (__hook_cmds.inject||[]).concat(__r.inject);"
    "if (Array.isArray(__r.suppress))"
    "  __hook_cmds.suppress = (__hook_cmds.suppress||[]).concat(__r.suppress);"
    "if (Array.isArray(__r.pin))"
    "  __hook_cmds.pin = (__hook_cmds.pin||[]).concat(__r.pin);"
    "if (__r.set_data && typeof __r.set_data === 'object') {"
    "  __hook_cmds.set_data = __hook_cmds.set_data || {};"
    "  for (var k in __r.set_data) __hook_cmds.set_data[k] = __r.set_data[k];"
    "}";

char *hook_dispatch_pre_advance(ExtensionCtx *ext_ctx, sqlite3 *db,
                                int64_t session_id) {
    if (!ext_ctx || !ext_ctx->rt || ext_ctx->hooks[HOOK_PRE_ADVANCE].count == 0)
        return NULL;
    char *input = query_json(db, SQL_PRE_ADVANCE_INPUT, session_id, 0);
    if (!input) return NULL;
    char *cmds = run_command_hooks(ext_ctx, HOOK_PRE_ADVANCE, input, MERGE_PRE_ADVANCE);
    free(input);
    return cmds;
}

/* inject: role must be system|user, non-empty content; capped per dispatch.
 * Ephemeral → hook_directives row (this request only); persistent → real
 * entries row. */
static int apply_inject(sqlite3 *db, int64_t session_id, const char *cmds_json) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(value,'$.role'), json_extract(value,'$.content'),"
            "       COALESCE(json_extract(value,'$.ephemeral'),0)"
            " FROM json_each(?1,'$.inject');",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, cmds_json, -1, SQLITE_STATIC);

    sqlite3_stmt *ins = NULL;
    int count = 0;
    size_t total = 0;
    int rc = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *role = (const char *)sqlite3_column_text(s, 0);
        const char *content = (const char *)sqlite3_column_text(s, 1);
        int ephemeral = sqlite3_column_int(s, 2);
        if (!role || (strcmp(role, "system") != 0 && strcmp(role, "user") != 0)) {
            LOG_WARN_("hook inject dropped: bad role");
            continue;
        }
        if (!content || !content[0]) {
            LOG_WARN_("hook inject dropped: empty content");
            continue;
        }
        size_t clen = strlen(content);
        if (count >= HOOK_INJECT_MAX_ENTRIES || total + clen > HOOK_INJECT_MAX_CHARS) {
            LOG_WARN_("hook inject dropped: cap exceeded"
                     " (%d entries / %d chars per dispatch)",
                     HOOK_INJECT_MAX_ENTRIES, HOOK_INJECT_MAX_CHARS);
            continue;
        }
        count++;
        total += clen;
        if (ephemeral) {
            if (!ins && sqlite3_prepare_v2(db,
                    "INSERT INTO hook_directives(session_id, kind, role, content)"
                    " VALUES(?1,'inject',?2,?3);", -1, &ins, NULL) != SQLITE_OK) {
                rc = -1;
                break;
            }
            sqlite3_bind_int64(ins, 1, session_id);
            sqlite3_bind_text(ins, 2, role, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 3, content, -1, SQLITE_STATIC);
            if (sqlite3_step(ins) != SQLITE_DONE) rc = -1;
            sqlite3_reset(ins);
        } else {
            Message m = {.role = strcmp(role, "user") == 0 ? ROLE_USER : ROLE_SYSTEM,
                         .content = (char *)content};
            if (entry_append_with_turn(db, session_id, &m, 0) < 0) rc = -1;
        }
    }
    if (ins) sqlite3_finalize(ins);
    sqlite3_finalize(s);
    return rc;
}

int hook_apply_pre_advance(sqlite3 *db, int64_t session_id, const char *cmds_json) {
    if (!db || !cmds_json) return -1;
    int rc = 0;

    /* pin — durable entries.data flag; session membership via the WHERE */
    if (exec_sid_json(db,
            "UPDATE entries SET data=json_patch(COALESCE(data,'{}'),'{\"pin\":true}')"
            " WHERE session_id=?1"
            "   AND id IN (SELECT value FROM json_each(?2,'$.pin'));",
            session_id, cmds_json) != 0)
        rc = -1;

    /* set_data — per-entry json_patch; object values only */
    if (exec_sid_json(db,
            /* j.type (not json_type(j.value)): json_each unquotes scalar
             * strings in .value, so json_type() on them throws malformed. */
            "UPDATE entries SET data=json_patch(COALESCE(entries.data,'{}'), j.value)"
            " FROM json_each(?2,'$.set_data') j"
            " WHERE json_type(?2,'$.set_data')='object'"
            "   AND entries.id=CAST(j.key AS INTEGER) AND entries.session_id=?1"
            "   AND j.type='object';",
            session_id, cmds_json) != 0)
        rc = -1;

    /* suppress — this-request-only directive rows. Excluded: the most recent
     * user entry (the actual request), same-dispatch pins, durable data.pin —
     * pin wins over suppress. */
    if (exec_sid_json(db,
            "INSERT INTO hook_directives(session_id, kind, entry_id)"
            " SELECT ?1, 'suppress', e.id"
            " FROM json_each(?2,'$.suppress') j"
            " JOIN entries e ON e.id=j.value AND e.session_id=?1"
            " WHERE e.id != (SELECT COALESCE(MAX(id),-1) FROM entries"
            "                 WHERE session_id=?1 AND role=1)"
            "   AND j.value NOT IN (SELECT value FROM json_each(?2,'$.pin'))"
            "   AND COALESCE(json_extract(e.data,'$.pin'),0) != 1;",
            session_id, cmds_json) != 0)
        rc = -1;

    if (apply_inject(db, session_id, cmds_json) != 0)
        rc = -1;

    return rc;
}

/* ── postAdvance ───────────────────────────────────────────────── */

static const char SQL_POST_ADVANCE_INPUT[] =
    "SELECT json_object("
    "  'session_id', ?1,"
    "  'agent', (SELECT COALESCE(agent_name,'') FROM sessions WHERE id=?1),"
    "  'entry_id', e.id,"
    "  'role', 'assistant',"
    "  'content', COALESCE(e.content,''),"
    "  'tool_calls', (SELECT json_group_array(json_object("
    "      'id', tc.tool_call_id, 'name', tc.tool_name,"
    "      'args', COALESCE(tc.content,'{}')) ORDER BY tc.part_index)"
    "    FROM entries tc WHERE tc.session_id=e.session_id"
    "      AND tc.turn_id=e.turn_id AND tc.type='tool_call'),"
    "  'stop_reason', CASE e.stop_reason WHEN 1 THEN 'stop' WHEN 2 THEN 'length'"
    "    WHEN 3 THEN 'tool_use' WHEN 4 THEN 'error' WHEN 5 THEN 'aborted'"
    "    ELSE 'none' END,"
    "  'usage_in', COALESCE(e.usage_in,0),"
    "  'usage_out', COALESCE(e.usage_out,0))"
    " FROM entries e WHERE e.id=?2 AND e.session_id=?1"
    "   AND e.type='assistant_message';";

/* Last transform_content wins; the chain sees it via input.content. annotate
 * merges across hooks. */
static const char MERGE_POST_ADVANCE[] =
    "if (typeof __r.transform_content === 'string') {"
    "  __hook_cmds.transform_content = __r.transform_content;"
    "  input.content = __r.transform_content;"
    "}"
    "if (__r.annotate && typeof __r.annotate === 'object') {"
    "  __hook_cmds.annotate = __hook_cmds.annotate || {};"
    "  for (var k in __r.annotate) __hook_cmds.annotate[k] = __r.annotate[k];"
    "}";

char *hook_dispatch_post_advance(ExtensionCtx *ext_ctx, sqlite3 *db,
                                 int64_t session_id, int64_t entry_id) {
    if (!ext_ctx || !ext_ctx->rt || ext_ctx->hooks[HOOK_POST_ADVANCE].count == 0)
        return NULL;
    char *input = query_json(db, SQL_POST_ADVANCE_INPUT, session_id, entry_id);
    if (!input) return NULL;
    char *cmds = run_command_hooks(ext_ctx, HOOK_POST_ADVANCE, input, MERGE_POST_ADVANCE);
    free(input);
    return cmds;
}

/* Rewrite the assistant entry's content, preserving data.original_content on
 * the first transform only. entries_fts is external-content with only an
 * AFTER INSERT trigger (schema.sql), so the old FTS row must be deleted and
 * the new one inserted by hand — all inside one transaction. */
static int apply_transform_content(sqlite3 *db, int64_t session_id,
                                   int64_t entry_id, const char *new_content) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    /* Fetch old content; gate on type so role/tool_calls stay untouchable. */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(content,'') FROM entries"
            " WHERE id=?1 AND session_id=?2 AND type='assistant_message';",
            -1, &s, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_bind_int64(s, 1, entry_id);
    sqlite3_bind_int64(s, 2, session_id);
    char *old_content = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        old_content = strdup(t ? t : "");
    }
    sqlite3_finalize(s);
    if (!old_content) {  /* not an assistant entry (or gone) — nothing to do */
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return 0;
    }

    if (sqlite3_prepare_v2(db,
            "INSERT INTO entries_fts(entries_fts, rowid, content)"
            " VALUES('delete', ?1, ?2);", -1, &s, NULL) != SQLITE_OK) {
        free(old_content);
        goto fail;
    }
    sqlite3_bind_int64(s, 1, entry_id);
    sqlite3_bind_text(s, 2, old_content, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    free(old_content);
    if (rc != SQLITE_DONE) goto fail;

    /* data patch reads the OLD content (UPDATE RHS sees pre-update values), so
     * original_content is set exactly once — the CASE keeps later transforms
     * from clobbering it. */
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET"
            "  data = CASE WHEN json_extract(COALESCE(data,'{}'),'$.original_content') IS NULL"
            "    THEN json_patch(COALESCE(data,'{}'),"
            "                    json_object('original_content', COALESCE(content,'')))"
            "    ELSE data END,"
            "  content = ?3,"
            "  token_estimate = length(?3)/4 + 4,"
            "  content_bytes = length(?3)"
            " WHERE id=?1 AND session_id=?2 AND type='assistant_message';",
            -1, &s, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_bind_int64(s, 1, entry_id);
    sqlite3_bind_int64(s, 2, session_id);
    sqlite3_bind_text(s, 3, new_content, -1, SQLITE_STATIC);
    rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) goto fail;

    if (sqlite3_prepare_v2(db,
            "INSERT INTO entries_fts(rowid, content) VALUES(?1, ?2);",
            -1, &s, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_bind_int64(s, 1, entry_id);
    sqlite3_bind_text(s, 2, new_content, -1, SQLITE_STATIC);
    rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) goto fail;

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    return 0;

fail:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

int hook_apply_post_advance(sqlite3 *db, int64_t session_id, int64_t entry_id,
                            const char *cmds_json) {
    if (!db || !cmds_json) return -1;
    int rc = 0;

    /* transform_content (string command only) */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(?1,'$.transform_content')"
            " WHERE json_type(?1,'$.transform_content')='text';",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, cmds_json, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(s, 0);
            if (t && apply_transform_content(db, session_id, entry_id, t) != 0)
                rc = -1;
        }
        sqlite3_finalize(s);
    }

    /* annotate — merge into entries.data */
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET data=json_patch(COALESCE(data,'{}'),"
            "                                   json_extract(?3,'$.annotate'))"
            " WHERE id=?1 AND session_id=?2"
            "   AND json_type(?3,'$.annotate')='object';",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, entry_id);
        sqlite3_bind_int64(s, 2, session_id);
        sqlite3_bind_text(s, 3, cmds_json, -1, SQLITE_STATIC);
        if (sqlite3_step(s) != SQLITE_DONE) rc = -1;
        sqlite3_finalize(s);
    } else {
        rc = -1;
    }

    return rc;
}

/* ── Tool-call hooks (gate / afterToolCall) ────────────────────── */

/* Build {name, args[, result]} JSON via SQLite. */
static char *build_tc_ctx_json(sqlite3 *db, const char *name, const char *args,
                               const char *result) {
    char *json_str = NULL;
    sqlite3_stmt *jstmt;
    const char *jsql = result
        ? "SELECT json_object('name',?1,'args',"
          "CASE WHEN json_valid(?2) THEN json(?2) ELSE ?2 END,'result',?3)"
        : "SELECT json_object('name',?1,'args',"
          "CASE WHEN json_valid(?2) THEN json(?2) ELSE ?2 END)";
    if (sqlite3_prepare_v2(db, jsql, -1, &jstmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(jstmt, 1, name ? name : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(jstmt, 2, args ? args : "null", -1, SQLITE_STATIC);
        if (result) sqlite3_bind_text(jstmt, 3, result, -1, SQLITE_STATIC);
        if (sqlite3_step(jstmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(jstmt, 0);
            if (v) json_str = strdup(v);
        }
        sqlite3_finalize(jstmt);
    }
    return json_str;
}

HookGate hook_dispatch_gate_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                      const char *name, const char *args,
                                      char **reason_out) {
    if (reason_out) *reason_out = NULL;
    if (!ext_ctx || !ext_ctx->rt) return HOOK_GATE_ALLOW;
    HookList *hl = &ext_ctx->hooks[HOOK_BEFORE_TOOL_CALL];
    if (hl->count == 0) return HOOK_GATE_ALLOW;

    char *json_str = build_tc_ctx_json(db, name, args, NULL);
    if (!json_str) return HOOK_GATE_ALLOW;

    JSContext *ctx = hook_ctx_new(ext_ctx);
    if (!ctx) { free(json_str); return HOOK_GATE_ALLOW; }

    if (qjs_set_global_json(ctx, "input", json_str) != 0) {
        free(json_str); JS_FreeContext(ctx); return HOOK_GATE_ALLOW;
    }
    free(json_str);

    HookGate decision = HOOK_GATE_ALLOW;
    for (size_t i = 0; i < hl->count; i++) {
        size_t cl = strlen(hl->fns[i]) + 320;
        char *code = malloc(cl);
        if (!code) continue;
        snprintf(code, cl,
                 "(function(){"
                 "var __fn = %s;"
                 "var __r = __fn(input);"
                 "if (!__r) return '';"
                 "var __why = (typeof __r.reason === 'string') ? __r.reason : '';"
                 "if (__r.deny || __r.block) return 'deny:' + __why;"
                 "if (__r.ask) return 'ask:' + __why;"
                 "return '';"
                 "})()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_TYPE_GLOBAL);
        free(code);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        const char *str = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!str || !str[0]) { if (str) JS_FreeCString(ctx, str); continue; }

        HookGate g = HOOK_GATE_ALLOW;
        const char *why = NULL;
        if (strncmp(str, "deny:", 5) == 0) { g = HOOK_GATE_DENY; why = str + 5; }
        else if (strncmp(str, "ask:", 4) == 0) { g = HOOK_GATE_ASK; why = str + 4; }
        if (g > decision) {
            decision = g;
            if (reason_out) {
                free(*reason_out);
                *reason_out = (why && why[0]) ? strdup(why) : NULL;
            }
        }
        JS_FreeCString(ctx, str);
    }

    JS_FreeContext(ctx);
    return decision;
}

char *hook_dispatch_observe_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                      const char *name, const char *args,
                                      const char *result, char **annotate_out) {
    if (annotate_out) *annotate_out = NULL;
    if (!ext_ctx || !ext_ctx->rt) return NULL;
    HookList *hl = &ext_ctx->hooks[HOOK_AFTER_TOOL_CALL];
    if (hl->count == 0) return NULL;

    char *json_str = build_tc_ctx_json(db, name, args, result ? result : "");
    if (!json_str) return NULL;

    JSContext *ctx = hook_ctx_new(ext_ctx);
    if (!ctx) { free(json_str); return NULL; }

    if (qjs_set_global_json(ctx, "input", json_str) != 0 ||
        qjs_set_global_json(ctx, "__hook_annotate", "null") != 0) {
        free(json_str); JS_FreeContext(ctx); return NULL;
    }
    free(json_str);

    for (size_t i = 0; i < hl->count; i++) {
        size_t cl = strlen(hl->fns[i]) + 512;
        char *code = malloc(cl);
        if (!code) continue;
        /* Replacement chains through input.result — the next hook sees it. */
        snprintf(code, cl,
                 "(function(){"
                 "var __fn = %s;"
                 "var __r = __fn(input);"
                 "if (!__r || typeof __r !== 'object') return;"
                 "if (typeof __r.result === 'string') input.result = __r.result;"
                 "if (__r.annotate && typeof __r.annotate === 'object') {"
                 "  if (!__hook_annotate) __hook_annotate = {};"
                 "  for (var k in __r.annotate) __hook_annotate[k] = __r.annotate[k];"
                 "}"
                 "})()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_TYPE_GLOBAL);
        free(code);
        if (JS_IsException(v))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, v);
    }

    /* Read back input.result; return it only if it actually changed. */
    char *replacement = NULL;
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue inp = JS_GetPropertyStr(ctx, global, "input");
        JS_FreeValue(ctx, global);
        JSValue rv = JS_GetPropertyStr(ctx, inp, "result");
        JS_FreeValue(ctx, inp);
        if (JS_IsString(rv)) {
            const char *sr = JS_ToCString(ctx, rv);
            if (sr && (!result || strcmp(sr, result) != 0))
                replacement = strdup(sr);
            if (sr) JS_FreeCString(ctx, sr);
        }
        JS_FreeValue(ctx, rv);
    }

    if (annotate_out) {
        char *ann = qjs_get_global_json(ctx, "__hook_annotate");
        if (ann && strcmp(ann, "null") == 0) { free(ann); ann = NULL; }
        *annotate_out = ann;
    }

    JS_FreeContext(ctx);
    return replacement;
}

/* ── Small SQL helpers shared by the apply paths ───────────────── */

int hook_entry_data_patch(sqlite3 *db, int64_t entry_id, const char *patch_json) {
    if (!db || !patch_json) return -1;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET data=json_patch(COALESCE(data,'{}'), json(?2))"
            " WHERE id=?1 AND json_type(?2)='object';",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(s, 1, entry_id);
    sqlite3_bind_text(s, 2, patch_json, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

void hook_directives_clear(sqlite3 *db, int64_t session_id) {
    if (!db) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "DELETE FROM hook_directives WHERE session_id=?1;",
                           -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, session_id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}
