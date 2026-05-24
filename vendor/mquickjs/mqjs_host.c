/* Host-provided JS functions for CClaw's mquickjs runtime */

/* JsHostCtx — set as JSContext opaque. Defined in tool_js.h. */
typedef struct {
    int instruction_count;
    int instruction_limit;
    char **allowed_hosts;
    size_t allowed_hosts_count;
} JsHostCtx;

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

/* V38: http_fetch(url, opts) — sole network path from JS runtime.
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
    JSCStringBuf method_buf, body_buf;

    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(m) && !JS_IsNull(m))
            method = JS_ToCString(ctx, m, &method_buf);
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(b) && !JS_IsNull(b))
            body = JS_ToCString(ctx, b, &body_buf);
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
    if (r.body)
        JS_SetPropertyStr(ctx, result, "body",
                          JS_NewStringLen(ctx, r.body, r.body_len));
    else
        JS_SetPropertyStr(ctx, result, "body", JS_NewString(ctx, ""));
    js_http_result_free(&r);
    return result;
}
