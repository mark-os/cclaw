/* T258: beforeRequest hook dispatch.
 * V112: serialize messages to JS array, call each registered hook in order,
 * deserialize modified array back. Skip on throw. */
#define _POSIX_C_SOURCE 200809L
#include "hook_dispatch.h"
#include "log.h"
#include <mquickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build a JS messages array from the plan entries (from cut onward).
 * Injects into global scope as __hook_messages. */
static int build_messages_array(JSContext *ctx, sqlite3 *db,
                                int64_t session_id, const ContextPlan *plan) {
    /* Build each message as JSON using SQLite json_object */
    const char *sql =
        "SELECT CASE"
        "  WHEN role=3 THEN json_object('role','tool',"
        "    'tool_call_id',COALESCE(tool_call_id,''),"
        "    'content',COALESCE(content,''))"
        "  WHEN role=2 AND tool_calls IS NOT NULL THEN"
        "    json_patch(json_object('role','assistant','content',content),"
        "      json_object('tool_calls',json(tool_calls)))"
        "  WHEN role=2 THEN json_object('role','assistant','content',content)"
        "  WHEN role=0 THEN json_object('role','system','content',COALESCE(content,''))"
        "  ELSE json_object('role','user','content',COALESCE(content,''))"
        " END FROM entries WHERE id=? AND session_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    /* Start: globalThis.__hook_messages = []; */
    const char *init = "globalThis.__hook_messages = [];";
    JSValue v = JS_Eval(ctx, init, strlen(init), "<hook>", 0);
    if (JS_IsException(v)) { sqlite3_finalize(stmt); return -1; }

    for (int i = plan->cut; i < plan->count; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, plan->entries[i].id);
        sqlite3_bind_int64(stmt, 2, session_id);
        if (sqlite3_step(stmt) != SQLITE_ROW) continue;

        const char *json_str = (const char *)sqlite3_column_text(stmt, 0);
        if (!json_str) continue;

        /* Push into JS array */
        size_t code_len = strlen(json_str) + 64;
        char *code = malloc(code_len);
        if (code) {
            snprintf(code, code_len,
                     "globalThis.__hook_messages.push(%s);", json_str);
            JSValue r = JS_Eval(ctx, code, strlen(code), "<hook>", 0);
            if (JS_IsException(r)) { /* skip entry */ }
            free(code);
        }
    }

    sqlite3_finalize(stmt);
    return 0;
}

/* Call each beforeRequest hook with the messages array. */
static int call_hooks(JSContext *ctx, ExtensionCtx *ext_ctx) {
    HookList *hl = &ext_ctx->hooks[HOOK_BEFORE_REQUEST];

    for (size_t i = 0; i < hl->count; i++) {
        /* Call: var __r = hook_fn(globalThis.__hook_messages);
         * if (__r && Array.isArray(__r)) globalThis.__hook_messages = __r; */
        size_t code_len = strlen(hl->fns[i]) + 256;
        char *code = malloc(code_len);
        if (!code) continue;
        snprintf(code, code_len,
                 "(function(){"
                 "var __fn = %s;"
                 "var __r = __fn(globalThis.__hook_messages);"
                 "if (__r && Array.isArray(__r)) globalThis.__hook_messages = __r;"
                 "})()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", 0);
        free(code);
        if (JS_IsException(v)) {
            /* V112: skip on throw — clear exception, continue */
            JSValue exc = JS_GetException(ctx);
            (void)exc;
            continue;
        }
    }
    return 0;
}

/* Serialize __hook_messages back to JSON array string. */
static char *serialize_messages(JSContext *ctx) {
    const char *code = "JSON.stringify(globalThis.__hook_messages)";
    JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_RETVAL);
    if (JS_IsException(v)) return NULL;

    JSCStringBuf buf;
    const char *str = JS_ToCString(ctx, v, &buf);
    if (!str) return NULL;
    return strdup(str);
}

char *hook_dispatch_before_request(ExtensionCtx *ext_ctx, sqlite3 *db,
                                   int64_t session_id, const Config *cfg,
                                   const ContextPlan *plan,
                                   const ToolSchema *tools, size_t tool_count) {
    if (!ext_ctx || !ext_ctx->rt || !ext_ctx->rt->ctx) return NULL;
    if (ext_ctx->hooks[HOOK_BEFORE_REQUEST].count == 0) return NULL;

    JSContext *ctx = (JSContext *)ext_ctx->rt->ctx;

    /* Load entries into JS messages array */
    if (build_messages_array(ctx, db, session_id, plan) != 0) return NULL;

    /* Call hooks */
    call_hooks(ctx, ext_ctx);

    /* Serialize modified messages back */
    char *messages_json = serialize_messages(ctx);
    if (!messages_json) return NULL;

    /* Build full request body: {"model":"...","messages":ARRAY,"tools":...,"max_tokens":...} */
    const char *model = cfg->provider.model ? cfg->provider.model : "unknown";

    /* Tools fragment via SQLite */
    char *tools_json = NULL;
    if (tools && tool_count > 0) {
        sqlite3_exec(db, "DROP TABLE IF EXISTS _hook_tools;", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TEMP TABLE _hook_tools(pos INTEGER,name TEXT,description TEXT,params TEXT);", NULL, NULL, NULL);
        sqlite3_stmt *ins;
        if (sqlite3_prepare_v2(db, "INSERT INTO _hook_tools VALUES(?,?,?,?)", -1, &ins, NULL) == SQLITE_OK) {
            for (size_t i = 0; i < tool_count; i++) {
                sqlite3_bind_int(ins, 1, (int)i);
                sqlite3_bind_text(ins, 2, tools[i].name, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 3, tools[i].description ? tools[i].description : "", -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, tools[i].parameters_json ? tools[i].parameters_json : "{}", -1, SQLITE_STATIC);
                sqlite3_step(ins); sqlite3_reset(ins);
            }
            sqlite3_finalize(ins);
        }
        sqlite3_stmt *sel;
        if (sqlite3_prepare_v2(db,
            "SELECT json_group_array(json_object('type','function','function',"
            "json_object('name',name,'description',description,"
            "'parameters',json(params))) ORDER BY pos) FROM _hook_tools",
            -1, &sel, NULL) == SQLITE_OK) {
            if (sqlite3_step(sel) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(sel, 0);
                if (v) tools_json = strdup(v);
            }
            sqlite3_finalize(sel);
        }
        sqlite3_exec(db, "DROP TABLE IF EXISTS _hook_tools;", NULL, NULL, NULL);
    }

    /* Assemble full request via SQLite json_object */
    char *body = NULL;
    sqlite3_stmt *asm_stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT json_object('model',?1,'messages',json(?2),"
        "'tools',CASE WHEN ?3 IS NOT NULL AND json_array_length(?3)>0 THEN json(?3) ELSE NULL END,"
        "'max_tokens',CASE WHEN ?4>0 THEN ?4 ELSE NULL END)",
        -1, &asm_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(asm_stmt, 1, model, -1, SQLITE_STATIC);
        sqlite3_bind_text(asm_stmt, 2, messages_json, -1, SQLITE_STATIC);
        if (tools_json) sqlite3_bind_text(asm_stmt, 3, tools_json, -1, SQLITE_STATIC);
        else sqlite3_bind_null(asm_stmt, 3);
        sqlite3_bind_int(asm_stmt, 4, cfg->provider.max_tokens);
        if (sqlite3_step(asm_stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(asm_stmt, 0);
            if (v) body = strdup(v);
        }
        sqlite3_finalize(asm_stmt);
    }

    free(messages_json);
    free(tools_json);

    LOG_DEBUG_(cfg, "beforeRequest hook: modified request (%zu bytes)", strlen(body));
    return body;
}

/* Build {name, args[, result]} as a JSON string via SQLite. Caller frees. */
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

/* §8 gating hook dispatch — restrict-only, most-restrictive wins. */
HookGate hook_dispatch_gate_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                      const char *name, const char *args,
                                      char **reason_out) {
    if (reason_out) *reason_out = NULL;
    if (!ext_ctx || !ext_ctx->rt || !ext_ctx->rt->ctx) return HOOK_GATE_ALLOW;
    HookList *hl = &ext_ctx->hooks[HOOK_BEFORE_TOOL_CALL];
    if (hl->count == 0) return HOOK_GATE_ALLOW;

    JSContext *ctx = (JSContext *)ext_ctx->rt->ctx;

    char *json_str = build_tc_ctx_json(db, name, args, NULL);
    if (!json_str) return HOOK_GATE_ALLOW;
    size_t code_len = strlen(json_str) + 64;
    char *setup = malloc(code_len);
    if (!setup) { free(json_str); return HOOK_GATE_ALLOW; }
    snprintf(setup, code_len, "globalThis.__hook_tc_ctx = %s;", json_str);
    free(json_str);
    JSValue sv = JS_Eval(ctx, setup, strlen(setup), "<hook>", 0);
    free(setup);
    if (JS_IsException(sv)) return HOOK_GATE_ALLOW;

    HookGate decision = HOOK_GATE_ALLOW;
    /* Each hook returns "deny:<reason>", "ask:<reason>", or "" (allow).
     * A hook may only restrict, so we keep the most restrictive across hooks. */
    for (size_t i = 0; i < hl->count; i++) {
        size_t cl = strlen(hl->fns[i]) + 320;
        char *code = malloc(cl);
        if (!code) continue;
        snprintf(code, cl,
                 "(function(){"
                 "var __fn = %s;"
                 "var __r = __fn(globalThis.__hook_tc_ctx);"
                 "if (!__r) return '';"
                 "var __why = (typeof __r.reason === 'string') ? __r.reason : '';"
                 "if (__r.deny || __r.block) return 'deny:' + __why;"
                 "if (__r.ask) return 'ask:' + __why;"
                 "return '';"
                 "})()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_RETVAL);
        free(code);
        if (JS_IsException(v)) { JSValue exc = JS_GetException(ctx); (void)exc; continue; }
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, v, &buf);
        if (!str || !str[0]) continue;
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
    }
    return decision;
}

/* §8 observer hook dispatch — side-effect only, return value ignored. */
void hook_dispatch_observe_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                     const char *name, const char *args,
                                     const char *result) {
    if (!ext_ctx || !ext_ctx->rt || !ext_ctx->rt->ctx) return;
    HookList *hl = &ext_ctx->hooks[HOOK_AFTER_TOOL_CALL];
    if (hl->count == 0) return;

    JSContext *ctx = (JSContext *)ext_ctx->rt->ctx;

    char *json_str = build_tc_ctx_json(db, name, args, result ? result : "");
    if (!json_str) return;
    size_t code_len = strlen(json_str) + 64;
    char *setup = malloc(code_len);
    if (!setup) { free(json_str); return; }
    snprintf(setup, code_len, "globalThis.__hook_tc_ctx = %s;", json_str);
    free(json_str);
    JSValue sv = JS_Eval(ctx, setup, strlen(setup), "<hook>", 0);
    free(setup);
    if (JS_IsException(sv)) return;

    /* Call each observer; its return value is deliberately ignored. */
    for (size_t i = 0; i < hl->count; i++) {
        size_t cl = strlen(hl->fns[i]) + 128;
        char *code = malloc(cl);
        if (!code) continue;
        snprintf(code, cl,
                 "(function(){ var __fn = %s; __fn(globalThis.__hook_tc_ctx); })()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", 0);
        free(code);
        if (JS_IsException(v)) { JSValue exc = JS_GetException(ctx); (void)exc; }
    }
}

/* V111/T260: turnStart/turnEnd — informational, no return value. */

