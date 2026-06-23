/* T258: Hook dispatch — fresh QuickJS context per invocation.
 * V112: Long-lived runtime (in ExtensionCtx->rt), fresh context per hook call.
 * Data injected as JSON globals, results serialized back. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hook_dispatch.h"
#include "qjs_helpers.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Create a fresh hooks context on the session runtime. */
static JSContext *hook_ctx_new(ExtensionCtx *ext_ctx) {
    if (!ext_ctx || !ext_ctx->rt || !ext_ctx->rt->heap) return NULL;
    QjsRuntime *qrt = (QjsRuntime *)ext_ctx->rt->heap;
    return qjs_context_create(qrt, QJS_PROFILE_HOOKS);
}

/* Build messages JSON array from context plan via SQLite. */
static char *build_messages_json(sqlite3 *db, int64_t session_id,
                                 const ContextPlan *plan) {
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
        return NULL;

    /* Collect JSON objects into an array string */
    size_t cap = 4096, len = 1;
    char *buf = malloc(cap);
    if (!buf) { sqlite3_finalize(stmt); return NULL; }
    buf[0] = '[';

    int first = 1;
    for (int i = plan->cut; i < plan->count; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, plan->entries[i].id);
        sqlite3_bind_int64(stmt, 2, session_id);
        if (sqlite3_step(stmt) != SQLITE_ROW) continue;
        const char *json_str = (const char *)sqlite3_column_text(stmt, 0);
        if (!json_str) continue;

        size_t jlen = strlen(json_str);
        size_t need = len + jlen + 2;
        if (need >= cap) {
            cap = need * 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); sqlite3_finalize(stmt); return NULL; }
            buf = tmp;
        }
        if (!first) buf[len++] = ',';
        memcpy(buf + len, json_str, jlen);
        len += jlen;
        first = 0;
    }
    sqlite3_finalize(stmt);
    buf[len++] = ']';
    buf[len] = '\0';
    return buf;
}

char *hook_dispatch_before_request(ExtensionCtx *ext_ctx, sqlite3 *db,
                                   int64_t session_id, const Config *cfg,
                                   const ContextPlan *plan,
                                   const ToolSchema *tools, size_t tool_count) {
    if (!ext_ctx || !ext_ctx->rt) return NULL;
    HookList *hl = &ext_ctx->hooks[HOOK_BEFORE_REQUEST];
    if (hl->count == 0) return NULL;

    /* Build messages JSON from DB */
    char *messages_json = build_messages_json(db, session_id, plan);
    if (!messages_json) return NULL;

    /* Create fresh context */
    JSContext *ctx = hook_ctx_new(ext_ctx);
    if (!ctx) { free(messages_json); return NULL; }

    /* Inject messages */
    if (qjs_set_global_json(ctx, "__hook_messages", messages_json) != 0) {
        free(messages_json);
        JS_FreeContext(ctx);
        return NULL;
    }
    free(messages_json);

    /* Call each hook function */
    for (size_t i = 0; i < hl->count; i++) {
        size_t code_len = strlen(hl->fns[i]) + 256;
        char *code = malloc(code_len);
        if (!code) continue;
        snprintf(code, code_len,
                 "(function(){"
                 "var __fn = %s;"
                 "var __r = __fn(__hook_messages);"
                 "if (__r && Array.isArray(__r)) __hook_messages = __r;"
                 "})()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_TYPE_GLOBAL);
        free(code);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            JS_FreeValue(ctx, v);
        }
    }

    /* Serialize modified messages back */
    char *modified = qjs_get_global_json(ctx, "__hook_messages");
    JS_FreeContext(ctx);

    if (!modified) return NULL;

    /* Build full request body via SQLite json_object */
    const char *model = cfg->provider.model ? cfg->provider.model : "unknown";

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

    char *body = NULL;
    sqlite3_stmt *asm_stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT json_object('model',?1,'messages',json(?2),"
        "'tools',CASE WHEN ?3 IS NOT NULL AND json_array_length(?3)>0 THEN json(?3) ELSE NULL END,"
        "'max_tokens',CASE WHEN ?4>0 THEN ?4 ELSE NULL END)",
        -1, &asm_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(asm_stmt, 1, model, -1, SQLITE_STATIC);
        sqlite3_bind_text(asm_stmt, 2, modified, -1, SQLITE_STATIC);
        if (tools_json) sqlite3_bind_text(asm_stmt, 3, tools_json, -1, SQLITE_STATIC);
        else sqlite3_bind_null(asm_stmt, 3);
        sqlite3_bind_int(asm_stmt, 4, cfg->provider.max_tokens);
        if (sqlite3_step(asm_stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(asm_stmt, 0);
            if (v) body = strdup(v);
        }
        sqlite3_finalize(asm_stmt);
    }

    free(modified);
    free(tools_json);
    return body;
}

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

    if (qjs_set_global_json(ctx, "__hook_tc_ctx", json_str) != 0) {
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
                 "var __r = __fn(__hook_tc_ctx);"
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

void hook_dispatch_observe_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                     const char *name, const char *args,
                                     const char *result) {
    if (!ext_ctx || !ext_ctx->rt) return;
    HookList *hl = &ext_ctx->hooks[HOOK_AFTER_TOOL_CALL];
    if (hl->count == 0) return;

    char *json_str = build_tc_ctx_json(db, name, args, result ? result : "");
    if (!json_str) return;

    JSContext *ctx = hook_ctx_new(ext_ctx);
    if (!ctx) { free(json_str); return; }

    if (qjs_set_global_json(ctx, "__hook_tc_ctx", json_str) != 0) {
        free(json_str); JS_FreeContext(ctx); return;
    }
    free(json_str);

    for (size_t i = 0; i < hl->count; i++) {
        size_t cl = strlen(hl->fns[i]) + 256;
        char *code = malloc(cl);
        if (!code) continue;
        snprintf(code, cl,
                 "(function(){ var __fn = %s; __fn(__hook_tc_ctx); })()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_TYPE_GLOBAL);
        free(code);
        if (JS_IsException(v))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, v);
    }

    JS_FreeContext(ctx);
}
