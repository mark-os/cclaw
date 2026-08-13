/* Host C functions for in-process qjs eval (the SBX_JS --run-tool broker child).
 * Registered into the QuickJS context as globals: http_request, fs.*, console.log, print. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qjs_helpers.h"
#include "qjs_xml.h"
#include "js_http_fetch.h"
#include "external_content.h"
#include "tool_web_fetch.h"
#include "tool_js.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── http_request(url[, opts]) ─────────────────────────────────── */

static JSValue js_http_request(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http_request: url argument required");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url)
        return JS_ThrowTypeError(ctx, "http_request: url must be a string");

    const char *method = NULL, *body = NULL;
    int markdownify = 0;
    char **hdr_lines = NULL;       /* owned "Name: Value" strings */
    const char **hdr_ptrs = NULL;  /* NULL-terminated view handed to fetch */
    size_t hdr_n = 0;

    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(m) && !JS_IsNull(m)) method = JS_ToCString(ctx, m);
        JS_FreeValue(ctx, m);
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(b) && !JS_IsNull(b)) body = JS_ToCString(ctx, b);
        JS_FreeValue(ctx, b);
        JSValue s = JS_GetPropertyStr(ctx, argv[1], "markdownify");
        if (JS_ToBool(ctx, s)) markdownify = 1;
        JS_FreeValue(ctx, s);

        /* headers: {Name: Value, ...} → NULL-terminated "Name: Value" array */
        JSValue h = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(h)) {
            JSPropertyEnum *tab = NULL;
            uint32_t n = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &n, h,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                hdr_lines = calloc(n, sizeof(*hdr_lines));
                hdr_ptrs = calloc((size_t)n + 1, sizeof(*hdr_ptrs));
                for (uint32_t i = 0; i < n && hdr_lines && hdr_ptrs; i++) {
                    const char *key = JS_AtomToCString(ctx, tab[i].atom);
                    JSValue v = JS_GetProperty(ctx, h, tab[i].atom);
                    const char *val = JS_ToCString(ctx, v);
                    if (key && val) {
                        size_t len = strlen(key) + strlen(val) + 3;
                        char *line = malloc(len);
                        if (line) {
                            snprintf(line, len, "%s: %s", key, val);
                            hdr_lines[hdr_n] = line;
                            hdr_ptrs[hdr_n] = line;
                            hdr_n++;
                        }
                    }
                    if (key) JS_FreeCString(ctx, key);
                    if (val) JS_FreeCString(ctx, val);
                    JS_FreeValue(ctx, v);
                }
                if (hdr_ptrs) hdr_ptrs[hdr_n] = NULL;
                JS_FreePropertyEnum(ctx, tab, n);
            }
        }
        JS_FreeValue(ctx, h);
    }

    JsHttpResult r = js_http_fetch_exec(url, method, body, hdr_ptrs);
    JS_FreeCString(ctx, url);
    if (method) JS_FreeCString(ctx, method);
    if (body) JS_FreeCString(ctx, body);
    for (size_t i = 0; i < hdr_n; i++) free(hdr_lines[i]);
    free(hdr_lines);
    free(hdr_ptrs);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, r.status));

    if (r.status < 0) {
        /* Network/transport failure: return {status:-1, body:"", error} instead
         * of throwing, so callers branch on r.error/r.status without try/catch
         * and .body is always a string (no "not a function" on a missing body). */
        JS_SetPropertyStr(ctx, result, "error",
            JS_NewString(ctx, r.error ? r.error : "http_request failed"));
        JS_SetPropertyStr(ctx, result, "body", JS_NewString(ctx, ""));
        js_http_result_free(&r);
        return result;
    }

    if (r.body && markdownify) {
        /* markdownify=true converts HTML to markdown for readability — it is
         * NOT a security boundary; untrusted-content wrapping happens at
         * query time via the entry's network_hosts tag. */
        char *md = html_to_markdown(r.body, r.body_len);
        if (md) {
            JS_SetPropertyStr(ctx, result, "body", JS_NewString(ctx, md));
            free(md);
        } else {
            JS_SetPropertyStr(ctx, result, "body", JS_NewStringLen(ctx, r.body, r.body_len));
        }
    } else if (r.body) {
        JS_SetPropertyStr(ctx, result, "body", JS_NewStringLen(ctx, r.body, r.body_len));
    } else {
        JS_SetPropertyStr(ctx, result, "body", JS_NewString(ctx, ""));
    }

    js_http_result_free(&r);
    return result;
}

/* ── Dual-mode return: tuple or callback ───────────────────────── */
/* If cb is a function, invoke cb(err, result) Node-style and return undefined.
 * Otherwise return [result, errno] tuple (Go-style). */

static JSValue fs_return(JSContext *ctx, JSValueConst cb, JSValue result, int err) {
    if (JS_IsFunction(ctx, cb)) {
        JSValue args[2];
        args[0] = err ? JS_NewString(ctx, strerror(err)) : JS_NULL;
        args[1] = err ? JS_NULL : result;
        JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, args[0]);
        if (err) JS_FreeValue(ctx, result);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx);
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, ret);
        return JS_UNDEFINED;
    }
    /* Direct mode: return value on success, throw on error */
    if (err) {
        JS_FreeValue(ctx, result);
        return JS_ThrowTypeError(ctx, "fs error: %s", strerror(err));
    }
    return result;
}

/* Detect callback: last arg if it's a function */
static JSValueConst fs_get_cb(JSContext *ctx, int argc, JSValueConst *argv, int cb_pos) {
    if (argc > cb_pos && JS_IsFunction(ctx, argv[cb_pos]))
        return argv[cb_pos];
    return JS_UNDEFINED;
}

/* ── fs.readFile(path[, cb]) ────────────────────────────────────── */

static JSValue js_fs_readFile(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readFile: path required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.readFile: path must be a string");
    JSValueConst cb = fs_get_cb(ctx, argc, argv, 1);

    FILE *f = fopen(path, "rb");
    if (!f) { int e = errno; JS_FreeCString(ctx, path); return fs_return(ctx, cb, JS_NULL, e); }
    JS_FreeCString(ctx, path);

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return fs_return(ctx, cb, JS_NULL, EIO); }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) { fclose(f); return fs_return(ctx, cb, JS_NULL, ENOMEM); }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    JSValue content = JS_NewStringLen(ctx, buf, nread);
    free(buf);
    return fs_return(ctx, cb, content, 0);
}

/* ── fs.writeFile(path, content[, cb]) ──────────────────────────── */

static JSValue js_fs_writeFile(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "fs.writeFile: path and content required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.writeFile: path must be a string");
    JSValueConst cb = fs_get_cb(ctx, argc, argv, 2);

    size_t clen = 0;
    const char *content = JS_ToCStringLen(ctx, &clen, argv[1]);
    if (!content) { JS_FreeCString(ctx, path); return JS_ThrowTypeError(ctx, "fs.writeFile: content must be a string"); }

    FILE *f = fopen(path, "wb");
    if (!f) { int e = errno; JS_FreeCString(ctx, path); JS_FreeCString(ctx, content); return fs_return(ctx, cb, JS_NULL, e); }
    JS_FreeCString(ctx, path);
    size_t written = fwrite(content, 1, clen, f);
    int err = ferror(f) ? errno : 0;
    fclose(f);
    JS_FreeCString(ctx, content);

    return fs_return(ctx, cb, JS_NewInt32(ctx, (int32_t)written), err);
}

/* ── fs.readdir(path[, cb]) ─────────────────────────────────────── */

static const char *dtype_str(unsigned char d_type) {
    switch (d_type) {
    case DT_DIR: return "dir";
    case DT_LNK: return "link";
    case DT_REG: return "file";
    default:     return "file";
    }
}

static JSValue js_fs_readDir(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readdir: path required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.readdir: path must be a string");
    JSValueConst cb = fs_get_cb(ctx, argc, argv, 1);

    DIR *d = opendir(path);
    if (!d) { int e = errno; JS_FreeCString(ctx, path); return fs_return(ctx, cb, JS_NULL, e); }
    JS_FreeCString(ctx, path);

    JSValue entries = JS_NewArray(ctx);
    struct dirent *ent;
    uint32_t idx = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, ent->d_name));
        JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, dtype_str(ent->d_type)));
        JS_SetPropertyUint32(ctx, entries, idx++, obj);
    }
    closedir(d);

    return fs_return(ctx, cb, entries, 0);
}

/* ── fs.stat / fs.lstat shared implementation ──────────────────── */

static JSValue js_fs_stat_impl(JSContext *ctx, int argc, JSValueConst *argv,
                               int use_lstat) {
    const char *fn = use_lstat ? "fs.lstat" : "fs.stat";
    if (argc < 1) return JS_ThrowTypeError(ctx, "%s: path required", fn);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "%s: path must be a string", fn);
    JSValueConst cb = fs_get_cb(ctx, argc, argv, 1);

    struct stat st;
    int rc = use_lstat ? lstat(path, &st) : stat(path, &st);
    if (rc != 0) { int e = errno; JS_FreeCString(ctx, path); return fs_return(ctx, cb, JS_NULL, e); }
    JS_FreeCString(ctx, path);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewFloat64(ctx, (double)st.st_size));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, (int32_t)st.st_mode));
    JS_SetPropertyStr(ctx, obj, "mtime", JS_NewFloat64(ctx, (double)st.st_mtime));
    JS_SetPropertyStr(ctx, obj, "isDir", JS_NewBool(ctx, S_ISDIR(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "isLink", JS_NewBool(ctx, S_ISLNK(st.st_mode)));

    return fs_return(ctx, cb, obj, 0);
}

static JSValue js_fs_stat(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    return js_fs_stat_impl(ctx, argc, argv, 0);
}

static JSValue js_fs_lstat(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    return js_fs_stat_impl(ctx, argc, argv, 1);
}

/* ── fs.cwd() ──────────────────────────────────────────────────── */

static JSValue js_fs_cwd(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return fs_return(ctx, JS_UNDEFINED, JS_NULL, errno);
    return fs_return(ctx, JS_UNDEFINED, JS_NewString(ctx, buf), 0);
}

/* ── console.log / print ───────────────────────────────────────── */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    /* Append to __console_buf global (prelude creates it) */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue buf_arr = JS_GetPropertyStr(ctx, global, "__console_buf");

    /* Build the line: stringify each arg, join with space */
    JSValue parts = JS_NewArray(ctx);
    for (int i = 0; i < argc; i++) {
        const char *s;
        if (JS_IsObject(argv[i])) {
            JSValue json = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException(json)) {
                JSValue ex = JS_GetException(ctx);
                JS_FreeValue(ctx, ex);
                s = JS_ToCString(ctx, argv[i]);
            } else {
                s = JS_ToCString(ctx, json);
                JS_FreeValue(ctx, json);
            }
        } else {
            s = JS_ToCString(ctx, argv[i]);
        }
        if (s) {
            JS_SetPropertyUint32(ctx, parts, (uint32_t)i, JS_NewString(ctx, s));
            JS_FreeCString(ctx, s);
        }
    }
    /* Join parts with space via JS */
    JSValue join_fn = JS_GetPropertyStr(ctx, parts, "join");
    JSValue space = JS_NewString(ctx, " ");
    JSValue line = JS_Call(ctx, join_fn, parts, 1, &space);
    JS_FreeValue(ctx, space);
    JS_FreeValue(ctx, join_fn);
    JS_FreeValue(ctx, parts);

    /* Push to __console_buf */
    JSValue push_fn = JS_GetPropertyStr(ctx, buf_arr, "push");
    JS_Call(ctx, push_fn, buf_arr, 1, &line);
    JS_FreeValue(ctx, push_fn);
    JS_FreeValue(ctx, line);
    JS_FreeValue(ctx, buf_arr);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

/* ── getConfig(key) — the owning extension's settings ───────────── */

/* Twin of channel.getConfig (qjs_host_channel.c): string on hit, null on miss.
 * The channel runner can query the DB; this context cannot, so the object was
 * resolved parent-side and arrives as JSON. It closes over func_data[0] rather
 * than living on a global so extension code can't rewrite its own config. */
static JSValue js_tool_get_config(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv,
                                  int magic, JSValue *func_data) {
    (void)this_val; (void)magic;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getConfig(key)");
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_NULL;
    JSValue v = JS_GetPropertyStr(ctx, func_data[0], key);
    JS_FreeCString(ctx, key);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_NULL;
    }
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return JS_NULL; }
    return v;
}

/* ── Registration entry point ──────────────────────────────────── */

void qjs_register_eval_host_functions(JSContext *ctx, const char *config_json) {
    JSValue global = JS_GetGlobalObject(ctx);

    /* getConfig — always bound (an empty object for js_eval and for extensions
     * with no config) so handlers never trip over a ReferenceError. */
    JSValue cfg = (config_json && config_json[0])
        ? JS_ParseJSON(ctx, config_json, strlen(config_json), "<config>")
        : JS_NewObject(ctx);
    if (JS_IsException(cfg) || !JS_IsObject(cfg)) {
        JS_FreeValue(ctx, cfg);
        /* Drop the pending parse error — it must not surface as the handler's
         * own exception on the next eval. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        cfg = JS_NewObject(ctx);
    }
    JS_SetPropertyStr(ctx, global, "getConfig",
        JS_NewCFunctionData(ctx, js_tool_get_config, 1, 0, 1, &cfg));
    JS_FreeValue(ctx, cfg);   /* JS_NewCFunctionData dups its data */

    /* http_request */
    JS_SetPropertyStr(ctx, global, "http_request",
        JS_NewCFunction(ctx, js_http_request, "http_request", 2));

    /* fs object — both Node callback style and Sync style point to same impl */
    JSValue fs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs, "readFile", JS_NewCFunction(ctx, js_fs_readFile, "readFile", 2));
    JS_SetPropertyStr(ctx, fs, "readFileSync", JS_NewCFunction(ctx, js_fs_readFile, "readFileSync", 1));
    JS_SetPropertyStr(ctx, fs, "writeFile", JS_NewCFunction(ctx, js_fs_writeFile, "writeFile", 3));
    JS_SetPropertyStr(ctx, fs, "writeFileSync", JS_NewCFunction(ctx, js_fs_writeFile, "writeFileSync", 2));
    JS_SetPropertyStr(ctx, fs, "readdir", JS_NewCFunction(ctx, js_fs_readDir, "readdir", 2));
    JS_SetPropertyStr(ctx, fs, "readdirSync", JS_NewCFunction(ctx, js_fs_readDir, "readdirSync", 1));
    JS_SetPropertyStr(ctx, fs, "stat", JS_NewCFunction(ctx, js_fs_stat, "stat", 2));
    JS_SetPropertyStr(ctx, fs, "statSync", JS_NewCFunction(ctx, js_fs_stat, "statSync", 1));
    JS_SetPropertyStr(ctx, fs, "lstat", JS_NewCFunction(ctx, js_fs_lstat, "lstat", 2));
    JS_SetPropertyStr(ctx, fs, "lstatSync", JS_NewCFunction(ctx, js_fs_lstat, "lstatSync", 1));
    JS_SetPropertyStr(ctx, fs, "cwd", JS_NewCFunction(ctx, js_fs_cwd, "cwd", 0));
    JS_SetPropertyStr(ctx, global, "fs", fs);

    /* console.log + print */
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 0));
    JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, js_console_log, "warn", 0));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_log, "error", 0));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_console_log, "print", 0));

    JS_FreeValue(ctx, global);
    qjs_register_xml(ctx);
}

/* ── In-process eval driver (qjs_eval_run) ──────────────────────────────── */

#define QJS_EVAL_HEAP_MB_DEFAULT 8
#define QJS_EVAL_MAX_INSTRUCTIONS 10000000
#define QJS_EVAL_MAX_FILE        (1024 * 1024)

/* Heap cap for one eval, in MB. The broker child has no DB, so the parent
 * resolves config `js_heap_mb` and hands it over in the env (proc.c). Bounds
 * are re-applied here because this side is what actually allocates. */
static int qjs_eval_heap_mb(void) {
    const char *env = getenv("CCLAW_JS_HEAP_MB");
    int mb = env && env[0] ? atoi(env) : QJS_EVAL_HEAP_MB_DEFAULT;
    if (mb < 1) mb = QJS_EVAL_HEAP_MB_DEFAULT;
    if (mb > 512) mb = 512;
    return mb;
}

static const char *QJS_EVAL_PRELUDE =
    "var __console_buf = [];\n"
    "var console = {\n"
    "  log: function() {\n"
    "    var parts = [];\n"
    "    for (var i = 0; i < arguments.length; i++) {\n"
    "      var v = arguments[i];\n"
    "      parts.push(typeof v === 'object' ? JSON.stringify(v) : '' + v);\n"
    "    }\n"
    "    __console_buf.push(parts.join(' '));\n"
    "  }\n"
    "};\n"
    "console.warn = console.log;\n"
    "console.error = console.log;\n"
    "var require = function() {\n"
    "  throw new TypeError('require() not available — there are no modules. Use globals: fs.readdir(path), fs.readFile(path), fs.writeFile(path, data), fs.stat(path), fs.cwd(), http_request(url), XML.parse(str).');\n"
    "};\n"
    "var process = {};\n"
    "Object.defineProperty(process, 'env', {get: function() { throw new TypeError('process.env not available.'); }});\n"
    "Object.defineProperty(process, 'cwd', {get: function() { throw new TypeError('process.cwd not available. Use fs.cwd().'); }});\n"
    "Object.defineProperty(process, 'argv', {get: function() { throw new TypeError('process.argv not available.'); }});\n"
    "Object.defineProperty(process, 'exit', {get: function() { throw new TypeError('process.exit not available.'); }});\n"
    "Object.defineProperty(process, 'platform', {get: function() { throw new TypeError('process.platform not available.'); }});\n"
    "var module = {};\n"
    "Object.defineProperty(module, 'exports', {\n"
    "  get: function() { throw new TypeError('module.exports not available.'); },\n"
    "  set: function() { throw new TypeError('module.exports not available. Return your value as the last expression.'); }\n"
    "});\n"
    "var print = console.log;\n";

/* The engine is modern QuickJS; the surviving gaps are environmental —
 * no module loader, no event loop, no top-level await in global eval. */
static const char *qjs_syntax_hint(const char *code) {
    if (!code) return "";
    if (strstr(code, "require(") || strstr(code, "import "))
        return " — hint: no modules; 'fs' and 'http_request' are globals (e.g. fs.readdir('.'))";
    if (strstr(code, "await "))
        return " — hint: no top-level await; http_request is synchronous — var r = http_request(url); then r.body / r.json()";
    return "";
}

char *qjs_eval_run(const char *code, const char *filename, const char *args_json,
                   const char *config_json) {
    if ((!code || !code[0]) && (!filename || !filename[0]))
        return strdup("error: must provide 'code' or 'filename'");

    int heap_mb = qjs_eval_heap_mb();
    QjsRuntime *qrt = qjs_runtime_create((size_t)heap_mb * 1024 * 1024);
    if (!qrt) return strdup("error: out of memory");
    qjs_set_interrupt_limit(qrt, QJS_EVAL_MAX_INSTRUCTIONS);

    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_EVAL);
    if (!ctx) { qjs_runtime_destroy(qrt); return strdup("error: JS context creation failed"); }

    JsHostCtx hctx = { .instruction_count = 0,
                       .instruction_limit = QJS_EVAL_MAX_INSTRUCTIONS,
                       .allowed_hosts = NULL, .allowed_hosts_count = 0 };
    JS_SetContextOpaque(ctx, &hctx);
    qjs_register_eval_host_functions(ctx, config_json);

    JSValue pv = JS_Eval(ctx, QJS_EVAL_PRELUDE, strlen(QJS_EVAL_PRELUDE),
                         "<prelude>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(pv)) {
        char *msg = qjs_get_exception_string(ctx);
        size_t len = (msg ? strlen(msg) : 8) + 32;
        char *r = malloc(len);
        if (r) snprintf(r, len, "error: prelude failed: %s", msg ? msg : "unknown");
        free(msg);
        JS_FreeContext(ctx); qjs_runtime_destroy(qrt);
        return r ? r : strdup("error: prelude failed");
    }
    JS_FreeValue(ctx, pv);

    /* Build code to eval */
    char *eval_code = NULL;
    int free_eval = 0;
    if (code && code[0]) {
        eval_code = (char *)code;  /* borrowed */
    } else {
        FILE *f = fopen(filename, "r");
        if (!f) {
            size_t len = strlen(filename) + 32;
            char *r = malloc(len);
            if (r) snprintf(r, len, "error: cannot open %s", filename);
            JS_FreeContext(ctx); qjs_runtime_destroy(qrt);
            return r ? r : strdup("error: cannot open file");
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz < 0 || sz > QJS_EVAL_MAX_FILE) {
            fclose(f); JS_FreeContext(ctx); qjs_runtime_destroy(qrt);
            return strdup("error: file too large or unreadable");
        }
        fseek(f, 0, SEEK_SET);
        char *fbuf = malloc((size_t)sz + 1);
        if (!fbuf) { fclose(f); JS_FreeContext(ctx); qjs_runtime_destroy(qrt); return strdup("error: out of memory"); }
        size_t rd = fread(fbuf, 1, (size_t)sz, f);
        fclose(f);
        fbuf[rd] = '\0';
        if (args_json && args_json[0]) {
            size_t wlen = 20 + rd + 4 + strlen(args_json) + 2;
            eval_code = malloc(wlen);
            if (!eval_code) { free(fbuf); JS_FreeContext(ctx); qjs_runtime_destroy(qrt); return strdup("error: out of memory"); }
            snprintf(eval_code, wlen, "(function(args){\n%s\n})(%s)", fbuf, args_json);
            free(fbuf);
        } else {
            eval_code = fbuf;
        }
        free_eval = 1;
    }

    JSValue val = JS_Eval(ctx, eval_code, strlen(eval_code), "<qjs_eval>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(val))
        val = qjs_resolve(ctx, val);

    char *result = NULL;
    int result_is_undefined = 0;
    if (JS_IsException(val)) {
        char *msg = qjs_get_exception_string(ctx);
        if (msg) {
            /* The OOM hint quotes the live cap — a hardcoded number goes
             * stale the moment js_heap_mb is retuned, and "heap limit is 4MB"
             * on an 8MB heap sends the agent chasing the wrong fix. */
            char oom_hint[192];
            snprintf(oom_hint, sizeof oom_hint,
                     " — heap limit is %dMB; the parsed value costs several times"
                     " the source (JSON ~10x, XML ~3x), so fetch less or extract"
                     " fields instead of parsing the whole document", heap_mb);
            const char *hint = strstr(msg, "SyntaxError") ? qjs_syntax_hint(eval_code)
                             : strstr(msg, "out of memory") ? oom_hint
                             : "";
            size_t len = strlen(msg) + strlen(hint) + 16;
            result = malloc(len);
            if (result) snprintf(result, len, "error: %s%s", msg, hint);
            free(msg);
        }
        if (!result) result = strdup("error: exception (no message)");
    } else if (JS_IsUndefined(val)) {
        JS_FreeValue(ctx, val);
        result_is_undefined = 1;
        result = strdup("undefined");
    } else if (JS_IsNull(val)) {
        JS_FreeValue(ctx, val);
        result = strdup("null");
    } else {
        const char *str = JS_ToCString(ctx, val);
        result = str ? strdup(str) : strdup("error: cannot convert result to string");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, val);
    }

    /* Console output (print/console.log): if the eval returned undefined, the
     * console buffer IS the result (replaces "undefined"). If a real value was
     * returned, prepend console output so script-side warnings are never
     * silently swallowed (e.g. a script that catches HTTP errors and returns
     * "Done!" while logging "network failed"). */
    {
        const char *check = "__console_buf.length > 0 ? __console_buf.join('\\n') : undefined";
        JSValue buf_val = JS_Eval(ctx, check, strlen(check), "<console>", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsUndefined(buf_val) && !JS_IsException(buf_val)) {
            const char *cstr = JS_ToCString(ctx, buf_val);
            if (cstr && cstr[0]) {
                if (result_is_undefined) {
                    /* Console output replaces "undefined" */
                    free(result);
                    result = strdup(cstr);
                } else if (result) {
                    /* Prepend console output to the actual result */
                    size_t clen = strlen(cstr);
                    size_t rlen = strlen(result);
                    char *combined = malloc(clen + 1 + rlen + 1);
                    if (combined) {
                        memcpy(combined, cstr, clen);
                        combined[clen] = '\n';
                        memcpy(combined + clen + 1, result, rlen + 1);
                        free(result);
                        result = combined;
                    }
                }
            }
            if (cstr) JS_FreeCString(ctx, cstr);
        }
        JS_FreeValue(ctx, buf_val);
    }

    if (free_eval) free(eval_code);
    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
    return result ? result : strdup("error: OOM");
}
