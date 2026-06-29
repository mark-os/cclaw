#define _POSIX_C_SOURCE 200809L
#include "tool_web_fetch.h"
#include "external_content.h"
#include "http.h"
#include "http_policy.h"
#include "tool_parse.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

#define WEB_FETCH_MAX (512 * 1024)
#define WEB_FETCH_DEFAULT_MAX_CHARS 20000

static const char *WEB_FETCH_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"url\":{\"type\":\"string\",\"description\":\"URL to fetch (HTTP GET)\"},"
    "\"offset\":{\"type\":\"integer\",\"description\":\"Character offset to start from (default 0)\"},"
    "\"max_chars\":{\"type\":\"integer\",\"description\":\"Max characters to return (default 20000)\"}"
    "},\"required\":[\"url\"]}";

/* Browser-like request headers */
static const char *FETCH_USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_7_2) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";

/* ── HTML to Markdown converter ──────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MdBuf;

static void mdbuf_append(MdBuf *buf, const char *str, size_t len) {
    if (buf->len + len + 1 > buf->cap) {
        size_t nc = buf->cap ? buf->cap * 2 : 4096;
        while (nc < buf->len + len + 1) nc *= 2;
        char *tmp = realloc(buf->data, nc);
        if (!tmp) return;
        buf->data = tmp;
        buf->cap = nc;
    }
    memcpy(buf->data + buf->len, str, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void mdbuf_append_char(MdBuf *buf, char c) {
    mdbuf_append(buf, &c, 1);
}

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

static char *html_to_markdown(const char *html, size_t html_len) {
    MdBuf buf = {0};
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
                        mdbuf_append(&buf, "](", 2);
                        mdbuf_append(&buf, active_href, strlen(active_href));
                        mdbuf_append_char(&buf, ')');
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
                mdbuf_append_char(&buf, '\n'); consec_nl++; last_was_newline = 1;
            }

            /* Markdown transforms */
            if (tname_len == 2 && tolower((unsigned char)tname_start[0]) == 'h' &&
                tname_start[1] >= '1' && tname_start[1] <= '6') {
                if (!last_was_newline && buf.len > 0 && consec_nl < 2) { mdbuf_append_char(&buf, '\n'); consec_nl++; }
                int level = tname_start[1] - '0';
                for (int k = 0; k < level; k++) mdbuf_append_char(&buf, '#');
                mdbuf_append_char(&buf, ' ');
                last_was_newline = 0; consec_nl = 0;
            } else if (tname_len == 2 && strncasecmp(tname_start, "li", 2) == 0) {
                if (!last_was_newline && buf.len > 0 && consec_nl < 2) { mdbuf_append_char(&buf, '\n'); consec_nl++; }
                mdbuf_append(&buf, "- ", 2);
                last_was_newline = 0; consec_nl = 0;
            } else if (tname_len == 2 && strncasecmp(tname_start, "br", 2) == 0) {
                if (consec_nl < 2) { mdbuf_append_char(&buf, '\n'); consec_nl++; }
                last_was_newline = 1;
            } else if (tname_len == 2 && strncasecmp(tname_start, "hr", 2) == 0) {
                if (!last_was_newline && consec_nl < 2) { mdbuf_append_char(&buf, '\n'); consec_nl++; }
                mdbuf_append(&buf, "---\n", 4);
                last_was_newline = 1; consec_nl = 1;
            } else if (tname_len == 1 && tolower((unsigned char)tname_start[0]) == 'a') {
                if (!active_href) {
                    active_href = extract_href(tag_content, tag_content_len);
                    if (active_href) mdbuf_append_char(&buf, '[');
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
                mdbuf_append_char(&buf, dec);
                last_was_newline = 0; consec_nl = 0;
                i += cons; continue;
            }
        }

        /* Whitespace normalization */
        char c = html[i];
        if (c == '\n' || c == '\r') {
            if (!last_was_newline && buf.len > 0 && buf.data[buf.len - 1] != ' ')
                mdbuf_append_char(&buf, ' ');
        } else if (c == ' ' || c == '\t') {
            if (buf.len > 0 && buf.data[buf.len - 1] != ' ' && !last_was_newline)
                mdbuf_append_char(&buf, ' ');
        } else {
            mdbuf_append_char(&buf, c);
            last_was_newline = 0; consec_nl = 0;
        }
        i++;
    }

    while (buf.len > 0 && (buf.data[buf.len - 1] == ' ' || buf.data[buf.len - 1] == '\n')) {
        buf.len--; buf.data[buf.len] = '\0';
    }

    free(active_href);
    if (!buf.data) return strdup("");
    return buf.data;
}

/* ── Legacy html_strip_tags (kept for backward compat / tests) ─── */

size_t html_strip_tags(const char *src, char *dst, size_t dst_cap) {
    char *md = html_to_markdown(src, strlen(src));
    if (!md) { dst[0] = '\0'; return 0; }
    size_t len = strlen(md);
    if (len >= dst_cap) len = dst_cap - 1;
    memcpy(dst, md, len);
    dst[len] = '\0';
    free(md);
    return len;
}

/* ── Tool handler ────────────────────────────────────────────────── */

char *tool_web_fetch_handler(const char *arguments, void *user_data) {
    (void)user_data;  /* egress is enforced by the per-hop proxy, not a preflight */

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *url = targ_str(&ta, "url");
    if (!url || !url[0]) {
        tool_parse_free(&ta);
        return strdup("error: missing or empty 'url' field");
    }

    int offset = targ_int(&ta, "offset", 0);
    if (offset < 0) offset = 0;
    int max_chars = targ_int(&ta, "max_chars", WEB_FETCH_DEFAULT_MAX_CHARS);
    if (max_chars <= 0) max_chars = WEB_FETCH_DEFAULT_MAX_CHARS;

    /* Egress is decided per-hop by the broker proxy (decide()) — no pre-flight
     * http_check_policy here: a single check can't see redirects (SSRF). */

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        tool_parse_free(&ta);
        return strdup("error: url must start with http:// or https://");
    }

    /* Browser-like request with markdown preference */
    const char *hdrs[] = {
        "Accept: text/markdown, text/html;q=0.9, */*;q=0.1",
        "Accept-Language: en-US,en;q=0.9",
        NULL
    };
    HttpRequestOpts opts = {
        .url = url,
        .method = "GET",
        .headers = hdrs,
        .timeout = 30,
        .follow_redirects = 1,
        .max_redirects = 3,
        .max_response_bytes = WEB_FETCH_MAX,
        .user_agent = FETCH_USER_AGENT,
    };
    HttpResponse resp = {0};
    int status = http_do(&opts, &resp);
    tool_parse_free(&ta);

    if (status < 0) {
        char *msg = malloc(512);
        if (msg) {
            if (resp.err_detail[0])
                snprintf(msg, 512, "error: HTTP request failed: %s", resp.err_detail);
            else
                snprintf(msg, 512, "error: HTTP request failed");
        }
        http_response_free(&resp);
        return msg ? msg : strdup("error: HTTP request failed");
    }

    if (status >= 400) {
        char *msg = malloc(64);
        if (msg) snprintf(msg, 64, "error: HTTP %d", status);
        http_response_free(&resp);
        return msg ? msg : strdup("error: HTTP error");
    }

    /* If server returned markdown, use body as-is; otherwise convert HTML */
    char *text;
    if (strncasecmp(resp.content_type, "text/markdown", 13) == 0) {
        text = resp.data;
        resp.data = NULL; /* take ownership */
    } else {
        text = html_to_markdown(resp.data, resp.len);
    }
    http_response_free(&resp);

    if (!text) return strdup("error: out of memory");

    /* V15: sanitize and wrap with randomized boundaries */
    /* Truncation with metadata */
    size_t total_len = strlen(text);
    size_t off = ((size_t)offset < total_len) ? (size_t)offset : total_len;
    size_t remaining = total_len - off;
    size_t slice_len = ((size_t)max_chars < remaining) ? (size_t)max_chars : remaining;
    int truncated = (off + slice_len < total_len);

    /* Build metadata + slice */
    char meta[128];
    snprintf(meta, sizeof(meta), "[offset=%zu max_chars=%d total=%zu%s]\n",
             off, max_chars, total_len, truncated ? " truncated" : "");
    size_t meta_len = strlen(meta);

    char *slice = malloc(meta_len + slice_len + 1);
    if (!slice) { free(text); return strdup("error: out of memory"); }
    memcpy(slice, meta, meta_len);
    memcpy(slice + meta_len, text + off, slice_len);
    slice[meta_len + slice_len] = '\0';
    free(text);

    char *result = wrap_external_content(slice, meta_len + slice_len, "web_fetch");
    free(slice);
    return result ? result : strdup("error: out of memory");
}

int tool_web_fetch_register(ToolRegistry *reg, WebFetchCtx *ctx) {
    int rc = tools_register(reg, "web_fetch",
                          "Fetch a URL via HTTP GET and return content as markdown",
                          WEB_FETCH_PARAMS_JSON, tool_web_fetch_handler, ctx);
    if (rc == 0)  /* sandboxed broker; egress via per-hop proxy decide() */
        tools_set_recipe(reg, "web_fetch", (ToolRecipe){EXEC_SANDBOX, SBX_WEB, NULL});
    return rc;
}
