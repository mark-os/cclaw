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

/* V2: capture Retry-After header */
static size_t header_cb(char *buf, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    HttpResponse *resp = userdata;
    const char *prefix = "retry-after:";
    size_t plen = 12;
    if (bytes > plen) {
        /* Case-insensitive check */
        int match = 1;
        for (size_t i = 0; i < plen; i++) {
            if (tolower((unsigned char)buf[i]) != prefix[i]) { match = 0; break; }
        }
        if (match) {
            const char *val = buf + plen;
            while (*val == ' ') val++;
            resp->retry_after = atoi(val);
            if (resp->retry_after < 1) resp->retry_after = 1;
        }
    }
    return bytes;
}

int http_post(const char *url, const char **headers, const char *body,
              HttpResponse *resp) {
    memset(resp, 0, sizeof(*resp));

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *hlist = NULL;
    if (headers) {
        for (const char **h = headers; *h; h++)
            hlist = curl_slist_append(hlist, *h);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    else if (rc == CURLE_OPERATION_TIMEDOUT)
        status = -2;

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);

    resp_strip_leading_ws(resp);
    return (int)status;
}

void http_response_free(HttpResponse *resp) {
    free(resp->data);
    resp->data = NULL;
    resp->len = 0;
    resp->cap = 0;
}

int http_post_stream(const char *url, const char **headers,
                     HttpReadFn read_cb, void *read_data,
                     HttpResponse *resp) {
    memset(resp, 0, sizeof(*resp));

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *hlist = NULL;
    if (headers) {
        for (const char **h = headers; *h; h++)
            hlist = curl_slist_append(hlist, *h);
    }
    /* Chunked transfer since content-length unknown */
    hlist = curl_slist_append(hlist, "Transfer-Encoding: chunked");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, read_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    else if (rc == CURLE_OPERATION_TIMEDOUT)
        status = -2;

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);

    resp_strip_leading_ws(resp);
    return (int)status;
}

/* SSE write callback context */
typedef struct {
    HttpResponse *resp;      /* accumulate full raw SSE response */
    HttpSseFn sse_cb;
    void *sse_data;
    /* Line buffer for partial SSE lines */
    char *line_buf;
    size_t line_len;
    size_t line_cap;
    /* Accumulated content for final response reconstruction */
    char *content;
    size_t content_len;
    size_t content_cap;
    /* Accumulated reasoning tokens */
    char *reasoning;
    size_t reasoning_len;
    size_t reasoning_cap;
    int reasoning_started;   /* flag: already printed header */
    /* Tool call accumulation (index-based assembly) */
    char **tc_ids;      /* array of tool call IDs */
    char **tc_names;    /* array of tool call function names */
    char **tc_args;     /* array of accumulated argument strings */
    size_t *tc_arg_lens;
    size_t *tc_arg_caps;
    size_t tc_count;
    size_t tc_alloc;
    char *finish_reason;
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int cache_read_tokens;
    int cache_write_tokens;
    int64_t cost_nano;
} SseCtx;

/* Unescape a JSON string segment in-place into dest. Returns bytes written. */
static size_t json_unescape(char *dest, size_t cap, const char *src, size_t src_len) {
    size_t w = 0;
    for (size_t i = 0; i < src_len && w < cap; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
            case 'n': dest[w++] = '\n'; break;
            case 'r': dest[w++] = '\r'; break;
            case 't': dest[w++] = '\t'; break;
            case '"': dest[w++] = '"'; break;
            case '\\': dest[w++] = '\\'; break;
            case '/': dest[w++] = '/'; break;
            case 'u':
                /* Simple: emit raw \uXXXX as-is for non-ASCII; for ASCII just write char */
                if (i + 4 < src_len) {
                    unsigned cp = 0;
                    for (int j = 1; j <= 4; j++) {
                        char c = src[i + j];
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                    }
                    i += 4;
                    if (cp < 0x80) {
                        dest[w++] = (char)cp;
                    } else if (cp < 0x800 && w + 1 < cap) {
                        dest[w++] = (char)(0xC0 | (cp >> 6));
                        dest[w++] = (char)(0x80 | (cp & 0x3F));
                    } else if (w + 2 < cap) {
                        dest[w++] = (char)(0xE0 | (cp >> 12));
                        dest[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        dest[w++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            default: dest[w++] = src[i]; break;
            }
        } else {
            dest[w++] = src[i];
        }
    }
    return w;
}

/* T294: Process one SSE "data:" line using jsmn-based per-provider parsers. */
static void sse_process_line(SseCtx *ctx, const char *line, size_t len) {
    /* Skip "data: " prefix */
    if (len < 5 || memcmp(line, "data:", 5) != 0) return;
    const char *json = line + 5;
    size_t jlen = len - 5;
    while (jlen > 0 && (*json == ' ')) { json++; jlen--; }

    /* [DONE] marker */
    if (jlen >= 6 && memcmp(json, "[DONE]", 6) == 0) return;

    /* Auto-detect provider format and parse */
    SseChunk chunk;
    int rc;
    /* Gemini: has "candidates" at top level */
    if (memchr(json, '{', jlen) && strstr(json, "\"candidates\""))
        rc = sse_parse_gemini(json, jlen, &chunk);
    else
        rc = sse_parse_openai(json, jlen, &chunk);

    if (rc != 0) return;

    /* Apply parsed chunk to SseCtx accumulation state */

    /* Content text */
    if (chunk.text && chunk.text_len > 0) {
        char *unesc = malloc(chunk.text_len + 1);
        if (unesc) {
            size_t ulen = json_unescape(unesc, chunk.text_len + 1, chunk.text, chunk.text_len);
            /* Append to accumulated content */
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
        /* Grow tc arrays if needed */
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
            /* Synthetic ID for Gemini */
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "call_%zu", idx);
            ctx->tc_ids[idx] = strdup(id_buf);
        }

        if (chunk.tc_name && chunk.tc_name_len > 0 && !ctx->tc_names[idx])
            ctx->tc_names[idx] = strndup(chunk.tc_name, chunk.tc_name_len);

        if (chunk.tc_args && chunk.tc_args_len > 0) {
            if (chunk.tc_args_complete) {
                /* Gemini: full args object, store directly */
                free(ctx->tc_args[idx]);
                ctx->tc_args[idx] = strndup(chunk.tc_args, chunk.tc_args_len);
                ctx->tc_arg_lens[idx] = chunk.tc_args_len;
                ctx->tc_arg_caps[idx] = chunk.tc_args_len + 1;
            } else {
                /* OpenAI: fragment, unescape and append */
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
        /* Map Gemini STOP/MAX_TOKENS to standard format */
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
            /* Process accumulated line (strip trailing \r) */
            if (ctx->line_len > 0) {
                if (ctx->line_buf[ctx->line_len - 1] == '\r')
                    ctx->line_len--;
                ctx->line_buf[ctx->line_len] = '\0';
                sse_process_line(ctx, ctx->line_buf, ctx->line_len);
            }
            ctx->line_len = 0;
        } else {
            /* Append to line buffer */
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

int http_post_stream_sse(const char *url, const char **headers,
                         HttpReadFn read_cb, void *read_data,
                         HttpSseFn sse_cb, void *sse_data,
                         HttpResponse *resp) {
    memset(resp, 0, sizeof(*resp));

    SseCtx ctx = {0};
    ctx.resp = resp;
    ctx.sse_cb = sse_cb;
    ctx.sse_data = sse_data;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *hlist = NULL;
    if (headers) {
        for (const char **h = headers; *h; h++)
            hlist = curl_slist_append(hlist, *h);
    }
    hlist = curl_slist_append(hlist, "Transfer-Encoding: chunked");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, read_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    else if (rc == CURLE_OPERATION_TIMEDOUT)
        status = -2;

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);

    /* On success, reconstruct a non-streaming-format JSON response for parsing.
     * This lets the existing llm_parse_response work unchanged. */
    if (status >= 200 && status < 300 && (ctx.content || ctx.tc_count > 0 || ctx.finish_reason)) {
        /* Build synthetic response JSON that llm_parse_response can handle */
        free(resp->data);
        resp->data = NULL;
        resp->len = 0;
        resp->cap = 0;

        size_t need = (ctx.content_len * 2) + (ctx.reasoning_len * 2) + (ctx.tc_count * 512) + 512;
        char *out = malloc(need);
        if (out) {
            int n = snprintf(out, need, "{\"choices\":[{\"message\":{");

            /* Content */
            if (ctx.content && ctx.content_len > 0) {
                n += snprintf(out + n, need - (size_t)n, "\"content\":\"");
                /* Ensure buffer is large enough for escaped content */
                size_t esc_need = ctx.content_len * 6 + 1;
                if ((size_t)n + esc_need + 256 > need) {
                    need = (size_t)n + esc_need + 1024;
                    out = realloc(out, need);
                }
                size_t esc_len = json_escape_into(out + n, need - (size_t)n, ctx.content);
                n += (int)esc_len;
                n += snprintf(out + n, need - (size_t)n, "\"");
            }

            /* Reasoning */
            if (ctx.reasoning && ctx.reasoning_len > 0) {
                if (ctx.content && ctx.content_len > 0)
                    n += snprintf(out + n, need - (size_t)n, ",");
                n += snprintf(out + n, need - (size_t)n, "\"reasoning\":\"");
                size_t esc_need = ctx.reasoning_len * 6 + 1;
                if ((size_t)n + esc_need + 256 > need) {
                    need = (size_t)n + esc_need + 1024;
                    out = realloc(out, need);
                }
                size_t esc_len = json_escape_into(out + n, need - (size_t)n, ctx.reasoning);
                n += (int)esc_len;
                n += snprintf(out + n, need - (size_t)n, "\"");
            }

            /* Tool calls */
            if (ctx.tc_count > 0) {
                if ((ctx.content && ctx.content_len > 0) || (ctx.reasoning && ctx.reasoning_len > 0))
                    n += snprintf(out + n, need - (size_t)n, ",");
                n += snprintf(out + n, need - (size_t)n, "\"tool_calls\":[");
                for (size_t i = 0; i < ctx.tc_count; i++) {
                    if (i > 0) out[n++] = ',';
                    /* Ensure buffer large enough */
                    size_t tc_need = 128 + (ctx.tc_ids[i] ? strlen(ctx.tc_ids[i]) : 0)
                                         + (ctx.tc_names[i] ? strlen(ctx.tc_names[i]) : 0)
                                         + ctx.tc_arg_lens[i] * 2;
                    if ((size_t)n + tc_need > need) {
                        need = (size_t)n + tc_need + 1024;
                        out = realloc(out, need);
                    }
                    n += snprintf(out + n, need - (size_t)n,
                        "{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":\"",
                        ctx.tc_ids[i] ? ctx.tc_ids[i] : "",
                        ctx.tc_names[i] ? ctx.tc_names[i] : "");
                    /* Escape arguments JSON string */
                    if (ctx.tc_args[i] && ctx.tc_arg_lens[i] > 0) {
                        size_t al = json_escape_into(out + n, need - (size_t)n, ctx.tc_args[i]);
                        n += (int)al;
                    }
                    n += snprintf(out + n, need - (size_t)n, "\"}}");
                }
                n += snprintf(out + n, need - (size_t)n, "]");
            }

            n += snprintf(out + n, need - (size_t)n, "},\"finish_reason\":\"%s\"}]",
                          ctx.finish_reason ? ctx.finish_reason : "stop");

            /* Usage */
            if (ctx.total_tokens > 0 || ctx.cost_nano > 0) {
                n += snprintf(out + n, need - (size_t)n,
                    ",\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d",
                    ctx.prompt_tokens, ctx.completion_tokens, ctx.total_tokens);
                if (ctx.cache_read_tokens > 0 || ctx.cache_write_tokens > 0)
                    n += snprintf(out + n, need - (size_t)n,
                        ",\"prompt_tokens_details\":{\"cached_tokens\":%d,\"cache_write_tokens\":%d}",
                        ctx.cache_read_tokens, ctx.cache_write_tokens);
                if (ctx.cost_nano > 0)
                    n += snprintf(out + n, need - (size_t)n,
                        ",\"cost\":%.10f", (double)ctx.cost_nano / 1e9);
                n += snprintf(out + n, need - (size_t)n, "}");
            }
            n += snprintf(out + n, need - (size_t)n, "}");
            resp->data = out;
            resp->len = (size_t)n;
            resp->cap = need;
        }
    }
    /* On error (non-2xx), resp->data has the raw error body — leave as-is */

    free(ctx.line_buf);
    free(ctx.content);
    free(ctx.reasoning);
    free(ctx.finish_reason);
    for (size_t i = 0; i < ctx.tc_alloc; i++) {
        free(ctx.tc_ids[i]);
        free(ctx.tc_names[i]);
        free(ctx.tc_args[i]);
    }
    free(ctx.tc_ids);
    free(ctx.tc_names);
    free(ctx.tc_args);
    free(ctx.tc_arg_lens);
    free(ctx.tc_arg_caps);
    return (int)status;
}
