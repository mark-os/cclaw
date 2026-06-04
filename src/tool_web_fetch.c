#define _POSIX_C_SOURCE 200809L
#include "tool_web_fetch.h"
#include "http.h"
#include "http_policy.h"
#include <cJSON.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define WEB_FETCH_MAX (512 * 1024)

static const char *WEB_FETCH_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"url\":{\"type\":\"string\",\"description\":\"URL to fetch (HTTP GET)\"}"
    "},\"required\":[\"url\"]}";

/* V15: boundary markers */
static const char *BOUNDARY_OPEN = "<tool_result name=\"web_fetch\">";
static const char *BOUNDARY_CLOSE = "</tool_result>";

/* curl write callback */
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    HttpResponse *resp = userdata;
    if (resp->len + bytes + 1 > resp->cap) {
        size_t new_cap = (resp->cap == 0) ? 4096 : resp->cap;
        while (new_cap < resp->len + bytes + 1)
            new_cap *= 2;
        if (new_cap > WEB_FETCH_MAX) new_cap = WEB_FETCH_MAX;
        if (resp->len + bytes + 1 > new_cap) return 0;
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

/* HTTP GET with libcurl */
static int http_get(const char *url, HttpResponse *resp) {
    memset(resp, 0, sizeof(*resp));
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cclaw/1.0");

    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(curl);
    long status = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    /* Stash curl error for caller */
    if (rc != CURLE_OK && resp->err_detail[0] == '\0') {
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        snprintf(resp->err_detail, sizeof(resp->err_detail), "%s", msg);
    }

    curl_easy_cleanup(curl);
    return (rc == CURLE_OK) ? (int)status : -1;
}

size_t html_strip_tags(const char *src, char *dst, size_t dst_cap) {
    size_t out = 0;
    int in_tag = 0;
    int in_script = 0;
    int in_style = 0;
    int prev_space = 0;

    for (const char *p = src; *p && out < dst_cap - 1; p++) {
        if (*p == '<') {
            /* Detect <script> and <style> */
            if (strncasecmp(p, "<script", 7) == 0) { in_script = 1; in_tag = 1; continue; }
            if (strncasecmp(p, "<style", 6) == 0) { in_style = 1; in_tag = 1; continue; }
            if (strncasecmp(p, "</script", 8) == 0) { in_script = 0; in_tag = 1; continue; }
            if (strncasecmp(p, "</style", 7) == 0) { in_style = 0; in_tag = 1; continue; }
            in_tag = 1;
            /* Block-level tags → newline */
            if (p[1] == '/' || p[1] == 'p' || p[1] == 'P' ||
                p[1] == 'd' || p[1] == 'D' || p[1] == 'h' || p[1] == 'H' ||
                p[1] == 'l' || p[1] == 'L' || p[1] == 'b' || p[1] == 'B' ||
                p[1] == 't' || p[1] == 'T') {
                if (out > 0 && dst[out - 1] != '\n') {
                    dst[out++] = '\n';
                    prev_space = 1;
                }
            }
            continue;
        }
        if (*p == '>') { in_tag = 0; continue; }
        if (in_tag || in_script || in_style) continue;

        /* Collapse whitespace */
        if (isspace((unsigned char)*p)) {
            if (!prev_space && out > 0) {
                dst[out++] = ' ';
                prev_space = 1;
            }
            continue;
        }
        prev_space = 0;
        dst[out++] = *p;
    }
    dst[out] = '\0';
    return out;
}

void sanitize_homoglyphs(char *text) {
    /* Replace < and > that could form fake boundary markers with safe equivalents.
     * Specifically target sequences that look like our boundary tags. */
    const char *patterns[] = {"<tool_result", "</tool_result", "<TOOL_RESULT", "</TOOL_RESULT"};
    size_t npatterns = sizeof(patterns) / sizeof(patterns[0]);

    for (char *p = text; *p; p++) {
        if (*p != '<') continue;
        for (size_t i = 0; i < npatterns; i++) {
            size_t plen = strlen(patterns[i]);
            if (strncasecmp(p, patterns[i], plen) == 0) {
                /* Replace '<' with safe Unicode lookalike (＜ = 0xEF 0xBC 0x9C) would
                 * expand string. Simpler: replace with bracket. */
                *p = '[';
                break;
            }
        }
    }
}

char *tool_web_fetch_handler(const char *arguments, void *user_data) {
    HttpPolicy *policy = (HttpPolicy *)user_data;

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *url_item = cJSON_GetObjectItemCaseSensitive(json, "url");
    if (!cJSON_IsString(url_item) || !url_item->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: missing or empty 'url' field");
    }
    const char *url = url_item->valuestring;

    /* V46: check policy before connecting */
    char err[256];
    if (http_check_policy(url, policy, err, sizeof(err)) != 0) {
        cJSON_Delete(json);
        char *msg = malloc(strlen(err) + 8);
        if (msg) { sprintf(msg, "error: %s", err); return msg; }
        return strdup("error: policy denied");
    }

    /* Basic URL validation (fallback if no policy) */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        cJSON_Delete(json);
        return strdup("error: url must start with http:// or https://");
    }

    HttpResponse resp = {0};
    int status = http_get(url, &resp);
    cJSON_Delete(json);

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
        char *err = malloc(64);
        if (err) snprintf(err, 64, "error: HTTP %d", status);
        http_response_free(&resp);
        return err ? err : strdup("error: HTTP error");
    }

    /* Strip HTML tags */
    size_t text_cap = resp.len + 1;
    char *text = malloc(text_cap);
    if (!text) {
        http_response_free(&resp);
        return strdup("error: out of memory");
    }

    html_strip_tags(resp.data, text, text_cap);
    http_response_free(&resp);

    /* V15: sanitize homoglyphs in extracted text */
    sanitize_homoglyphs(text);

    /* V15: wrap in boundary markers */
    size_t text_len = strlen(text);
    size_t open_len = strlen(BOUNDARY_OPEN);
    size_t close_len = strlen(BOUNDARY_CLOSE);
    size_t result_len = open_len + 1 + text_len + 1 + close_len;
    char *result = malloc(result_len + 1);
    if (!result) {
        free(text);
        return strdup("error: out of memory");
    }

    memcpy(result, BOUNDARY_OPEN, open_len);
    result[open_len] = '\n';
    memcpy(result + open_len + 1, text, text_len);
    result[open_len + 1 + text_len] = '\n';
    memcpy(result + open_len + 1 + text_len + 1, BOUNDARY_CLOSE, close_len);
    result[result_len] = '\0';

    free(text);
    return result;
}

int tool_web_fetch_register(ToolRegistry *reg, HttpPolicy *policy) {
    return tools_register(reg, "web_fetch",
                          "Fetch a URL via HTTP GET, extract text from HTML",
                          WEB_FETCH_PARAMS_JSON, tool_web_fetch_handler, policy);
}
