#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "json_escape.h"
#include "sse_parse.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Strip leading whitespace (provider keep-alive newlines) from response in-place */
static void resp_strip_leading_ws(HttpResponse *resp) {
    if (!resp->data || resp->len == 0) return;
    size_t skip = 0;
    while (skip < resp->len && (resp->data[skip] == '\n' || resp->data[skip] == '\r'
                                || resp->data[skip] == ' ' || resp->data[skip] == '\t'))
        skip++;
    if (skip == 0) return;
    resp->len -= skip;
    memmove(resp->data, resp->data + skip, resp->len + 1);
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    HttpResponse *resp = userdata;

    if (resp->max_bytes > 0 && resp->len + bytes > resp->max_bytes)
        return 0;

    if (resp->len + bytes + 1 > resp->cap) {
        size_t new_cap = (resp->cap == 0) ? 4096 : resp->cap;
        while (new_cap < resp->len + bytes + 1)
            new_cap *= 2;
        char *tmp = realloc(resp->data, new_cap);
        if (!tmp) return 0;
        resp->data = tmp;
        resp->cap = new_cap;
    }

    memcpy(resp->data + resp->len, ptr, bytes);
    resp->len += bytes;
    resp->data[resp->len] = '\0';
    return bytes;
}

/* V2: capture Retry-After and Content-Type headers */
static size_t header_cb(char *buf, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    HttpResponse *resp = userdata;

    /* Case-insensitive header matching */
    if (bytes > 12) {
        const char *prefix = "retry-after:";
        int match = 1;
        for (size_t i = 0; i < 12; i++) {
            if (tolower((unsigned char)buf[i]) != prefix[i]) { match = 0; break; }
        }
        if (match) {
            const char *val = buf + 12;
            size_t vlen = bytes - 12;
            while (vlen > 0 && *val == ' ') { val++; vlen--; }
            char tmp[32];
            size_t tlen = vlen < (sizeof(tmp)-1) ? vlen : (sizeof(tmp)-1);
            memcpy(tmp, val, tlen);
            tmp[tlen] = '\0';
            resp->retry_after = atoi(tmp);
            if (resp->retry_after < 1) resp->retry_after = 1;
        }
    }
    if (bytes > 13) {
        const char *prefix = "content-type:";
        int match = 1;
        for (size_t i = 0; i < 13; i++) {
            if (tolower((unsigned char)buf[i]) != prefix[i]) { match = 0; break; }
        }
        if (match) {
            const char *val = buf + 13;
            size_t vlen = bytes - 13;
            while (vlen > 0 && *val == ' ') { val++; vlen--; }
            while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == '\n')) vlen--;
            if (vlen >= sizeof(resp->content_type)) vlen = sizeof(resp->content_type) - 1;
            memcpy(resp->content_type, val, vlen);
            resp->content_type[vlen] = '\0';
        }
    }
    return bytes;
}

/* json_unescape is in json_escape.c */

/* T294: Process one SSE "data:" line using jsmn-based per-provider parsers. */
static void sse_process_line(SseCtx *ctx, const char *line, size_t len) {
    if (len < 5 || memcmp(line, "data:", 5) != 0) return;
    const char *json = line + 5;
    size_t jlen = len - 5;
    while (jlen > 0 && (*json == ' ')) { json++; jlen--; }

    if (jlen >= 6 && memcmp(json, "[DONE]", 6) == 0) return;

    SseChunk chunk;
    int rc;
    if (memchr(json, '{', jlen) && strstr(json, "\"candidates\""))
        rc = sse_parse_gemini(json, jlen, &chunk);
    else
        rc = sse_parse_openai(json, jlen, &chunk);

    if (rc != 0) return;

    /* Content text */
    if (chunk.text && chunk.text_len > 0) {
        char *unesc = malloc(chunk.text_len + 1);
        if (unesc) {
            size_t ulen = json_unescape(unesc, chunk.text_len + 1, chunk.text, chunk.text_len);
            if (ctx->content_len + ulen + 1 > ctx->content_cap) {
                size_t nc = ctx->content_cap ? ctx->content_cap * 2 : 4096;
                while (nc < ctx->content_len + ulen + 1) nc *= 2;
                char *tmp = realloc(ctx->content, nc);
                if (tmp) { ctx->content = tmp; ctx->content_cap = nc; }
            }
            if (ctx->content) {
                memcpy(ctx->content + ctx->content_len, unesc, ulen);
                ctx->content_len += ulen;
                ctx->content[ctx->content_len] = '\0';
            }
            if (ctx->sse_cb) {
                if (ctx->reasoning_started) {
                    ctx->sse_cb("\033[0m\n", 5, ctx->sse_data);
                    ctx->reasoning_started = 0;
                }
                ctx->sse_cb(unesc, ulen, ctx->sse_data);
            }
            free(unesc);
        }
    }

    /* Reasoning text */
    if (chunk.reasoning && chunk.reasoning_len > 0) {
        char *unesc = malloc(chunk.reasoning_len + 1);
        if (unesc) {
            size_t ulen = json_unescape(unesc, chunk.reasoning_len + 1, chunk.reasoning, chunk.reasoning_len);
            if (ctx->reasoning_len + ulen + 1 > ctx->reasoning_cap) {
                size_t nc = ctx->reasoning_cap ? ctx->reasoning_cap * 2 : 4096;
                while (nc < ctx->reasoning_len + ulen + 1) nc *= 2;
                char *tmp = realloc(ctx->reasoning, nc);
                if (tmp) { ctx->reasoning = tmp; ctx->reasoning_cap = nc; }
            }
            if (ctx->reasoning) {
                memcpy(ctx->reasoning + ctx->reasoning_len, unesc, ulen);
                ctx->reasoning_len += ulen;
                ctx->reasoning[ctx->reasoning_len] = '\0';
            }
            if (ctx->sse_cb) {
                if (!ctx->reasoning_started) {
                    ctx->reasoning_started = 1;
                    ctx->sse_cb("\033[2m", 4, ctx->sse_data);
                }
                ctx->sse_cb(unesc, ulen, ctx->sse_data);
            }
            free(unesc);
        }
    }

    /* Tool calls */
    if (chunk.tc_index >= 0) {
        size_t idx = (size_t)chunk.tc_index;
        if (idx >= ctx->tc_alloc) {
            size_t na = idx + 4;
            ctx->tc_ids = realloc(ctx->tc_ids, na * sizeof(char *));
            ctx->tc_names = realloc(ctx->tc_names, na * sizeof(char *));
            ctx->tc_args = realloc(ctx->tc_args, na * sizeof(char *));
            ctx->tc_arg_lens = realloc(ctx->tc_arg_lens, na * sizeof(size_t));
            ctx->tc_arg_caps = realloc(ctx->tc_arg_caps, na * sizeof(size_t));
            for (size_t i = ctx->tc_alloc; i < na; i++) {
                ctx->tc_ids[i] = NULL;
                ctx->tc_names[i] = NULL;
                ctx->tc_args[i] = NULL;
                ctx->tc_arg_lens[i] = 0;
                ctx->tc_arg_caps[i] = 0;
            }
            ctx->tc_alloc = na;
        }
        if (idx >= ctx->tc_count) ctx->tc_count = idx + 1;

        if (chunk.tc_id && chunk.tc_id_len > 0 && !ctx->tc_ids[idx])
            ctx->tc_ids[idx] = strndup(chunk.tc_id, chunk.tc_id_len);
        else if (!ctx->tc_ids[idx]) {
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "call_%zu", idx);
            ctx->tc_ids[idx] = strdup(id_buf);
        }

        if (chunk.tc_name && chunk.tc_name_len > 0 && !ctx->tc_names[idx])
            ctx->tc_names[idx] = strndup(chunk.tc_name, chunk.tc_name_len);

        if (chunk.tc_args && chunk.tc_args_len > 0) {
            if (chunk.tc_args_complete) {
                free(ctx->tc_args[idx]);
                ctx->tc_args[idx] = strndup(chunk.tc_args, chunk.tc_args_len);
                ctx->tc_arg_lens[idx] = chunk.tc_args_len;
                ctx->tc_arg_caps[idx] = chunk.tc_args_len + 1;
            } else {
                char *ue = malloc(chunk.tc_args_len + 1);
                if (ue) {
                    size_t ul = json_unescape(ue, chunk.tc_args_len + 1, chunk.tc_args, chunk.tc_args_len);
                    if (ctx->tc_arg_lens[idx] + ul + 1 > ctx->tc_arg_caps[idx]) {
                        size_t nc = ctx->tc_arg_caps[idx] ? ctx->tc_arg_caps[idx] * 2 : 256;
                        while (nc < ctx->tc_arg_lens[idx] + ul + 1) nc *= 2;
                        ctx->tc_args[idx] = realloc(ctx->tc_args[idx], nc);
                        ctx->tc_arg_caps[idx] = nc;
                    }
                    if (ctx->tc_args[idx]) {
                        memcpy(ctx->tc_args[idx] + ctx->tc_arg_lens[idx], ue, ul);
                        ctx->tc_arg_lens[idx] += ul;
                        ctx->tc_args[idx][ctx->tc_arg_lens[idx]] = '\0';
                    }
                    free(ue);
                }
            }
        }
    }

    /* Finish reason */
    if (chunk.finish && chunk.finish_len > 0) {
        free(ctx->finish_reason);
        ctx->finish_reason = NULL;
        if (chunk.finish_len == 4 && memcmp(chunk.finish, "STOP", 4) == 0)
            ctx->finish_reason = strdup("stop");
        else if (chunk.finish_len == 10 && memcmp(chunk.finish, "MAX_TOKENS", 10) == 0)
            ctx->finish_reason = strdup("length");
        else if (chunk.finish_len == 4 && memcmp(chunk.finish, "null", 4) == 0)
            ; /* skip null */
        else
            ctx->finish_reason = strndup(chunk.finish, chunk.finish_len);
    }

    /* Usage */
    if (chunk.prompt_tokens) ctx->prompt_tokens = chunk.prompt_tokens;
    if (chunk.completion_tokens) ctx->completion_tokens = chunk.completion_tokens;
    if (chunk.total_tokens) ctx->total_tokens = chunk.total_tokens;
    if (chunk.cache_read_tokens) ctx->cache_read_tokens = chunk.cache_read_tokens;
    if (chunk.cache_write_tokens) ctx->cache_write_tokens = chunk.cache_write_tokens;
    if (chunk.cost_nano) ctx->cost_nano = chunk.cost_nano;
}

static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    SseCtx *ctx = userdata;
    const char *data = ptr;

    /* Also accumulate raw response for error detection */
    HttpResponse *resp = ctx->resp;
    if (resp->len + bytes + 1 > resp->cap) {
        size_t new_cap = (resp->cap == 0) ? 4096 : resp->cap;
        while (new_cap < resp->len + bytes + 1) new_cap *= 2;
        char *tmp = realloc(resp->data, new_cap);
        if (!tmp) return 0;
        resp->data = tmp;
        resp->cap = new_cap;
    }
    memcpy(resp->data + resp->len, data, bytes);
    resp->len += bytes;
    resp->data[resp->len] = '\0';

    /* Process line-by-line (SSE lines end with \n) */
    for (size_t i = 0; i < bytes; i++) {
        char c = data[i];
        if (c == '\n') {
            if (ctx->line_len > 0) {
                if (ctx->line_buf[ctx->line_len - 1] == '\r')
                    ctx->line_len--;
                ctx->line_buf[ctx->line_len] = '\0';
                sse_process_line(ctx, ctx->line_buf, ctx->line_len);
            }
            ctx->line_len = 0;
        } else {
            if (ctx->line_len + 1 >= ctx->line_cap) {
                size_t nc = ctx->line_cap ? ctx->line_cap * 2 : 1024;
                char *tmp = realloc(ctx->line_buf, nc);
                if (!tmp) return 0;
                ctx->line_buf = tmp;
                ctx->line_cap = nc;
            }
            ctx->line_buf[ctx->line_len++] = c;
        }
    }

    return bytes;
}

void sse_ctx_free(SseCtx *ctx) {
    if (!ctx) return;
    free(ctx->line_buf);
    free(ctx->content);
    free(ctx->reasoning);
    free(ctx->finish_reason);
    for (size_t i = 0; i < ctx->tc_alloc; i++) {
        free(ctx->tc_ids[i]);
        free(ctx->tc_names[i]);
        free(ctx->tc_args[i]);
    }
    free(ctx->tc_ids);
    free(ctx->tc_names);
    free(ctx->tc_args);
    free(ctx->tc_arg_lens);
    free(ctx->tc_arg_caps);
}

/* General-purpose HTTP request */
int http_do(const HttpRequestOpts *opts, HttpResponse *resp) {
    memset(resp, 0, sizeof(*resp));
    if (!opts || !opts->url) return -1;

    resp->max_bytes = opts->max_response_bytes;

    int own_curl = 0;
    CURL *curl = opts->curl_handle;
    if (!curl) {
        curl = curl_easy_init();
        own_curl = 1;
    } else {
        curl_easy_reset(curl);
    }
    if (!curl) return -1;

    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_URL, opts->url);

    /* Method */
    int is_post = (opts->body || opts->read_cb);
    const char *method = opts->method;
    if (!method) method = is_post ? "POST" : "GET";

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    /* Body */
    if (opts->body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, opts->body);
    } else if (opts->read_cb) {
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, opts->read_cb);
        curl_easy_setopt(curl, CURLOPT_READDATA, opts->read_data);
    }

    /* Headers */
    struct curl_slist *hlist = NULL;
    if (opts->headers) {
        for (const char **h = opts->headers; *h; h++)
            hlist = curl_slist_append(hlist, *h);
    }
    if (opts->read_cb)
        hlist = curl_slist_append(hlist, "Transfer-Encoding: chunked");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    /* Timeout */
    long timeout = opts->timeout > 0 ? opts->timeout : 300L;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

    /* Redirects */
    if (opts->follow_redirects) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, opts->max_redirects > 0 ? (long)opts->max_redirects : 5L);
    }

    /* User agent */
    if (opts->user_agent)
        curl_easy_setopt(curl, CURLOPT_USERAGENT, opts->user_agent);

    /* SSE or regular write callback */
    SseCtx sse_ctx = {0};
    if (opts->sse_cb) {
        sse_ctx.resp = resp;
        sse_ctx.sse_cb = opts->sse_cb;
        sse_ctx.sse_data = opts->sse_data;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sse_ctx);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    }

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        /* Task 6: connection reuse metrics (visible at TRACE level via caller) */
        long num_connects = 0;
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &num_connects);
        resp->conn_reused = (num_connects == 0);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &resp->ttfb);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &resp->tls_time);
    } else if (rc == CURLE_OPERATION_TIMEDOUT)
        status = -2;

    /* Stash curl error */
    if (rc != CURLE_OK && resp->err_detail[0] == '\0') {
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        snprintf(resp->err_detail, sizeof(resp->err_detail), "%s", msg);
    }

    curl_slist_free_all(hlist);
    if (own_curl) curl_easy_cleanup(curl);

    /* SSE: transfer or free accumulated context */
    if (opts->sse_cb) {
        if (opts->out_ctx) {
            *opts->out_ctx = sse_ctx;
            /* Clear local so nothing gets double-freed */
            memset(&sse_ctx, 0, sizeof(sse_ctx));
        } else {
            sse_ctx_free(&sse_ctx);
        }
    }

    if (!opts->sse_cb)
        resp_strip_leading_ws(resp);

    return (int)status;
}

int http_post(const char *url, const char **headers, const char *body,
              HttpResponse *resp) {
    HttpRequestOpts opts = {
        .url = url,
        .method = "POST",
        .body = body,
        .headers = headers,
    };
    return http_do(&opts, resp);
}

int http_post_stream(const char *url, const char **headers,
                     HttpReadFn read_cb, void *read_data,
                     HttpResponse *resp) {
    HttpRequestOpts opts = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .read_cb = read_cb,
        .read_data = read_data,
    };
    return http_do(&opts, resp);
}

int http_post_stream_sse(const char *url, const char **headers,
                         HttpReadFn read_cb, void *read_data,
                         HttpSseFn sse_cb, void *sse_data,
                         HttpResponse *resp, SseCtx *out_ctx) {
    HttpRequestOpts opts = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .read_cb = read_cb,
        .read_data = read_data,
        .sse_cb = sse_cb,
        .sse_data = sse_data,
        .out_ctx = out_ctx,
    };
    return http_do(&opts, resp);
}

void http_response_free(HttpResponse *resp) {
    free(resp->data);
    resp->data = NULL;
    resp->len = 0;
    resp->cap = 0;
}
