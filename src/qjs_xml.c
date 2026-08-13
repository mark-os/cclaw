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
    JS_SetPropertyUint32(ctx, arr, 0, cur);   /* takes ownership of cur */
    JS_SetPropertyUint32(ctx, arr, 1, val);
    return JS_SetPropertyStr(ctx, parent, name, arr) < 0 ? -1 : 0;
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
    if (!input)
        return JS_ThrowTypeError(ctx, "xml.parse: argument must be a string");

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
    frames[0].obj = JS_NewObject(ctx);
    frames[0].name = NULL;
    Buf attrval = {0};
    const char *err = NULL;

    for (size_t i = 0; i <= len && !err; i++) {
        yxml_ret_t r = (i < len) ? yxml_parse(&x, input[i]) : yxml_eof(&x);
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
            if (!frames[depth].name) err = "out of memory";
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
                if (tlen)
                    JS_SetPropertyStr(ctx, f->obj, "#text", JS_NewStringLen(ctx, ts, tlen));
                val = f->obj;
                f->obj = JS_UNDEFINED;
            }
            XmlFrame *parent = &frames[depth - 1];
            if (depth > 1 && JS_IsUndefined(parent->obj))
                parent->obj = JS_NewObject(ctx);
            if (node_add(ctx, parent->obj, f->name, val) != 0)
                err = "out of memory";
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
            if (JS_IsUndefined(f->obj)) f->obj = JS_NewObject(ctx);
            char key[512] = "@_";
            size_t alen = strlen(x.attr);
            if (alen > sizeof(key) - 3) { err = "attribute name too long"; break; }
            memcpy(key + 2, x.attr, alen + 1);
            JS_SetPropertyStr(ctx, f->obj, key,
                JS_NewStringLen(ctx, attrval.data ? attrval.data : "", attrval.len));
            break;
        }
        default:  /* YXML_E* */
            err = "syntax error";
            break;
        }
        if (!err && (frames[depth].text.oom || attrval.oom))
            err = "out of memory";
    }

    JSValue result;
    if (err) {
        for (int d = depth; d >= 0; d--) frame_clear(ctx, &frames[d]);
        result = JS_ThrowSyntaxError(ctx, "xml.parse: %s at line %u byte %u",
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
