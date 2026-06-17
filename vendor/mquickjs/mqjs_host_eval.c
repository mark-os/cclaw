/* Host-provided JS functions for the eval profile (forked sandboxed evaluator).
 * ALL functions static — this links into the same binary as the main profile. */

#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

/* JsHostCtx — set as JSContext opaque */
typedef struct {
    int instruction_count;
    int instruction_limit;
    char **allowed_hosts;
    size_t allowed_hosts_count;
} JsHostCtx;

/* --- helpers --- */

static JSValue make_err_tuple(JSContext *ctx, int err) {
    JSValue arr = JS_NewArray(ctx, 2);
    if (JS_IsException(arr)) return arr;
    JS_SetPropertyUint32(ctx, arr, 0, JS_NULL);
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, (int32_t)err));
    return arr;
}

/* --- Date.now() --- */

static JSValue js_date_now(JSContext *ctx, JSValue *this_val,
                           int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    return JS_NewFloat64(ctx, ms);
}

/* --- http_fetch --- */

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
extern size_t html_strip_tags(const char *src, char *dst, size_t dst_cap);
extern char *wrap_external_content(const char *content, size_t len, const char *source);

static JSValue js_http_fetch(JSContext *ctx, JSValue *this_val,
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
        return JS_ThrowTypeError(ctx, "http_request: url argument required");

    JSCStringBuf url_buf;
    const char *url = JS_ToCString(ctx, argv[0], &url_buf);
    if (!url)
        return JS_ThrowTypeError(ctx, "http_request: url must be a string");

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
        snprintf(msg, sizeof(msg), "http_request: %s", r.error ? r.error : "unknown error");
        js_http_result_free(&r);
        return JS_ThrowTypeError(ctx, "%s", msg);
    }

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, (int32_t)r.status));

    if (r.body && sanitize) {
        size_t cap = r.body_len + 1;
        char *text = (char *)malloc(cap);
        if (text) {
            size_t tlen = html_strip_tags(r.body, text, cap);
            char *wrapped = wrap_external_content(text, tlen, "http_request");
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

/* --- fs functions --- */

static JSValue js_fs_readFile(JSContext *ctx, JSValue *this_val,
                              int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fs.readFile: path required");

    JSCStringBuf pbuf;
    const char *path = JS_ToCString(ctx, argv[0], &pbuf);
    if (!path)
        return JS_ThrowTypeError(ctx, "fs.readFile: path must be a string");

    FILE *f = fopen(path, "rb");
    if (!f)
        return make_err_tuple(ctx, errno);

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        return make_err_tuple(ctx, ENOMEM);
    }

    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    JSValue arr = JS_NewArray(ctx, 2);
    if (JS_IsException(arr)) { free(buf); return arr; }
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewStringLen(ctx, buf, nread));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    free(buf);
    return arr;
}

static JSValue js_fs_writeFile(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fs.writeFile: path and content required");

    JSCStringBuf pbuf;
    const char *path = JS_ToCString(ctx, argv[0], &pbuf);
    if (!path)
        return JS_ThrowTypeError(ctx, "fs.writeFile: path must be a string");

    /* Consume content pointer before any JS allocation */
    JSCStringBuf cbuf;
    size_t clen = 0;
    const char *content = JS_ToCStringLen(ctx, &clen, argv[1], &cbuf);
    if (!content)
        return JS_ThrowTypeError(ctx, "fs.writeFile: content must be a string");

    FILE *f = fopen(path, "wb");
    if (!f)
        return make_err_tuple(ctx, errno);

    size_t written = fwrite(content, 1, clen, f);
    int err = ferror(f) ? errno : 0;
    fclose(f);

    if (err)
        return make_err_tuple(ctx, err);

    JSValue arr = JS_NewArray(ctx, 2);
    if (JS_IsException(arr)) return arr;
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, (int32_t)written));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

static JSValue js_fs_readDir(JSContext *ctx, JSValue *this_val,
                             int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fs.readDir: path required");

    JSCStringBuf pbuf;
    const char *path = JS_ToCString(ctx, argv[0], &pbuf);
    if (!path)
        return JS_ThrowTypeError(ctx, "fs.readDir: path must be a string");

    DIR *d = opendir(path);
    if (!d)
        return make_err_tuple(ctx, errno);

    JSValue names = JS_NewArray(ctx, 0);
    if (JS_IsException(names)) { closedir(d); return names; }

    struct dirent *ent;
    uint32_t idx = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        JS_SetPropertyUint32(ctx, names, idx++, JS_NewString(ctx, ent->d_name));
    }
    closedir(d);

    JSValue arr = JS_NewArray(ctx, 2);
    if (JS_IsException(arr)) return arr;
    JS_SetPropertyUint32(ctx, arr, 0, names);
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

static JSValue js_fs_stat(JSContext *ctx, JSValue *this_val,
                          int argc, JSValue *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fs.stat: path required");

    JSCStringBuf pbuf;
    const char *path = JS_ToCString(ctx, argv[0], &pbuf);
    if (!path)
        return JS_ThrowTypeError(ctx, "fs.stat: path must be a string");

    struct stat st;
    if (stat(path, &st) != 0)
        return make_err_tuple(ctx, errno);

    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return obj;
    JS_SetPropertyStr(ctx, obj, "size", JS_NewFloat64(ctx, (double)st.st_size));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, (int32_t)st.st_mode));
    JS_SetPropertyStr(ctx, obj, "mtime", JS_NewFloat64(ctx, (double)st.st_mtime));
    JS_SetPropertyStr(ctx, obj, "isDir", JS_NewBool(S_ISDIR(st.st_mode)));

    JSValue arr = JS_NewArray(ctx, 2);
    if (JS_IsException(arr)) return arr;
    JS_SetPropertyUint32(ctx, arr, 0, obj);
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

static JSValue js_fs_cwd(JSContext *ctx, JSValue *this_val,
                         int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf)))
        return make_err_tuple(ctx, errno);

    JSValue arr = JS_NewArray(ctx, 2);
    if (JS_IsException(arr)) return arr;
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, buf));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

/* js_call_tool — not used in eval profile, but referenced by Date class def.
 * Provide a stub that throws. */
static JSValue js_call_tool(JSContext *ctx, JSValue *this_val,
                            int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "callTool not available in eval context");
}
