#define _POSIX_C_SOURCE 200809L
#include "tool_web_fetch.h"
#include "external_content.h"
#include "host_match.h"
#include "http.h"
#include "run_tool.h"
#include "buf.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>

/* Cap on bytes pulled per fetch. Oversized pages are truncated here (not
 * hard-errored) and the agent pages the captured text via offset/max_chars. */
#define WEB_FETCH_MAX (2 * 1024 * 1024)

static const char *WEB_FETCH_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"url\":{\"type\":\"string\",\"description\":\"URL to fetch (HTTP GET)\"},"
    "\"raw\":{\"type\":\"boolean\",\"description\":\"Return raw response without HTML-to-markdown conversion (auto-skipped for JSON responses)\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 60, max 600)\"},"
    "\"save_secret\":{\"type\":\"string\",\"description\":\"Capture a credential from this response: NAME (^[A-Z][A-Z0-9_]*$) stores it encrypted and masks it to {{SECRET:NAME}} — the raw value never enters context\"},"
    "\"save_secret_path\":{\"type\":\"string\",\"description\":\"With save_secret: JSON path (e.g. $.token) selecting the credential field; omit to capture the whole trimmed response\"}"
    "},\"required\":[\"url\"]}";

/* Browser-like request headers */
/* Browser-like UA to satisfy sites that 403 non-browser clients. Deliberately
 * Firefox, NOT Chrome: some CDNs (ESPN/CloudFront) sinkhole any UA containing
 * the literal "Chrome/" token with an empty 202 challenge — verified 2026-07-06
 * that a "Chrome/" UA gets 202/0 while Firefox, Safari, curl and empty all get
 * 200. A spoofed "Chrome/" without Chrome's TLS/JS fingerprint is the cheapest
 * bot tell, so it's the most-challenged token; avoiding it sidesteps the trap. */
static const char *FETCH_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:133.0) "
    "Gecko/20100101 Firefox/133.0";

/* Honest fallback UA, used only to retry a fetch that first came back empty.
 * The Firefox default sidesteps the known "Chrome/" sinkhole, but this is the
 * backstop for any other CDN that empty-shells our default UA yet serves a
 * plain client the real page (the inverse of sites that block bots). */
static const char *PLAIN_USER_AGENT = "cclaw/1.0";

/* ── HTML to Markdown converter ──────────────────────────────────── */

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

/* ── Actionable denial hint ──────────────────────────────────────── */

/* Extract the hostname from an http(s) URL (strips userinfo, port, and
 * IPv6 brackets). Returns 0 on success. */
static int url_parse_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) p = at + 1;
    if (*p == '[') {
        p++;
        const char *rb = memchr(p, ']', (size_t)(end - p));
        if (!rb) return -1;
        end = rb;
    } else {
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon) end = colon;
    }
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= cap) return -1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* After a fetch failure: if egress enforcement is active and the URL's host
 * is not allowlisted, tell the model which grant to request. Message only —
 * the per-hop proxy remains the enforcement (a redirect-hop denial still
 * yields the generic error). Returns malloc'd hint or NULL. */
char *web_fetch_host_hint(const char *url, char **rules, size_t n, int host_mode) {
    if (host_mode) return NULL; /* no enforcement — a hint would mislead */
    char host[256];
    if (url_parse_host(url, host, sizeof(host)) != 0) return NULL;
    if (host_match(rules, n, host)) return NULL;
    size_t cap = 2 * strlen(host) + 192;
    char *msg = malloc(cap);
    if (!msg) return NULL;
    snprintf(msg, cap,
             "error: host '%s' is not in your allowed hosts — request it with "
             "request_config {\"changes\":{\"grants\":{\"hosts\":[\"%s\"]}}}",
             host, host);
    return msg;
}

/* ── Tool handler ────────────────────────────────────────────────── */

static char *web_fetch_run(const RunToolParsed *q, WebFetchCtx *ctx, int *is_error) {
    /* Egress is enforced by the per-hop proxy, not a preflight; ctx (may be
     * NULL) is consulted only to phrase denials. Params are pre-extracted by
     * the parent — no JSON is parsed in this process. */
    const char *url = run_tool_param_str(q, "url");
    if (!url || !url[0])
        return tool_fail(is_error, "error: missing or empty 'url' field");

    int raw = run_tool_param_bool(q, "raw", 0);

    /* Egress is decided per-hop by the broker proxy (decide()) — no pre-flight
     * http_check_policy here: a single check can't see redirects (SSRF). */

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return tool_fail(is_error, "error: url must start with http:// or https://");

    /* Browser-like request with markdown preference */
    const char *hdrs[] = {
        "Accept: text/markdown, text/html;q=0.9, */*;q=0.1",
        "Accept-Language: en-US,en;q=0.9",
        NULL
    };
    /* The call's own budget (broker-clamped, wire field) minus a small margin
     * so curl gives up — and we say why — before the broker kills us. */
    int http_timeout = q->timeout > 5 ? q->timeout - 5 : q->timeout;
    if (http_timeout <= 0) http_timeout = 30;
    HttpRequestOpts opts = {
        .url = url,
        .method = "GET",
        .headers = hdrs,
        .timeout = http_timeout,
        .follow_redirects = 1,
        .max_redirects = 3,
        .max_response_bytes = WEB_FETCH_MAX,
        .user_agent = FETCH_USER_AGENT,
    };
    HttpResponse resp = {0};
    int status = http_do(&opts, &resp);

    /* Anti-bot fallback: some CDNs (e.g. ESPN behind CloudFront) hand a
     * browser-like UA an empty challenge shell (a 202, or a 2xx with no body)
     * while serving a plain UA the real page. If the first attempt came back
     * empty, retry once as cclaw/1.0. A page that is legitimately empty just
     * comes back empty again — the retry is one cheap extra GET, never a loop. */
    if (status >= 200 && status < 400 && resp.len == 0 &&
        opts.user_agent != PLAIN_USER_AGENT) {
        http_response_free(&resp);
        opts.user_agent = PLAIN_USER_AGENT;
        status = http_do(&opts, &resp);
    }

    if (status < 0) {
        /* An ungranted host fails as opaque curl/proxy noise — swap in the
         * grant hint when that's what happened. */
        char *hint = ctx ? web_fetch_host_hint(url, ctx->allowed_hosts,
                                               ctx->allowed_host_count,
                                               ctx->host_mode)
                         : NULL;
        if (hint) { http_response_free(&resp); return hint; }
        char *msg = malloc(512);
        if (msg) {
            if (resp.err_detail[0])
                snprintf(msg, 512, "error: HTTP request failed: %s", resp.err_detail);
            else
                snprintf(msg, 512, "error: HTTP request failed");
        }
        http_response_free(&resp);
        *is_error = 1;
        return msg ? msg : tool_fail(is_error, "error: HTTP request failed");
    }

    if (status >= 400) {
        char *msg = malloc(64);
        if (msg) snprintf(msg, 64, "error: HTTP %d", status);
        http_response_free(&resp);
        *is_error = 1;
        return msg ? msg : tool_fail(is_error, "error: HTTP error");
    }

    /* If server returned markdown or JSON, or raw requested, use body as-is */
    char *text;
    int skip_convert = raw
        || strncasecmp(resp.content_type, "text/markdown", 13) == 0
        || strncasecmp(resp.content_type, "application/json", 16) == 0;
    if (skip_convert) {
        text = resp.data;
        resp.data = NULL; /* take ownership */
    } else {
        text = html_to_markdown(resp.data, resp.len);
    }
    int capped = resp.truncated;  /* source larger than WEB_FETCH_MAX; tail dropped */
    http_response_free(&resp);

    if (!text) return tool_fail(is_error, "error: out of memory");

    /* Return the document whole. Oversized results are truncated and spilled
     * by the broker on the way out (spill_large_result), the same path every
     * other tool takes — web_fetch used to keep a private copy under
     * .tool_results and page it back with offset/max_chars, which is redundant
     * now that the agent gets a real file path it can read or grep. */
    size_t total_len = strlen(text);
    char meta[96];
    snprintf(meta, sizeof(meta), "[total=%zu%s]\n", total_len,
             capped ? " capped(source exceeded fetch limit)" : "");
    size_t meta_len = strlen(meta);

    /* No storage-time wrapping: the entry is tagged with network_hosts by the
     * broker frame, sanitized in the parent, and wrapped at query time. */
    char *out = malloc(meta_len + total_len + 1);
    if (!out) { free(text); return tool_fail(is_error, "error: out of memory"); }
    memcpy(out, meta, meta_len);
    memcpy(out + meta_len, text, total_len + 1);
    free(text);
    return out;
}

int tool_web_fetch_register(ToolRegistry *reg, WebFetchCtx *ctx) {
    int rc = tools_register(reg, "web_fetch",
                          "Fetch a URL via HTTP GET and return content as markdown. "
                          "Use raw:true to skip HTML-to-markdown (auto-skipped for JSON responses). "
                          "A long page is truncated in the result and written whole to a file "
                          "under the workspace — the path is named in the result, and you read "
                          "or grep it with your file tools for the rest. Examples:\n"
                          "  {\"url\":\"https://api.example.com/data\"} — JSON auto-detected, returned raw\n"
                          "  {\"url\":\"https://example.com\",\"raw\":true} — skip HTML-to-markdown conversion",
                          WEB_FETCH_PARAMS_JSON, tool_sandboxed_stub, ctx);
    if (rc == 0)  /* sandboxed broker; egress via per-hop proxy decide() */
        tools_set_recipe(reg, "web_fetch", (ToolRecipe){EXEC_SANDBOX, SBX_WEB, NULL});
    return rc;
}

char *tool_web_tier_run(const RunToolParsed *q, int *is_error) {
    /* Runs in the inner fork, inside the netns + proxy. Our libcurl honors the
     * HTTP_PROXY set by sandbox_child_setup → net_shim → broker → decide() on
     * every hop. The rebuilt ctx only phrases denials (host grant hint) —
     * egress stays the proxy's job. */
    WebFetchCtx ctx = {0};
    ctx.workspace = q->workspace;   /* rw-mounted at this path in the child */
    ctx.cwd_path = q->cwd_path;
    ctx.allowed_hosts = q->host_rules;
    ctx.allowed_host_count = q->host_count;
    ctx.host_mode = q->sandbox ? 0 : 1;
    char *r = web_fetch_run(q, &ctx, is_error);
    return r ? r : tool_fail(is_error, "error: web_fetch returned null");
}
