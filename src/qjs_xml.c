/* xml.parse(str) — XML → JS object tree in the fast-xml-parser shape, the
 * dominant Node convention models already know how to navigate:
 *   <rss><channel><item><title>A</title></item></channel></rss>
 *     → {rss: {channel: {item: {title: "A"}}}}
 *   attributes → "@_name" keys; repeated siblings → arrays;
 *   text-only elements → plain strings; mixed content → "#text".
 * Parser is vendored yxml (SAX, fixed stack buffer); the tree is built
 * directly as JS values so peak memory is input string + output tree. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qjs_xml.h"
#include "buf.h"
#include "yxml.h"
#include <stdlib.h>
#include <string.h>

#define XML_MAX_DEPTH 128
#define XML_STACK_BUF 8192   /* yxml scratch: bounds nesting + name lengths */

typedef struct {
    char *name;      /* element name (owned) */
    JSValue obj;     /* JS_UNDEFINED until an attr or child forces an object */
    Buf text;        /* accumulated character content */
} XmlFrame;

/* Nodes get a NULL prototype: element names like "constructor" or "toString"
 * would otherwise collide with inherited Object.prototype members, and
 * node_add's JS_GetPropertyStr would see the inherited function as an
 * existing sibling and wrap it in an array. */
static JSValue new_node(JSContext *ctx) {
    return JS_NewObjectProto(ctx, JS_NULL);
}

/* Add child value under `name` on parent object: absent → set; present
 * once → wrap both in an array; already an array → push. */
static int node_add(JSContext *ctx, JSValue parent, const char *name, JSValue val) {
    JSValue cur = JS_GetPropertyStr(ctx, parent, name);
    if (JS_IsUndefined(cur)) {
        return JS_SetPropertyStr(ctx, parent, name, val) < 0 ? -1 : 0;
    }
    if (JS_IsArray(ctx, cur)) {
        JSValue lenv = JS_GetPropertyStr(ctx, cur, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lenv);
        JS_FreeValue(ctx, lenv);
        int rc = JS_SetPropertyUint32(ctx, cur, len, val) < 0 ? -1 : 0;
        JS_FreeValue(ctx, cur);
        return rc;
    }
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) { JS_FreeValue(ctx, cur); JS_FreeValue(ctx, val); return -1; }
    JS_SetPropertyUint32(ctx, arr, 0, cur);   /* takes ownership of cur */
    JS_SetPropertyUint32(ctx, arr, 1, val);
    return JS_SetPropertyStr(ctx, parent, name, arr) < 0 ? -1 : 0;
}

/* ── Entity leniency ────────────────────────────────────────────────────
 * XML defines five entities; real feeds ship &nbsp;, &mdash;, &rsquo; and
 * bare & anyway, and to a conforming parser each one is fatal — a single
 * stray reference loses the whole document rather than one item. So
 * references are screened on the way into yxml: the five built-ins and
 * numeric refs pass through untouched, the common HTML names are decoded
 * here, and anything else (including a lone &) is re-escaped so it survives
 * as literal text. Screening happens byte-by-byte on the way in, so it costs
 * no copy of the document. */
static const struct { const char *name; const char *utf8; } HTML_ENTITIES[] = {
    { "nbsp", "\xc2\xa0" },     { "mdash", "\xe2\x80\x94" }, { "ndash", "\xe2\x80\x93" },
    { "rsquo", "\xe2\x80\x99" },{ "lsquo", "\xe2\x80\x98" }, { "ldquo", "\xe2\x80\x9c" },
    { "rdquo", "\xe2\x80\x9d" },{ "hellip", "\xe2\x80\xa6" },{ "bull", "\xe2\x80\xa2" },
    { "copy", "\xc2\xa9" },     { "reg", "\xc2\xae" },       { "trade", "\xe2\x84\xa2" },
    { "deg", "\xc2\xb0" },      { "middot", "\xc2\xb7" },    { "laquo", "\xc2\xab" },
    { "raquo", "\xc2\xbb" },    { "eacute", "\xc3\xa9" },    { "egrave", "\xc3\xa8" },
    { "uuml", "\xc3\xbc" },     { "ouml", "\xc3\xb6" },      { "auml", "\xc3\xa4" },
};
#define XML_ENTITY_MAX_NAME 32

static int entity_is_builtin(const char *n, size_t len) {
    return (len == 3 && !memcmp(n, "amp", 3)) || (len == 2 && !memcmp(n, "lt", 2)) ||
           (len == 2 && !memcmp(n, "gt", 2))  || (len == 4 && !memcmp(n, "quot", 4)) ||
           (len == 4 && !memcmp(n, "apos", 4));
}

/* Fills `sub` with what should be fed to yxml in place of the reference at
 * `in` (which starts at '&'), and reports how many input bytes it consumed.
 * Returns 0 when the reference should pass through to yxml untouched. */
static int entity_rewrite(const char *in, size_t avail, char *sub, size_t subcap,
                          size_t *sub_len, size_t *consumed) {
    size_t n = 1;
    while (n < avail && n <= XML_ENTITY_MAX_NAME && in[n] != ';' &&
           in[n] != '&' && in[n] != '<' && in[n] != ' ' && in[n] != '\t' &&
           in[n] != '\n' && in[n] != '\r') n++;

    if (n >= avail || n > XML_ENTITY_MAX_NAME || in[n] != ';') {
        /* Not a reference at all — a bare & in text ("AT&T"). Escape it. */
        *sub_len = (size_t)snprintf(sub, subcap, "&amp;");
        *consumed = 1;
        return 1;
    }
    const char *name = in + 1;
    size_t nlen = n - 1;
    if (nlen == 0) {                       /* "&;" */
        *sub_len = (size_t)snprintf(sub, subcap, "&amp;");
        *consumed = 1;
        return 1;
    }
    if (name[0] == '#' || entity_is_builtin(name, nlen)) return 0;  /* yxml handles */

    for (unsigned e = 0; e < sizeof(HTML_ENTITIES)/sizeof(HTML_ENTITIES[0]); e++) {
        if (strlen(HTML_ENTITIES[e].name) == nlen &&
            !memcmp(HTML_ENTITIES[e].name, name, nlen)) {
            *sub_len = (size_t)snprintf(sub, subcap, "%s", HTML_ENTITIES[e].utf8);
            *consumed = n + 1;
            return 1;
        }
    }
    /* Unknown name: keep it as visible text rather than failing the parse. */
    *sub_len = (size_t)snprintf(sub, subcap, "&amp;%.*s;", (int)nlen, name);
    *consumed = n + 1;
    return 1;
}

/* yxml distinguishes five failures; collapsing them all to "syntax error"
 * throws away the only clue the agent has about what to change. */
static const char *yxml_error_text(yxml_ret_t r) {
    switch (r) {
    case YXML_EREF:   return "invalid character or entity reference";
    case YXML_ECLOSE: return "close tag does not match the open tag";
    case YXML_ESTACK: return "element name too long, or nesting too deep";
    case YXML_EEOF:   return "document ended mid-element (truncated?)";
    default:          return "syntax error";
    }
}

/* Trimmed view of a frame's text; returns length, sets *start. */
static size_t text_trimmed(Buf *b, const char **start) {
    const char *s = b->data ? b->data : "";
    size_t len = b->len;
    while (len && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) { s++; len--; }
    while (len && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n' || s[len-1] == '\r')) len--;
    *start = s;
    return len;
}

static void frame_clear(JSContext *ctx, XmlFrame *f) {
    free(f->name);
    if (!JS_IsUndefined(f->obj)) JS_FreeValue(ctx, f->obj);
    buf_free(&f->text);
}

static JSValue js_xml_parse(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "xml.parse: string argument required");
    size_t len = 0;
    const char *input = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!input) {
        /* A string argument that will not convert means the conversion itself
         * could not allocate — reporting that as a type error sends the caller
         * to fix an argument that was never wrong. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_IsString(argv[0])
            ? JS_ThrowInternalError(ctx, "xml.parse: out of memory")
            : JS_ThrowTypeError(ctx, "xml.parse: argument must be a string");
    }

    char *stack = malloc(XML_STACK_BUF);
    XmlFrame *frames = calloc(XML_MAX_DEPTH, sizeof(*frames));
    yxml_t x;
    if (!stack || !frames) {
        free(stack); free(frames);
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }
    yxml_init(&x, stack, XML_STACK_BUF);

    /* frames[0] is the synthetic root holder; the result is its obj. */
    int depth = 0;
    frames[0].obj = new_node(ctx);
    if (JS_IsException(frames[0].obj)) {
        free(frames); free(stack);
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }
    frames[0].name = NULL;
    Buf attrval = {0};
    const char *err = NULL;
    int err_oom = 0;   /* OOM is not a document defect — class it apart */

    /* Byte source: normally `input`, briefly a rewritten entity. `literal`
     * tracks CDATA and comment spans, where & carries no meaning and must
     * reach yxml exactly as written — the two regions are recognised by
     * their delimiters alone, no parser state is duplicated. */
    char sub[XML_ENTITY_MAX_NAME + 8];
    size_t sub_len = 0, sub_pos = 0, i = 0;
    enum { LIT_NONE, LIT_CDATA, LIT_COMMENT } literal = LIT_NONE;

    while (!err) {
        char c;
        if (sub_pos < sub_len) {
            c = sub[sub_pos++];
        } else if (i < len) {
            if (literal == LIT_NONE) {
                if (input[i] == '<' && len - i >= 9 && !memcmp(input + i, "<![CDATA[", 9))
                    literal = LIT_CDATA;
                else if (input[i] == '<' && len - i >= 4 && !memcmp(input + i, "<!--", 4))
                    literal = LIT_COMMENT;
                else if (input[i] == '&') {
                    size_t used = 0;
                    if (entity_rewrite(input + i, len - i, sub, sizeof sub, &sub_len, &used)) {
                        sub_pos = 0;
                        i += used;
                        continue;          /* re-enter and drain `sub` */
                    }
                }
            } else if (literal == LIT_CDATA && input[i] == ']' &&
                       len - i >= 3 && !memcmp(input + i, "]]>", 3)) {
                literal = LIT_NONE;
            } else if (literal == LIT_COMMENT && input[i] == '-' &&
                       len - i >= 3 && !memcmp(input + i, "-->", 3)) {
                literal = LIT_NONE;
            }
            c = input[i++];
        } else {
            yxml_ret_t re = yxml_eof(&x);
            if (re < 0) err = yxml_error_text(re);
            break;
        }

        yxml_ret_t r = yxml_parse(&x, c);
        switch (r) {
        case YXML_OK:
        case YXML_PISTART: case YXML_PICONTENT: case YXML_PIEND:
            break;
        case YXML_ELEMSTART:
            if (depth + 1 >= XML_MAX_DEPTH) { err = "document nested too deeply"; break; }
            depth++;
            frames[depth].name = strdup(x.elem);
            frames[depth].obj = JS_UNDEFINED;
            memset(&frames[depth].text, 0, sizeof(Buf));
            if (!frames[depth].name) { err = "out of memory"; err_oom = 1; }
            break;
        case YXML_CONTENT:
            buf_append_str(&frames[depth].text, x.data);
            break;
        case YXML_ELEMEND: {
            XmlFrame *f = &frames[depth];
            const char *ts; size_t tlen = text_trimmed(&f->text, &ts);
            JSValue val;
            if (JS_IsUndefined(f->obj)) {
                /* leaf: no attrs, no children → collapse to string */
                val = JS_NewStringLen(ctx, ts, tlen);
            } else {
                if (tlen) {
                    JSValue t = JS_NewStringLen(ctx, ts, tlen);
                    if (JS_IsException(t)) { err = "out of memory"; err_oom = 1; break; }
                    JS_SetPropertyStr(ctx, f->obj, "#text", t);
                }
                val = f->obj;
                f->obj = JS_UNDEFINED;
            }
            if (JS_IsException(val)) { err = "out of memory"; err_oom = 1; break; }
            XmlFrame *parent = &frames[depth - 1];
            if (depth > 1 && JS_IsUndefined(parent->obj)) {
                parent->obj = new_node(ctx);
                if (JS_IsException(parent->obj)) {
                    JS_FreeValue(ctx, val);
                    parent->obj = JS_UNDEFINED;
                    err = "out of memory"; err_oom = 1; break;
                }
            }
            if (node_add(ctx, parent->obj, f->name, val) != 0) {
                err = "out of memory"; err_oom = 1;
            }
            free(f->name); f->name = NULL;
            buf_free(&f->text);
            depth--;
            break;
        }
        case YXML_ATTRSTART:
            attrval.len = 0;
            break;
        case YXML_ATTRVAL:
            buf_append_str(&attrval, x.data);
            break;
        case YXML_ATTREND: {
            XmlFrame *f = &frames[depth];
            if (JS_IsUndefined(f->obj)) {
                f->obj = new_node(ctx);
                if (JS_IsException(f->obj)) {
                    f->obj = JS_UNDEFINED;
                    err = "out of memory"; err_oom = 1; break;
                }
            }
            char key[512] = "@_";
            size_t alen = strlen(x.attr);
            if (alen > sizeof(key) - 3) { err = "attribute name too long"; break; }
            memcpy(key + 2, x.attr, alen + 1);
            JSValue av = JS_NewStringLen(ctx, attrval.data ? attrval.data : "",
                                         attrval.len);
            if (JS_IsException(av)) { err = "out of memory"; err_oom = 1; break; }
            JS_SetPropertyStr(ctx, f->obj, key, av);
            break;
        }
        default:  /* YXML_E* */
            err = yxml_error_text(r);
            break;
        }
        if (!err && (frames[depth].text.oom || attrval.oom)) {
            err = "out of memory"; err_oom = 1;
        }
    }

    JSValue result;
    if (err) {
        for (int d = depth; d >= 0; d--) frame_clear(ctx, &frames[d]);
        /* Running out of heap says nothing about where in the document we
         * were, and a line/byte on it sends the agent hunting for malformed
         * XML that isn't there. The "out of memory" wording is also what
         * qjs_eval_run keys on to append the live heap limit. */
        result = err_oom
            ? JS_ThrowInternalError(ctx, "xml.parse: out of memory")
            : JS_ThrowSyntaxError(ctx, "xml.parse: %s at line %u byte %u",
                                  err, (unsigned)x.line, (unsigned)x.byte);
    } else {
        result = frames[0].obj;
    }
    buf_free(&attrval);
    free(frames);
    free(stack);
    JS_FreeCString(ctx, input);
    return result;
}

void qjs_register_xml(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue xml = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, xml, "parse", JS_NewCFunction(ctx, js_xml_parse, "parse", 1));
    /* XML.parse mirrors JSON.parse (the prior models reach for); the
     * lowercase alias catches the other guess for free. */
    JS_SetPropertyStr(ctx, global, "XML", JS_DupValue(ctx, xml));
    JS_SetPropertyStr(ctx, global, "xml", xml);
    JS_FreeValue(ctx, global);
}
