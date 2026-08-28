#define _POSIX_C_SOURCE 200809L
#include "qjs_llm.h"
#include "js_http_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Borrowed from the run-tool request (q->llm_json) for the eval's lifetime.
 * Parsed fresh per llm() call; the parsed value — auth headers included —
 * stays in locals below and is never attached to a global, so handler code
 * cannot enumerate its way to a key. */
static const char *g_llm_json;

void qjs_host_set_llm(const char *llm_json) {
    g_llm_json = (llm_json && llm_json[0]) ? llm_json : NULL;
}

/* Copy src's own enumerable string-keyed props onto dst (shallow). The
 * provider escape hatch: candidate `extra` first, then caller `extra`, so a
 * caller can override what the provider config injects. */
static void merge_object(JSContext *ctx, JSValue dst, JSValueConst src) {
    if (!JS_IsObject(src)) return;
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, src,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
        return;
    for (uint32_t i = 0; i < n; i++) {
        JSValue v = JS_GetProperty(ctx, src, tab[i].atom);
        JS_SetProperty(ctx, dst, tab[i].atom, v);  /* consumes v */
    }
    JS_FreePropertyEnum(ctx, tab, n);
}

/* Own string prop as a fresh C string, NULL if absent/not-a-string-able.
 * Caller JS_FreeCString()s. */
static const char *get_str(JSContext *ctx, JSValueConst obj, const char *k) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return NULL; }
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    return s;
}

/* messages, normalized to the OpenAI shape: string prompt → one user
 * message; {messages:[...]} passes through; opts.system prepends. Returns a
 * JS array (caller frees) or JS_EXCEPTION. */
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
        "llm: pass a prompt string or {messages:[{role,content},...]}");
}

/* Request body for one candidate. `format` is "gemini" or "openai" (the
 * parent's wire vocabulary — anything unknown is treated as openai, the
 * house default). Returns malloc'd JSON string or NULL with a JS exception
 * pending. */
static char *build_body(JSContext *ctx, const char *format, const char *model,
                        JSValueConst msgs, JSValueConst cand_extra,
                        JSValueConst opts) {
    JSValue body = JS_NewObject(ctx);
    int gemini = (format && strcmp(format, "gemini") == 0);

    JSValue max_tokens = JS_UNDEFINED, temperature = JS_UNDEFINED;
    if (JS_IsObject(opts)) {
        max_tokens = JS_GetPropertyStr(ctx, opts, "max_tokens");
        temperature = JS_GetPropertyStr(ctx, opts, "temperature");
    }

    if (gemini) {
        /* {contents:[{role,parts:[{text}]}], systemInstruction?,
         *  generationConfig?} — system messages fold into systemInstruction,
         * assistant becomes 'model'. */
        JSValue contents = JS_NewArray(ctx);
        uint32_t ci = 0;
        JSValue sys_parts = JS_UNDEFINED;
        JSValue len_v = JS_GetPropertyStr(ctx, msgs, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, len_v);
        JS_FreeValue(ctx, len_v);
        for (uint32_t i = 0; i < len; i++) {
            JSValue m = JS_GetPropertyUint32(ctx, msgs, i);
            const char *role = get_str(ctx, m, "role");
            JSValue content = JS_GetPropertyStr(ctx, m, "content");
            JSValue part = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, part, "text", content);  /* consumes */
            if (role && strcmp(role, "system") == 0) {
                if (JS_IsUndefined(sys_parts)) sys_parts = JS_NewArray(ctx);
                JSValue sl = JS_GetPropertyStr(ctx, sys_parts, "length");
                uint32_t sn = 0; JS_ToUint32(ctx, &sn, sl); JS_FreeValue(ctx, sl);
                JS_SetPropertyUint32(ctx, sys_parts, sn, part);
            } else {
                JSValue entry = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, entry, "role",
                    JS_NewString(ctx, role && strcmp(role, "assistant") == 0
                                          ? "model" : "user"));
                JSValue parts = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, parts, 0, part);
                JS_SetPropertyStr(ctx, entry, "parts", parts);
                JS_SetPropertyUint32(ctx, contents, ci++, entry);
            }
            if (role) JS_FreeCString(ctx, role);
            JS_FreeValue(ctx, m);
        }
        JS_SetPropertyStr(ctx, body, "contents", contents);
        if (!JS_IsUndefined(sys_parts)) {
            JSValue si = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, si, "parts", sys_parts);
            JS_SetPropertyStr(ctx, body, "systemInstruction", si);
        }
        if (!JS_IsUndefined(max_tokens) || !JS_IsUndefined(temperature)) {
            JSValue gc = JS_NewObject(ctx);
            if (!JS_IsUndefined(max_tokens))
                JS_SetPropertyStr(ctx, gc, "maxOutputTokens",
                                  JS_DupValue(ctx, max_tokens));
            if (!JS_IsUndefined(temperature))
                JS_SetPropertyStr(ctx, gc, "temperature",
                                  JS_DupValue(ctx, temperature));
            JS_SetPropertyStr(ctx, body, "generationConfig", gc);
        }
    } else {
        JS_SetPropertyStr(ctx, body, "model", JS_NewString(ctx, model));
        JS_SetPropertyStr(ctx, body, "messages", JS_DupValue(ctx, msgs));
        if (!JS_IsUndefined(max_tokens))
            JS_SetPropertyStr(ctx, body, "max_tokens",
                              JS_DupValue(ctx, max_tokens));
        if (!JS_IsUndefined(temperature))
            JS_SetPropertyStr(ctx, body, "temperature",
                              JS_DupValue(ctx, temperature));
    }
    JS_FreeValue(ctx, max_tokens);
    JS_FreeValue(ctx, temperature);

    merge_object(ctx, body, cand_extra);
    if (JS_IsObject(opts)) {
        JSValue oe = JS_GetPropertyStr(ctx, opts, "extra");
        merge_object(ctx, body, oe);
        JS_FreeValue(ctx, oe);
    }

    JSValue js = JS_JSONStringify(ctx, body, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, body);
    if (JS_IsException(js)) return NULL;
    const char *s = JS_ToCString(ctx, js);
    char *out = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, js);
    return out;
}

/* Completion text out of a 2xx response body, per wire format. Returns a JS
 * string, or JS_NULL when the body has no text where text should be (the
 * caller then treats the candidate as failed rather than returning ""). */
static JSValue extract_text(JSContext *ctx, const char *format,
                            JSValueConst resp) {
    if (format && strcmp(format, "gemini") == 0) {
        JSValue cands = JS_GetPropertyStr(ctx, resp, "candidates");
        JSValue c0 = JS_GetPropertyUint32(ctx, cands, 0);
        JS_FreeValue(ctx, cands);
        JSValue content = JS_GetPropertyStr(ctx, c0, "content");
        JS_FreeValue(ctx, c0);
        JSValue parts = JS_GetPropertyStr(ctx, content, "parts");
        JS_FreeValue(ctx, content);
        JSValue len_v = JS_GetPropertyStr(ctx, parts, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, len_v);
        JS_FreeValue(ctx, len_v);
        /* Multiple text parts arrive on grounded responses; join with a
         * newline so the seams stay visible to the caller. */
        JSValue acc = JS_UNDEFINED;
        for (uint32_t i = 0; i < len; i++) {
            JSValue p = JS_GetPropertyUint32(ctx, parts, i);
            JSValue t = JS_GetPropertyStr(ctx, p, "text");
            JS_FreeValue(ctx, p);
            if (JS_IsString(t)) {
                if (JS_IsUndefined(acc)) {
                    acc = t;
                } else {
                    const char *a = JS_ToCString(ctx, acc);
                    const char *b = JS_ToCString(ctx, t);
                    if (a && b) {
                        size_t need = strlen(a) + 1 + strlen(b) + 1;
                        char *buf = malloc(need);
                        if (buf) {
                            snprintf(buf, need, "%s\n%s", a, b);
                            JS_FreeValue(ctx, acc);
                            acc = JS_NewString(ctx, buf);
                            free(buf);
                        }
                    }
                    if (a) JS_FreeCString(ctx, a);
                    if (b) JS_FreeCString(ctx, b);
                    JS_FreeValue(ctx, t);
                }
            } else {
                JS_FreeValue(ctx, t);
            }
        }
        JS_FreeValue(ctx, parts);
        return JS_IsUndefined(acc) ? JS_NULL : acc;
    }

    JSValue choices = JS_GetPropertyStr(ctx, resp, "choices");
    JSValue c0 = JS_GetPropertyUint32(ctx, choices, 0);
    JS_FreeValue(ctx, choices);
    JSValue msg = JS_GetPropertyStr(ctx, c0, "message");
    JS_FreeValue(ctx, c0);
    JSValue text = JS_GetPropertyStr(ctx, msg, "content");
    JS_FreeValue(ctx, msg);
    if (JS_IsString(text)) return text;
    JS_FreeValue(ctx, text);
    return JS_NULL;
}

static JSValue js_llm(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
    (void)this_val;
    if (!g_llm_json)
        return JS_ThrowTypeError(ctx,
            "llm: this agent has no routable model (agent_models is empty "
            "or every provider key is missing)");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "llm: prompt required");

    JSValue cands = JS_ParseJSON(ctx, g_llm_json, strlen(g_llm_json), "<llm>");
    if (JS_IsException(cands)) return cands;

    JSValueConst opts = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    JSValue msgs = build_messages(ctx, argv[0], opts);
    if (JS_IsException(msgs)) { JS_FreeValue(ctx, cands); return msgs; }

    const char *want = JS_IsObject(opts) ? get_str(ctx, opts, "model") : NULL;
    int full = 0;
    int timeout = QJS_LLM_TIMEOUT_DEFAULT;
    if (JS_IsObject(opts)) {
        JSValue f = JS_GetPropertyStr(ctx, opts, "full");
        full = JS_ToBool(ctx, f);
        JS_FreeValue(ctx, f);
        JSValue t = JS_GetPropertyStr(ctx, opts, "timeout");
        if (!JS_IsUndefined(t) && !JS_IsNull(t)) {
            int32_t secs = 0;
            if (JS_ToInt32(ctx, &secs, t) == 0 && secs > 0)
                timeout = secs > JS_HTTP_TIMEOUT_MAX ? JS_HTTP_TIMEOUT_MAX : secs;
        }
        JS_FreeValue(ctx, t);
    }

    JSValue len_v = JS_GetPropertyStr(ctx, cands, "length");
    uint32_t ncand = 0;
    JS_ToUint32(ctx, &ncand, len_v);
    JS_FreeValue(ctx, len_v);

    /* Failure trail across the walk, for the final throw. */
    char fails[512] = "";
    size_t fpos = 0;
    int matched = 0;
    JSValue result = JS_UNDEFINED;
    int done = 0;

    for (uint32_t i = 0; i < ncand && !done; i++) {
        JSValue cand = JS_GetPropertyUint32(ctx, cands, i);
        const char *id = get_str(ctx, cand, "id");
        const char *model = get_str(ctx, cand, "model");
        const char *url = get_str(ctx, cand, "url");
        const char *auth = get_str(ctx, cand, "auth");
        const char *format = get_str(ctx, cand, "format");

        /* opts.model narrows the walk: substring match on the routing id or
         * the provider model name, same leniency as `cclaw models` search. */
        if (want && want[0] &&
            !(id && strstr(id, want)) && !(model && strstr(model, want)))
            goto next;
        matched++;

        if (!url || !auth || !model) goto next;

        JSValue cand_extra = JS_GetPropertyStr(ctx, cand, "extra");
        char *body = build_body(ctx, format, model, msgs, cand_extra, opts);
        JS_FreeValue(ctx, cand_extra);
        if (!body) { done = 1; result = JS_EXCEPTION; goto next; }

        const char *headers[] = { "Content-Type: application/json", auth, NULL };
        JsHttpResult r = js_http_fetch_exec(url, "POST", body, headers, timeout);
        free(body);

        if (r.status >= 200 && r.status < 300 && r.body) {
            JSValue resp = JS_ParseJSON(ctx, r.body, r.body_len, "<llm-resp>");
            if (!JS_IsException(resp)) {
                JSValue text = extract_text(ctx, format, resp);
                if (JS_IsString(text)) {
                    if (full) {
                        JSValue o = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, o, "text", text);
                        JS_SetPropertyStr(ctx, o, "model",
                            JS_NewString(ctx, model));
                        JS_SetPropertyStr(ctx, o, "id",
                            JS_NewString(ctx, id ? id : ""));
                        JS_SetPropertyStr(ctx, o, "status",
                            JS_NewInt32(ctx, r.status));
                        JS_SetPropertyStr(ctx, o, "body", resp);  /* consumed */
                        result = o;
                    } else {
                        JS_FreeValue(ctx, resp);
                        result = text;
                    }
                    done = 1;
                    js_http_result_free(&r);
                    goto next;
                }
                JS_FreeValue(ctx, text);
                JS_FreeValue(ctx, resp);
                if (fpos < sizeof(fails))
                    fpos += (size_t)snprintf(fails + fpos, sizeof(fails) - fpos,
                        "%s%s: empty completion", fpos ? "; " : "",
                        id ? id : model);
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
                if (fpos < sizeof(fails))
                    fpos += (size_t)snprintf(fails + fpos, sizeof(fails) - fpos,
                        "%s%s: unparseable response", fpos ? "; " : "",
                        id ? id : model);
            }
        } else if (fpos < sizeof(fails)) {
            if (r.status < 0)
                fpos += (size_t)snprintf(fails + fpos, sizeof(fails) - fpos,
                    "%s%s: %s", fpos ? "; " : "", id ? id : (model ? model : "?"),
                    r.error ? r.error : "transport error");
            else
                fpos += (size_t)snprintf(fails + fpos, sizeof(fails) - fpos,
                    "%s%s: http %d %.120s", fpos ? "; " : "",
                    id ? id : (model ? model : "?"), r.status,
                    r.body ? r.body : "");
        }
        js_http_result_free(&r);

next:
        if (id) JS_FreeCString(ctx, id);
        if (model) JS_FreeCString(ctx, model);
        if (url) JS_FreeCString(ctx, url);
        if (auth) JS_FreeCString(ctx, auth);
        if (format) JS_FreeCString(ctx, format);
        JS_FreeValue(ctx, cand);
    }

    if (want) JS_FreeCString(ctx, want);
    JS_FreeValue(ctx, msgs);
    JS_FreeValue(ctx, cands);

    if (done) return result;
    if (!matched)
        return JS_ThrowTypeError(ctx,
            "llm: no routed model matches opts.model — llm() can only use "
            "models already on this agent's routing list");
    return JS_ThrowTypeError(ctx, "llm: every candidate failed — %s",
                             fails[0] ? fails : "no usable candidate");
}

void qjs_register_llm(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "llm",
                      JS_NewCFunction(ctx, js_llm, "llm", 2));
    JS_FreeValue(ctx, global);
}
