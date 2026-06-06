/* Host-provided JS functions for CClaw's mquickjs runtime */

/* JsHostCtx — set as JSContext opaque. Must match tool_js.h. */
typedef struct {
    int instruction_count;
    int instruction_limit;
    char **allowed_hosts;
    size_t allowed_hosts_count;
    void *tool_registry;  /* ToolRegistry* — V116: for callTool dispatch */
    int call_depth;       /* V116: re-entrancy depth counter */
} JsHostCtx;

/* Forward declarations for tool dispatch (defined in tools.c) */
typedef char *(*ToolHandlerFn)(const char *arguments, void *user_data);
typedef struct {
    char *name;
    char *description;
    char *parameters_json;
    ToolHandlerFn handler;
    void *user_data;
    void (*free_fn)(void *);
} ToolEntry;
typedef struct {
    ToolEntry entries[32];
    size_t count;
} ToolRegistry;
static ToolEntry *tools_lookup_host(ToolRegistry *reg, const char *name) {
    if (!reg || !name) return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    }
    return NULL;
}

#define CALL_TOOL_MAX_DEPTH 8

/* V116: cclaw.callTool(name, args) — synchronous dispatch into C tool registry.
 * Depth limit 8 to prevent infinite recursion. */
JSValue js_call_tool(JSContext *ctx, JSValue *this_val,
                     int argc, JSValue *argv)
{
    (void)this_val;

    JsHostCtx *hctx = (JsHostCtx *)JS_GetContextOpaque(ctx);
    if (!hctx || !hctx->tool_registry)
        return JS_ThrowTypeError(ctx, "callTool: no tool registry available");

    if (hctx->call_depth >= CALL_TOOL_MAX_DEPTH)
        return JS_ThrowTypeError(ctx, "callTool: max recursion depth (%d) exceeded",
                                 CALL_TOOL_MAX_DEPTH);

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "callTool: name argument required");

    JSCStringBuf name_buf;
    const char *name = JS_ToCString(ctx, argv[0], &name_buf);
    if (!name)
        return JS_ThrowTypeError(ctx, "callTool: name must be a string");

    /* Serialize args to JSON string */
    char *args_json = NULL;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        /* JSON.stringify the args object — use eval to access JSON builtin */
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__callTool_tmp", argv[1]);
        const char *code = "JSON.stringify(globalThis.__callTool_tmp)";
        JSValue json_val = JS_Eval(ctx, code, strlen(code), "<callTool>", JS_EVAL_RETVAL);
        if (JS_IsException(json_val)) {
            /* Fallback: try toString for simple types */
            JSCStringBuf sbuf;
            const char *s = JS_ToCString(ctx, argv[1], &sbuf);
            if (s) {
                size_t len = strlen(s);
                args_json = (char *)malloc(len + 1);
                if (args_json) memcpy(args_json, s, len + 1);
            }
        } else {
            JSCStringBuf json_buf;
            const char *json_str = JS_ToCString(ctx, json_val, &json_buf);
            if (json_str) {
                size_t len = strlen(json_str);
                args_json = (char *)malloc(len + 1);
                if (args_json) memcpy(args_json, json_str, len + 1);
            }
        }
        if (!args_json) {
            args_json = (char *)malloc(3);
            if (args_json) memcpy(args_json, "{}", 3);
        }
    } else {
        args_json = (char *)malloc(3);
        if (args_json) memcpy(args_json, "{}", 3);
    }
    if (!args_json)
        return JS_ThrowTypeError(ctx, "callTool: out of memory");

    /* Look up tool */
    ToolRegistry *reg = (ToolRegistry *)hctx->tool_registry;
    ToolEntry *entry = tools_lookup_host(reg, name);
    if (!entry) {
        char msg[128];
        snprintf(msg, sizeof(msg), "callTool: tool '%s' not found", name);
        free(args_json);
        return JS_ThrowTypeError(ctx, "%s", msg);
    }

    /* Dispatch with depth tracking */
    hctx->call_depth++;
    char *result = entry->handler(args_json, entry->user_data);
    hctx->call_depth--;
    free(args_json);

    if (!result) {
        result = (char *)malloc(24);
        if (result) memcpy(result, "error: tool returned NULL", 24);
    }

    JSValue ret = JS_NewString(ctx, result ? result : "error: OOM");
    free(result);
    return ret;
}

/* Date.now() — milliseconds since epoch */
JSValue js_date_now(JSContext *ctx, JSValue *this_val,
                    int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    return JS_NewFloat64(ctx, ms);
}

/* Result struct from js_http_fetch_exec (defined in js_http_fetch.c) */
typedef struct {
    int status;
    char *body;
    size_t body_len;
    char *error;
} JsHttpResult;

extern JsHttpResult js_http_fetch_exec(const char *url, const char *method,
                                       const char *body,
                                       char **allowed_hosts, size_t hosts_count);
extern void js_http_result_free(JsHttpResult *r);

/* V46: sanitize helpers */
extern size_t html_strip_tags(const char *src, char *dst, size_t dst_cap);
extern char *wrap_external_content(const char *content, size_t len, const char *source);

/* V38: http_fetch(url, opts) — sole network path from JS runtime.
 * opts.sanitize: true → strip HTML + wrap with anti-spoofing boundaries (V46).
 * Returns {status, body} object or throws on error. */
JSValue js_http_fetch(JSContext *ctx, JSValue *this_val,
                      int argc, JSValue *argv)
{
    (void)this_val;

    JsHostCtx *hctx = (JsHostCtx *)JS_GetContextOpaque(ctx);

    char **hosts = NULL;
    size_t hosts_count = 0;
    if (hctx) {
        hosts = hctx->allowed_hosts;
        hosts_count = hctx->allowed_hosts_count;
    }

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http_fetch: url argument required");

    JSCStringBuf url_buf;
    const char *url = JS_ToCString(ctx, argv[0], &url_buf);
    if (!url)
        return JS_ThrowTypeError(ctx, "http_fetch: url must be a string");

    /* Parse opts */
    const char *method = NULL;
    const char *body = NULL;
    int sanitize = 0;
    JSCStringBuf method_buf, body_buf;

    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(m) && !JS_IsNull(m))
            method = JS_ToCString(ctx, m, &method_buf);
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(b) && !JS_IsNull(b))
            body = JS_ToCString(ctx, b, &body_buf);
        JSValue s = JS_GetPropertyStr(ctx, argv[1], "sanitize");
        if (s == JS_TRUE)
            sanitize = 1;
    }

    JsHttpResult r = js_http_fetch_exec(url, method, body, hosts, hosts_count);

    if (r.status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "http_fetch: %s", r.error ? r.error : "unknown error");
        js_http_result_free(&r);
        return JS_ThrowTypeError(ctx, "%s", msg);
    }

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, (int32_t)r.status));

    if (r.body && sanitize) {
        /* V46: html_strip_tags + wrap with randomized anti-spoofing boundaries */
        size_t cap = r.body_len + 1;
        char *text = (char *)malloc(cap);
        if (text) {
            size_t tlen = html_strip_tags(r.body, text, cap);
            char *wrapped = wrap_external_content(text, tlen, "http_fetch");
            if (wrapped) {
                JS_SetPropertyStr(ctx, result, "body",
                                  JS_NewStringLen(ctx, wrapped, strlen(wrapped)));
                free(wrapped);
            } else {
                JS_SetPropertyStr(ctx, result, "body",
                                  JS_NewStringLen(ctx, text, tlen));
            }
            free(text);
        } else {
            JS_SetPropertyStr(ctx, result, "body",
                              JS_NewStringLen(ctx, r.body, r.body_len));
        }
    } else if (r.body) {
        JS_SetPropertyStr(ctx, result, "body",
                          JS_NewStringLen(ctx, r.body, r.body_len));
    } else {
        JS_SetPropertyStr(ctx, result, "body", JS_NewString(ctx, ""));
    }

    js_http_result_free(&r);
    return result;
}
