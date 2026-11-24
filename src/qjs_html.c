/* HTML.toMarkdown(str) — the HTML-to-markdown converter web_fetch uses,
 * exposed to every script in the JS tier. In C because it is a byte scanner
 * over pages up to the 2 MB fetch cap, and a slow ARM box should not run that
 * in an interpreter; it parses untrusted bytes, so it lives only here, in the
 * engine's host functions, which run inside the sandboxed tool child. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qjs_html.h"
#include "buf.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *extract_href(const char *tag_content, size_t tag_len) {
    const char *p = tag_content;
    while (p + 6 <= tag_content + tag_len) {
        if (strncasecmp(p, "href=\"", 6) == 0 || strncasecmp(p, "href='", 6) == 0) {
            char quote = p[5];
            const char *start = p + 6;
            const char *end = memchr(start, quote, (size_t)(tag_content + tag_len - start));
            if (end) {
                size_t len = (size_t)(end - start);
                char *href = malloc(len + 1);
                if (href) { memcpy(href, start, len); href[len] = '\0'; return href; }
            }
        }
        p++;
    }
    return NULL;
}

char *html_to_markdown(const char *html, size_t html_len) {
    Buf buf = {0};
    size_t i = 0;
    int in_script = 0, in_style = 0, last_was_newline = 0, consec_nl = 0;
    char *active_href = NULL;

    static const char *blocks[] = {
        "p", "div", "section", "article", "main", "header", "footer", "nav",
        "aside", "blockquote", "pre", "table", "tr", "th", "td", "ul",
        "ol", "dl", "dt", "dd", "form", "fieldset", "figure"
    };
    size_t num_blocks = sizeof(blocks) / sizeof(blocks[0]);

    while (i < html_len) {
        if (html[i] == '<') {
            const char *end_bracket = memchr(html + i, '>', html_len - i);
            if (!end_bracket) { i++; continue; }

            size_t tag_len_total = (size_t)(end_bracket - (html + i)) + 1;
            const char *tag_content = html + i + 1;
            size_t tag_content_len = tag_len_total - 2;

            int is_close = 0;
            const char *tname_start = tag_content;
            size_t tname_max = tag_content_len;

            while (tname_max > 0 && isspace((unsigned char)*tname_start)) { tname_start++; tname_max--; }
            if (tname_max > 0 && *tname_start == '/') { is_close = 1; tname_start++; tname_max--; }
            while (tname_max > 0 && isspace((unsigned char)*tname_start)) { tname_start++; tname_max--; }

            size_t tname_len = 0;
            while (tname_len < tname_max && !isspace((unsigned char)tname_start[tname_len]) &&
                   tname_start[tname_len] != '/' && tname_start[tname_len] != '>') {
                tname_len++;
            }

            if (is_close) {
                if (tname_len == 6 && strncasecmp(tname_start, "script", 6) == 0) in_script = 0;
                else if (tname_len == 5 && strncasecmp(tname_start, "style", 5) == 0) in_style = 0;
                else if (tname_len == 1 && tolower((unsigned char)tname_start[0]) == 'a') {
                    if (active_href) {
                        buf_append(&buf, "](", 2);
                        buf_append(&buf, active_href, strlen(active_href));
                        buf_append_char(&buf, ')');
                        free(active_href);
                        active_href = NULL;
                        last_was_newline = 0; consec_nl = 0;
                    }
                }
                i += tag_len_total; continue;
            }

            if (tname_len == 6 && strncasecmp(tname_start, "script", 6) == 0) { in_script = 1; i += tag_len_total; continue; }
            if (tname_len == 5 && strncasecmp(tname_start, "style", 5) == 0) { in_style = 1; i += tag_len_total; continue; }
            if (in_script || in_style) { i += tag_len_total; continue; }

            /* Block elements → newline */
            int is_block = 0;
            for (size_t b = 0; b < num_blocks; b++) {
                if (tname_len == strlen(blocks[b]) && strncasecmp(tname_start, blocks[b], tname_len) == 0) {
                    is_block = 1; break;
                }
            }
            if (is_block && !last_was_newline && buf.len > 0 && consec_nl < 2) {
                buf_append_char(&buf, '\n'); consec_nl++; last_was_newline = 1;
            }

            /* Markdown transforms */
            if (tname_len == 2 && tolower((unsigned char)tname_start[0]) == 'h' &&
                tname_start[1] >= '1' && tname_start[1] <= '6') {
                if (!last_was_newline && buf.len > 0 && consec_nl < 2) { buf_append_char(&buf, '\n'); consec_nl++; }
                int level = tname_start[1] - '0';
                for (int k = 0; k < level; k++) buf_append_char(&buf, '#');
                buf_append_char(&buf, ' ');
                last_was_newline = 0; consec_nl = 0;
            } else if (tname_len == 2 && strncasecmp(tname_start, "li", 2) == 0) {
                if (!last_was_newline && buf.len > 0 && consec_nl < 2) { buf_append_char(&buf, '\n'); consec_nl++; }
                buf_append(&buf, "- ", 2);
                last_was_newline = 0; consec_nl = 0;
            } else if (tname_len == 2 && strncasecmp(tname_start, "br", 2) == 0) {
                if (consec_nl < 2) { buf_append_char(&buf, '\n'); consec_nl++; }
                last_was_newline = 1;
            } else if (tname_len == 2 && strncasecmp(tname_start, "hr", 2) == 0) {
                if (!last_was_newline && consec_nl < 2) { buf_append_char(&buf, '\n'); consec_nl++; }
                buf_append(&buf, "---\n", 4);
                last_was_newline = 1; consec_nl = 1;
            } else if (tname_len == 1 && tolower((unsigned char)tname_start[0]) == 'a') {
                if (!active_href) {
                    active_href = extract_href(tag_content, tag_content_len);
                    if (active_href) buf_append_char(&buf, '[');
                }
            }

            i += tag_len_total; continue;
        }

        if (in_script || in_style) { i++; continue; }

        /* HTML entities */
        if (html[i] == '&') {
            char dec = 0; size_t cons = 0; size_t rem = html_len - i;
            if (rem >= 5 && strncmp(html + i, "&amp;", 5) == 0) { dec = '&'; cons = 5; }
            else if (rem >= 4 && strncmp(html + i, "&lt;", 4) == 0) { dec = '<'; cons = 4; }
            else if (rem >= 4 && strncmp(html + i, "&gt;", 4) == 0) { dec = '>'; cons = 4; }
            else if (rem >= 6 && strncmp(html + i, "&quot;", 6) == 0) { dec = '"'; cons = 6; }
            else if (rem >= 6 && strncmp(html + i, "&apos;", 6) == 0) { dec = '\''; cons = 6; }
            else if (rem >= 6 && strncmp(html + i, "&nbsp;", 6) == 0) { dec = ' '; cons = 6; }
            if (cons > 0) {
                buf_append_char(&buf, dec);
                last_was_newline = 0; consec_nl = 0;
                i += cons; continue;
            }
        }

        /* Whitespace normalization */
        char c = html[i];
        if (c == '\n' || c == '\r') {
            if (!last_was_newline && buf.len > 0 && buf.data[buf.len - 1] != ' ')
                buf_append_char(&buf, ' ');
        } else if (c == ' ' || c == '\t') {
            if (buf.len > 0 && buf.data[buf.len - 1] != ' ' && !last_was_newline)
                buf_append_char(&buf, ' ');
        } else {
            buf_append_char(&buf, c);
            last_was_newline = 0; consec_nl = 0;
        }
        i++;
    }

    while (buf.len > 0 && (buf.data[buf.len - 1] == ' ' || buf.data[buf.len - 1] == '\n')) {
        buf.len--; buf.data[buf.len] = '\0';
    }

    free(active_href);
    return buf_take(&buf);
}

static JSValue js_html_to_markdown(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    size_t len = 0;
    const char *html = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : NULL;
    if (!html)
        return JS_ThrowTypeError(ctx, "HTML.toMarkdown: argument must be a string");
    char *md = html_to_markdown(html, len);
    JS_FreeCString(ctx, html);
    if (!md) return JS_ThrowInternalError(ctx, "HTML.toMarkdown: out of memory");
    JSValue v = JS_NewString(ctx, md);
    free(md);
    return v;
}

void qjs_register_html(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue html = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, html, "toMarkdown",
                      JS_NewCFunction(ctx, js_html_to_markdown, "toMarkdown", 1));
    JS_SetPropertyStr(ctx, global, "HTML", html);
    JS_FreeValue(ctx, global);
}
