/* T258: beforeRequest hook dispatch.
 * V112: serialize messages to JS array, call each registered hook in order,
 * deserialize modified array back. Skip on throw. */
#define _POSIX_C_SOURCE 200809L
#include "hook_dispatch.h"
#include "json_escape.h"
#include "log.h"
#include <cJSON.h>
#include <mquickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build a JS messages array from the plan entries (from cut onward).
 * Injects into global scope as __hook_messages. */
static int build_messages_array(JSContext *ctx, sqlite3 *db,
                                int64_t session_id, const ContextPlan *plan) {
    const char *sql = "SELECT role, content, tool_calls, tool_call_id "
                      "FROM entries WHERE id=? AND session_id=?;";
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

        int role = sqlite3_column_int(stmt, 0);
        const char *content = (const char *)sqlite3_column_text(stmt, 1);
        const char *tool_calls_raw = (const char *)sqlite3_column_text(stmt, 2);
        const char *tool_call_id = (const char *)sqlite3_column_text(stmt, 3);

        /* Build message object as JSON, then parse in JS */
        cJSON *msg = cJSON_CreateObject();
        const char *role_str;
        switch (role) {
            case 0: role_str = "system"; break;
            case 1: role_str = "user"; break;
            case 2: role_str = "assistant"; break;
            case 3: role_str = "tool"; break;
            default: role_str = "user"; break;
        }
        cJSON_AddStringToObject(msg, "role", role_str);

        if (role == 3) {
            /* tool result */
            if (tool_call_id) cJSON_AddStringToObject(msg, "tool_call_id", tool_call_id);
            cJSON_AddStringToObject(msg, "content", content ? content : "");
        } else if (role == 2) {
            /* assistant */
            if (content) cJSON_AddStringToObject(msg, "content", content);
            else cJSON_AddNullToObject(msg, "content");
            if (tool_calls_raw) {
                cJSON *tc = cJSON_Parse(tool_calls_raw);
                if (tc) cJSON_AddItemToObject(msg, "tool_calls", tc);
            }
        } else {
            cJSON_AddStringToObject(msg, "content", content ? content : "");
        }

        char *json_str = cJSON_PrintUnformatted(msg);
        cJSON_Delete(msg);
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
        free(json_str);
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

    /* Tools fragment */
    char *tools_json = NULL;
    if (tools && tool_count > 0) {
        cJSON *arr = cJSON_CreateArray();
        for (size_t i = 0; i < tool_count; i++) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", tools[i].name);
            if (tools[i].description)
                cJSON_AddStringToObject(fn, "description", tools[i].description);
            if (tools[i].parameters_json) {
                cJSON *params = cJSON_Parse(tools[i].parameters_json);
                if (params) cJSON_AddItemToObject(fn, "parameters", params);
            }
            cJSON_AddItemToObject(tool, "function", fn);
            cJSON_AddItemToArray(arr, tool);
        }
        tools_json = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
    }

    /* Assemble: {"model":"<model>","messages":<messages>[,"tools":<tools>][,"max_tokens":<n>]} */
    size_t needed = 32 + strlen(model) + strlen(messages_json) +
                    (tools_json ? strlen(tools_json) + 16 : 0) + 32;
    char *body = malloc(needed);
    if (!body) { free(messages_json); free(tools_json); return NULL; }

    int written;
    if (tools_json && cfg->provider.max_tokens > 0) {
        written = snprintf(body, needed,
                           "{\"model\":\"%s\",\"messages\":%s,\"tools\":%s,\"max_tokens\":%d}",
                           model, messages_json, tools_json, cfg->provider.max_tokens);
    } else if (tools_json) {
        written = snprintf(body, needed,
                           "{\"model\":\"%s\",\"messages\":%s,\"tools\":%s}",
                           model, messages_json, tools_json);
    } else if (cfg->provider.max_tokens > 0) {
        written = snprintf(body, needed,
                           "{\"model\":\"%s\",\"messages\":%s,\"max_tokens\":%d}",
                           model, messages_json, cfg->provider.max_tokens);
    } else {
        written = snprintf(body, needed,
                           "{\"model\":\"%s\",\"messages\":%s}",
                           model, messages_json);
    }
    (void)written;

    free(messages_json);
    free(tools_json);

    LOG_DEBUG(cfg, "beforeRequest hook: modified request (%zu bytes)", strlen(body));
    return body;
}

/* V113: beforeToolCall hook dispatch.
 * Calls each hook with {name, args}. If any returns {block:true}, return error. */
char *hook_dispatch_before_tool_call(ExtensionCtx *ext_ctx, const char *name,
                                     const char *args) {
    if (!ext_ctx || !ext_ctx->rt || !ext_ctx->rt->ctx) return NULL;
    HookList *hl = &ext_ctx->hooks[HOOK_BEFORE_TOOL_CALL];
    if (hl->count == 0) return NULL;

    JSContext *ctx = (JSContext *)ext_ctx->rt->ctx;

    /* Build context object as JSON */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", name ? name : "");
    if (args) {
        cJSON *parsed = cJSON_Parse(args);
        if (parsed) cJSON_AddItemToObject(obj, "args", parsed);
        else cJSON_AddStringToObject(obj, "args", args);
    } else {
        cJSON_AddNullToObject(obj, "args");
    }
    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json_str) return NULL;

    /* Set as global for hooks to access */
    size_t code_len = strlen(json_str) + 64;
    char *setup = malloc(code_len);
    if (!setup) { free(json_str); return NULL; }
    snprintf(setup, code_len, "globalThis.__hook_tc_ctx = %s;", json_str);
    free(json_str);
    JSValue sv = JS_Eval(ctx, setup, strlen(setup), "<hook>", 0);
    free(setup);
    if (JS_IsException(sv)) return NULL;

    /* Call each hook — first block wins */
    for (size_t i = 0; i < hl->count; i++) {
        size_t cl = strlen(hl->fns[i]) + 256;
        char *code = malloc(cl);
        if (!code) continue;
        snprintf(code, cl,
                 "(function(){"
                 "var __fn = %s;"
                 "var __r = __fn(globalThis.__hook_tc_ctx);"
                 "if (__r && __r.block) return JSON.stringify(__r);"
                 "return '';"
                 "})()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_RETVAL);
        free(code);
        if (JS_IsException(v)) {
            JSValue exc = JS_GetException(ctx);
            (void)exc;
            continue;
        }
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, v, &buf);
        if (str && str[0]) {
            /* Blocked — extract reason */
            cJSON *res = cJSON_Parse(str);
            const char *reason = "blocked by extension hook";
            if (res) {
                cJSON *r = cJSON_GetObjectItem(res, "reason");
                if (r && r->valuestring) reason = r->valuestring;
                char *ret = malloc(strlen(reason) + 16);
                if (ret) snprintf(ret, strlen(reason) + 16, "error: %s", reason);
                cJSON_Delete(res);
                return ret;
            }
            char *ret = strdup("error: blocked by extension hook");
            return ret;
        }
    }
    return NULL;
}

/* V114: afterToolCall hook dispatch.
 * Calls each hook with {name, args, result}. Returns replacement if any. */
char *hook_dispatch_after_tool_call(ExtensionCtx *ext_ctx, const char *name,
                                    const char *args, const char *result) {
    if (!ext_ctx || !ext_ctx->rt || !ext_ctx->rt->ctx) return NULL;
    HookList *hl = &ext_ctx->hooks[HOOK_AFTER_TOOL_CALL];
    if (hl->count == 0) return NULL;

    JSContext *ctx = (JSContext *)ext_ctx->rt->ctx;

    /* Build context object */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", name ? name : "");
    if (args) {
        cJSON *parsed = cJSON_Parse(args);
        if (parsed) cJSON_AddItemToObject(obj, "args", parsed);
        else cJSON_AddStringToObject(obj, "args", args);
    } else {
        cJSON_AddNullToObject(obj, "args");
    }
    cJSON_AddStringToObject(obj, "result", result ? result : "");
    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json_str) return NULL;

    size_t code_len = strlen(json_str) + 64;
    char *setup = malloc(code_len);
    if (!setup) { free(json_str); return NULL; }
    snprintf(setup, code_len, "globalThis.__hook_tc_ctx = %s;", json_str);
    free(json_str);
    JSValue sv = JS_Eval(ctx, setup, strlen(setup), "<hook>", 0);
    free(setup);
    if (JS_IsException(sv)) return NULL;

    char *replacement = NULL;

    /* Call each hook in order — each sees previous result */
    for (size_t i = 0; i < hl->count; i++) {
        size_t cl = strlen(hl->fns[i]) + 192;
        char *code = malloc(cl);
        if (!code) continue;
        snprintf(code, cl,
                 "(function(){"
                 "var __fn = %s;"
                 "var __r = __fn(globalThis.__hook_tc_ctx);"
                 "if (__r && typeof __r.result === 'string') {"
                 "  globalThis.__hook_tc_ctx.result = __r.result;"
                 "  return __r.result;"
                 "}"
                 "return '';"
                 "})()",
                 hl->fns[i]);
        JSValue v = JS_Eval(ctx, code, strlen(code), "<hook>", JS_EVAL_RETVAL);
        free(code);
        if (JS_IsException(v)) {
            JSValue exc = JS_GetException(ctx);
            (void)exc;
            continue;
        }
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, v, &buf);
        if (str && str[0]) {
            free(replacement);
            replacement = strdup(str);
        }
    }
    return replacement;
}
