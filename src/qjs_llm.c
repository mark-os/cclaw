#define _POSIX_C_SOURCE 200809L
#include "qjs_llm.h"
#include "llm_bridge.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Bridge responses carry the provider's parsed body; bound what we are
 * willing to hand the (heap-limited) engine. */
#define LLM_CLIENT_RESP_MAX (8u * 1024 * 1024)

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, (const char *)buf + put, n - put);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        put += (size_t)r;
    }
    return 0;
}

/* One bridge round-trip: 4-byte LE length + JSON both ways. Returns malloc'd
 * response JSON, or NULL with *err set to a static message. No read timeout:
 * the parent enforces the per-attempt HTTP timeout, and the tool's own
 * timeout is the outer bound on this whole child. */
static char *bridge_call(const char *sock_path, const char *req_json,
                         const char **err) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t plen = strlen(sock_path);
    if (plen >= sizeof(addr.sun_path)) { *err = "bridge socket path too long"; return NULL; }
    memcpy(addr.sun_path, sock_path, plen + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *err = "socket() failed"; return NULL; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        *err = "cannot reach the LLM bridge";
        return NULL;
    }
    uint32_t len = (uint32_t)strlen(req_json);
    unsigned char hdr[4] = { (unsigned char)len, (unsigned char)(len >> 8),
                             (unsigned char)(len >> 16), (unsigned char)(len >> 24) };
    if (write_full(fd, hdr, 4) != 0 || write_full(fd, req_json, len) != 0) {
        close(fd);
        *err = "bridge write failed";
        return NULL;
    }
    if (read_full(fd, hdr, 4) != 0) {
        close(fd);
        *err = "bridge closed without answering";
        return NULL;
    }
    uint32_t rlen = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                    ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (rlen == 0 || rlen > LLM_CLIENT_RESP_MAX) {
        close(fd);
        *err = "bridge response oversized";
        return NULL;
    }
    char *resp = malloc((size_t)rlen + 1);
    if (!resp || read_full(fd, resp, rlen) != 0) {
        free(resp);
        close(fd);
        *err = "bridge read failed";
        return NULL;
    }
    resp[rlen] = '\0';
    close(fd);
    return resp;
}

/* Own string prop as a fresh C string, NULL if absent. Caller frees via
 * JS_FreeCString. */
static const char *get_str(JSContext *ctx, JSValueConst obj, const char *k) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return NULL; }
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    return s;
}

/* messages, normalized to the OpenAI shape the bridge expects: string prompt
 * → one user message; {messages:[...]} passes through; opts.system prepends.
 * Returns a JS array (caller frees) or JS_EXCEPTION. */
static JSValue build_messages(JSContext *ctx, JSValueConst input,
                              JSValueConst opts) {
    JSValue msgs = JS_NewArray(ctx);
    uint32_t idx = 0;

    if (JS_IsObject(opts)) {
        const char *sys = get_str(ctx, opts, "system");
        if (sys) {
            JSValue m = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, m, "role", JS_NewString(ctx, "system"));
            JS_SetPropertyStr(ctx, m, "content", JS_NewString(ctx, sys));
            JS_SetPropertyUint32(ctx, msgs, idx++, m);
            JS_FreeCString(ctx, sys);
        }
    }

    if (JS_IsString(input)) {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "role", JS_NewString(ctx, "user"));
        JS_SetPropertyStr(ctx, m, "content", JS_DupValue(ctx, input));
        JS_SetPropertyUint32(ctx, msgs, idx++, m);
        return msgs;
    }

    JSValue arr = JS_GetPropertyStr(ctx, input, "messages");
    if (JS_IsObject(arr)) {
        JSValue len_v = JS_GetPropertyStr(ctx, arr, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, len_v);
        JS_FreeValue(ctx, len_v);
        for (uint32_t i = 0; i < len; i++)
            JS_SetPropertyUint32(ctx, msgs, idx++,
                                 JS_GetPropertyUint32(ctx, arr, i));
        JS_FreeValue(ctx, arr);
        return msgs;
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, msgs);
    return JS_ThrowTypeError(ctx,
        "LLM: pass a prompt string or {messages:[{role,content},...]}");
}

/* Image type from the file's first bytes — the name is the caller's claim,
 * the bytes are what the provider will decode. NULL = not a supported image. */
static const char *image_mime(const unsigned char *d, size_t n) {
    const char *m = util_sniff_mime(d, n);
    return (m && strncmp(m, "image/", 6) == 0) ? m : NULL;
}

/* opts.images (file paths) → the request with ,"images":[{mime,data}] added.
 * The files are read and base64-encoded here in C and spliced into the JSON
 * text, so an image is never a JS value: the engine's heap limit does not
 * bound it, and the script cannot see or alter what is sent. Paths resolve
 * in the child's own (sandboxed) view of the filesystem.
 * Returns malloc'd JSON; NULL with errbuf set on failure; req_json's own
 * copy when there are no images. */
static char *splice_images(JSContext *ctx, JSValueConst opts,
                           const char *req_json, char *errbuf, size_t errcap) {
    uint32_t n = 0;
    JSValue arr = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "images")
                                    : JS_UNDEFINED;
    if (JS_IsString(arr)) {            /* images:'one.jpg' — the obvious slip */
        JSValue one = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, one, 0, arr);
        arr = one;
    }
    if (JS_IsArray(ctx, arr)) {
        JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
    } else if (!JS_IsUndefined(arr) && !JS_IsNull(arr)) {
        JS_FreeValue(ctx, arr);
        snprintf(errbuf, errcap, "opts.images must be an array of file paths");
        return NULL;
    }
    if (n == 0) { JS_FreeValue(ctx, arr); return strdup(req_json); }

    size_t rlen = strlen(req_json), len = rlen - 1, total = 0;   /* drop '}' */
    size_t cap = rlen + 64;
    char *out = malloc(cap);
    if (out) memcpy(out, req_json, len);
    for (uint32_t i = 0; out && i < n; i++) {
        JSValue pv = JS_GetPropertyUint32(ctx, arr, i);
        const char *path = JS_ToCString(ctx, pv);
        JS_FreeValue(ctx, pv);
        size_t flen = 0;
        char *raw = path ? util_read_file(path, &flen) : NULL;
        const char *mime = raw ? image_mime((const unsigned char *)raw, flen) : NULL;
        total += flen;
        if (!raw)
            snprintf(errbuf, errcap, "opts.images: cannot read %.200s",
                     path ? path : "(not a string)");
        else if (!mime)
            snprintf(errbuf, errcap, "opts.images: %.200s is not a JPEG, PNG, "
                     "GIF or WebP image", path);
        else if (total > LLM_IMAGES_MAX)
            snprintf(errbuf, errcap, "opts.images: %zu bytes of images is over "
                     "the %u-byte limit providers accept — downscale first",
                     total, (unsigned)LLM_IMAGES_MAX);
        char *b64 = errbuf[0] ? NULL
                  : base64_encode((const unsigned char *)raw, flen);
        free(raw);
        if (path) JS_FreeCString(ctx, path);
        if (!b64) {
            if (!errbuf[0]) snprintf(errbuf, errcap, "out of memory");
            free(out);
            out = NULL;
            break;
        }
        size_t blen = strlen(b64), need = len + blen + 96;
        char *grown = need > cap ? realloc(out, cap = need * 2) : out;
        if (!grown) { free(out); free(b64); out = NULL; break; }
        out = grown;
        len += (size_t)snprintf(out + len, cap - len,
                                "%s{\"mime\":\"%s\",\"data\":\"",
                                i == 0 ? ",\"images\":[" : ",", mime);
        memcpy(out + len, b64, blen);
        len += blen;
        memcpy(out + len, "\"}", 2);
        len += 2;
        free(b64);
    }
    JS_FreeValue(ctx, arr);
    if (!out) {
        if (!errbuf[0]) snprintf(errbuf, errcap, "out of memory");
        return NULL;
    }
    memcpy(out + len, "]}", 3);
    return out;
}

static JSValue js_llm(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
    (void)this_val;
    const char *sock = getenv("CCLAW_LLM_SOCK");
    if (!sock || !sock[0])
        return JS_ThrowTypeError(ctx, "LLM: not available in this context "
                                      "(no bridge socket)");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "LLM: prompt required");

    JSValueConst opts = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    JSValue msgs = build_messages(ctx, argv[0], opts);
    if (JS_IsException(msgs)) return msgs;

    int full = 0;
    if (JS_IsObject(opts)) {
        JSValue f = JS_GetPropertyStr(ctx, opts, "full");
        full = JS_ToBool(ctx, f);
        JS_FreeValue(ctx, f);
    }

    /* {messages, opts} → JSON. opts passes through whole; the parent reads
     * only what it knows (model/max_tokens/temperature/timeout/extra). */
    JSValue reqo = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, reqo, "messages", msgs);   /* consumed */
    JS_SetPropertyStr(ctx, reqo, "opts",
                      JS_IsObject(opts) ? JS_DupValue(ctx, opts)
                                        : JS_NewObject(ctx));
    JSValue reqs = JS_JSONStringify(ctx, reqo, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, reqo);
    if (JS_IsException(reqs)) return reqs;
    const char *req_json = JS_ToCString(ctx, reqs);
    if (!req_json) { JS_FreeValue(ctx, reqs); return JS_EXCEPTION; }

    char imgerr[320] = "";
    char *wire = splice_images(ctx, opts, req_json, imgerr, sizeof(imgerr));
    JS_FreeCString(ctx, req_json);
    JS_FreeValue(ctx, reqs);
    if (!wire)
        return JS_ThrowTypeError(ctx, "LLM: %s", imgerr);

    const char *err = "bridge error";
    char *resp = bridge_call(sock, wire, &err);
    free(wire);
    if (!resp)
        return JS_ThrowTypeError(ctx, "LLM: %s", err);

    JSValue ro = JS_ParseJSON(ctx, resp, strlen(resp), "<llm>");
    free(resp);
    if (JS_IsException(ro)) return ro;

    JSValue okv = JS_GetPropertyStr(ctx, ro, "ok");
    int ok = JS_ToBool(ctx, okv);
    JS_FreeValue(ctx, okv);
    if (!ok) {
        const char *msg = get_str(ctx, ro, "error");
        JSValue e = JS_ThrowTypeError(ctx, "LLM: %s",
                                      msg ? msg : "request failed");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, ro);
        return e;
    }
    if (full) return ro;   /* {ok:true, text, model, id, status, body} */
    JSValue text = JS_GetPropertyStr(ctx, ro, "text");
    JS_FreeValue(ctx, ro);
    return text;
}

void qjs_register_llm(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "LLM",
                      JS_NewCFunction(ctx, js_llm, "LLM", 2));
    JS_FreeValue(ctx, global);
}
