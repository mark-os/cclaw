#define _POSIX_C_SOURCE 200809L
#include "tool_js.h"
#include <cJSON.h>
#include <mquickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* V5: 1MB heap cap, 10M instruction limit */
#define JS_HEAP_SIZE (1024 * 1024)
#define JS_MAX_INSTRUCTIONS 10000000

extern const JSSTDLibraryDef js_std_library;

typedef struct {
    int count;
} InterruptState;

static int interrupt_handler(JSContext *ctx, void *opaque) {
    (void)ctx;
    InterruptState *state = (InterruptState *)opaque;
    state->count++;
    return state->count > JS_MAX_INSTRUCTIONS;
}

/* Eval JS code in a fresh sandboxed context. Returns heap-allocated result. */
static char *js_eval_code(const char *code) {
    size_t code_len = strlen(code);

    void *heap = malloc(JS_HEAP_SIZE);
    if (!heap) return strdup("error: out of memory");

    JSContext *ctx = JS_NewContext(heap, JS_HEAP_SIZE, &js_std_library);
    if (!ctx) { free(heap); return strdup("error: JS context creation failed"); }

    InterruptState istate = {0};
    JS_SetInterruptHandler(ctx, interrupt_handler);
    JS_SetContextOpaque(ctx, &istate);

    JSValue val = JS_Eval(ctx, code, code_len, "<eval>", JS_EVAL_RETVAL);

    char *result;
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        if (msg) {
            size_t len = strlen(msg) + 16;
            result = malloc(len);
            if (result) snprintf(result, len, "error: %s", msg);
            else result = strdup("error: OOM");
        } else {
            result = strdup("error: exception (no message)");
        }
    } else if (istate.count > JS_MAX_INSTRUCTIONS) {
        result = strdup("error: instruction limit exceeded (10M)");
    } else if (JS_IsUndefined(val)) {
        result = strdup("undefined");
    } else if (JS_IsNull(val)) {
        result = strdup("null");
    } else {
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, val, &buf);
        result = str ? strdup(str) : strdup("error: cannot convert result to string");
    }

    JS_FreeContext(ctx);
    free(heap);
    return result;
}

/* Eval JS code in a persistent session runtime. Returns heap-allocated result. */
static char *js_eval_in_runtime(JsSessionRuntime *rt, const char *code) {
    if (!rt || !rt->ctx) return js_eval_code(code);

    JSContext *ctx = (JSContext *)rt->ctx;
    size_t code_len = strlen(code);

    JSValue val = JS_Eval(ctx, code, code_len, "<tool>", JS_EVAL_RETVAL);

    char *result;
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        if (msg) {
            size_t len = strlen(msg) + 16;
            result = malloc(len);
            if (result) snprintf(result, len, "error: %s", msg);
            else result = strdup("error: OOM");
        } else {
            result = strdup("error: exception (no message)");
        }
    } else if (JS_IsUndefined(val)) {
        result = strdup("undefined");
    } else if (JS_IsNull(val)) {
        result = strdup("null");
    } else {
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, val, &buf);
        result = str ? strdup(str) : strdup("error: cannot convert result to string");
    }

    return result;
}

static const char *JSEVAL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"code\":{\"type\":\"string\",\"description\":\"JavaScript code to execute\"}"
    "},\"required\":[\"code\"]}";

char *tool_js_eval_handler(const char *arguments, void *user_data) {
    (void)user_data;

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(json, "code");
    if (!cJSON_IsString(code_item) || !code_item->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: missing or empty 'code' field");
    }

    char *result = js_eval_code(code_item->valuestring);
    cJSON_Delete(json);
    return result;
}

int tool_js_eval_register(ToolRegistry *reg) {
    return tools_register(reg, "js_eval",
                          "Execute JavaScript code in a sandboxed environment and return the result",
                          JSEVAL_PARAMS_JSON, tool_js_eval_handler, NULL);
}

/* --- js_define_tool --- */

static const char *JSDEFINE_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Tool name (snake_case)\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"What the tool does\"},"
    "\"parameters\":{\"type\":\"string\",\"description\":\"JSON Schema for tool parameters\"},"
    "\"code\":{\"type\":\"string\",\"description\":\"JS function body. Receives 'args' object, must return a string.\"}"
    "},\"required\":[\"name\",\"code\"]}";

/* User data for JS-defined tool handler: code + runtime pointer */
typedef struct {
    char *code;
    JsSessionRuntime *rt;
} JsToolData;

/* Handler for JS-defined tools. user_data points to JsToolData. */
static char *js_defined_tool_handler(const char *arguments, void *user_data) {
    JsToolData *td = (JsToolData *)user_data;
    if (!td || !td->code) return strdup("error: no code for this tool");

    /* Wrap: (function(args){<code>})(JSON.parse('<escaped_args>')) */
    cJSON *args_json = cJSON_Parse(arguments);
    char *args_str = args_json ? cJSON_PrintUnformatted(args_json) : strdup("{}");
    cJSON_Delete(args_json);

    /* Escape single quotes in args for embedding in JS string literal */
    size_t args_len = strlen(args_str);
    size_t escaped_cap = args_len * 2 + 1;
    char *escaped = malloc(escaped_cap);
    if (!escaped) { free(args_str); return strdup("error: OOM"); }
    size_t j = 0;
    for (size_t i = 0; i < args_len; i++) {
        if (args_str[i] == '\'' || args_str[i] == '\\') {
            escaped[j++] = '\\';
        }
        escaped[j++] = args_str[i];
    }
    escaped[j] = '\0';
    free(args_str);

    size_t wrap_len = strlen(td->code) + j + 64;
    char *wrapped = malloc(wrap_len);
    if (!wrapped) { free(escaped); return strdup("error: OOM"); }
    snprintf(wrapped, wrap_len, "(function(args){%s})(JSON.parse('%s'))", td->code, escaped);
    free(escaped);

    char *result = js_eval_in_runtime(td->rt, wrapped);
    free(wrapped);
    return result;
}

/* Persist a JS tool definition to DB */
static int js_tool_persist(sqlite3 *db, int64_t session_id, const char *name,
                           const char *description, const char *parameters_json,
                           const char *code) {
    const char *sql =
        "INSERT OR REPLACE INTO js_tools (session_id, name, description, parameters_json, code)"
        " VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, description ? description : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, parameters_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, code, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Free function for JsToolData */
static void js_tool_data_free(void *user_data) {
    JsToolData *td = (JsToolData *)user_data;
    if (td) { free(td->code); free(td); }
}

/* Register a single JS-defined tool into the registry with shared runtime. */
static int js_tool_register_one(ToolRegistry *reg, const char *name,
                                const char *description, const char *parameters_json,
                                const char *code, JsSessionRuntime *rt) {
    /* Check if already registered (re-define overwrites) */
    ToolEntry *existing = tools_lookup(reg, name);
    if (existing) {
        JsToolData *td = (JsToolData *)existing->user_data;
        free(td->code);
        td->code = strdup(code);
        td->rt = rt;
        free(existing->description);
        free(existing->parameters_json);
        existing->description = description ? strdup(description) : NULL;
        existing->parameters_json = parameters_json ? strdup(parameters_json) : NULL;
        existing->handler = js_defined_tool_handler;
        return 0;
    }
    JsToolData *td = malloc(sizeof(JsToolData));
    if (!td) return -1;
    td->code = strdup(code);
    td->rt = rt;
    if (!td->code) { free(td); return -1; }
    int rc = tools_register(reg, name, description, parameters_json,
                            js_defined_tool_handler, td);
    if (rc == 0) {
        ToolEntry *e = tools_lookup(reg, name);
        if (e) e->free_fn = js_tool_data_free;
    }
    return rc;
}

/* Replay a tool definition into the persistent runtime.
 * Registers as global function AND executes once to restore side effects. */
static void js_runtime_replay_tool(JsSessionRuntime *rt, const char *name, const char *code) {
    if (!rt || !rt->ctx) return;
    /* Register and call: globalThis.<name> = function(args){<code>}; <name>({}) */
    size_t wrap_len = strlen(name) * 2 + strlen(code) + 80;
    char *wrapped = malloc(wrap_len);
    if (!wrapped) return;
    snprintf(wrapped, wrap_len,
             "globalThis.%s = function(args){%s}; %s({})", name, code, name);
    JSContext *ctx = (JSContext *)rt->ctx;
    JS_Eval(ctx, wrapped, strlen(wrapped), "<replay>", 0);
    free(wrapped);
}

char *tool_js_define_handler(const char *arguments, void *user_data) {
    JsDefineCtx *ctx = (JsDefineCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->reg) return strdup("error: js_define_tool not configured");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(json, "name");
    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(json, "code");
    if (!cJSON_IsString(name_item) || !name_item->valuestring[0] ||
        !cJSON_IsString(code_item) || !code_item->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: 'name' and 'code' are required non-empty strings");
    }

    const char *name = name_item->valuestring;
    const char *code = code_item->valuestring;

    cJSON *desc_item = cJSON_GetObjectItemCaseSensitive(json, "description");
    const char *description = cJSON_IsString(desc_item) ? desc_item->valuestring : NULL;

    cJSON *params_item = cJSON_GetObjectItemCaseSensitive(json, "parameters");
    const char *parameters = cJSON_IsString(params_item) ? params_item->valuestring : "{}";

    /* Persist to DB */
    if (js_tool_persist(ctx->db, ctx->session_id, name, description, parameters, code) != 0) {
        cJSON_Delete(json);
        return strdup("error: failed to persist tool definition");
    }

    /* Register in live registry */
    if (js_tool_register_one(ctx->reg, name, description, parameters, code, ctx->rt) != 0) {
        cJSON_Delete(json);
        return strdup("error: failed to register tool");
    }

    /* Replay into shared runtime so other tools can reference it */
    js_runtime_replay_tool(ctx->rt, name, code);

    cJSON_Delete(json);

    size_t rlen = strlen(name) + 32;
    char *result = malloc(rlen);
    if (result) snprintf(result, rlen, "tool '%s' defined", name);
    else result = strdup("ok");
    return result;
}

int tool_js_define_register(ToolRegistry *reg, JsDefineCtx *ctx) {
    return tools_register(reg, "js_define_tool",
                          "Define a new tool from JavaScript code. The code receives an 'args' object and must return a string.",
                          JSDEFINE_PARAMS_JSON, tool_js_define_handler, ctx);
}

/* --- Session runtime --- */

JsSessionRuntime *js_runtime_create(void) {
    JsSessionRuntime *rt = malloc(sizeof(JsSessionRuntime));
    if (!rt) return NULL;
    rt->heap = malloc(JS_HEAP_SIZE);
    if (!rt->heap) { free(rt); return NULL; }
    rt->ctx = JS_NewContext(rt->heap, JS_HEAP_SIZE, &js_std_library);
    if (!rt->ctx) { free(rt->heap); free(rt); return NULL; }
    return rt;
}

void js_runtime_destroy(JsSessionRuntime *rt) {
    if (!rt) return;
    if (rt->ctx) JS_FreeContext((JSContext *)rt->ctx);
    free(rt->heap);
    free(rt);
}

int tool_js_load_session(sqlite3 *db, int64_t session_id, ToolRegistry *reg,
                         JsSessionRuntime *rt) {
    const char *sql = "SELECT name, description, parameters_json, code FROM js_tools WHERE session_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);

    int loaded = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *desc = (const char *)sqlite3_column_text(stmt, 1);
        const char *params = (const char *)sqlite3_column_text(stmt, 2);
        const char *code = (const char *)sqlite3_column_text(stmt, 3);
        if (name && code) {
            if (js_tool_register_one(reg, name, desc, params, code, rt) == 0) {
                /* Replay into shared runtime */
                js_runtime_replay_tool(rt, name, code);
                loaded++;
            }
        }
    }
    sqlite3_finalize(stmt);
    return loaded;
}
