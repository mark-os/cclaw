#define _POSIX_C_SOURCE 200809L
#include "gemini_cache.h"
#include "db.h"
#include "http.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* T291: Minimum tokens to trigger caching */
#define GEMINI_CACHE_MIN_TOKENS 1024
/* T291: Cache TTL in seconds */
#define GEMINI_CACHE_TTL 300

/* Build kv key for cache name: gemini_cache_<session_id> */
static void cache_kv_key(int64_t session_id, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "gemini_cache_%lld", (long long)session_id);
}

/* Build kv key for cache expiry: gemini_cache_exp_<session_id> */
static void cache_exp_key(int64_t session_id, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "gemini_cache_exp_%lld", (long long)session_id);
}

/* Build cachedContents API URL from provider base_url.
 * base_url for gemini native is like https://generativelanguage.googleapis.com/v1beta
 * We need: {base_url_without_trailing}/cachedContents */
static char *build_cache_url(const Config *cfg) {
    const char *base = cfg->provider.base_url;
    if (!base) return NULL;
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') blen--;
    /* Strip /openai suffix if present (compat endpoint) */
    if (blen > 7 && memcmp(base + blen - 7, "/openai", 7) == 0) blen -= 7;
    /* /cachedContents */
    size_t need = blen + 16 + 1;
    char *url = malloc(need);
    if (!url) return NULL;
    snprintf(url, need, "%.*s/cachedContents", (int)blen, base);
    return url;
}

/* Build request body for creating cached content.
 * Includes: model, systemInstruction (from plan system entries), contents (from plan),
 * ttl. */
static char *build_cache_request(sqlite3 *db, int64_t session_id,
                                 const Config *cfg, const ContextPlan *plan) {
    const char *model = cfg->provider.model ? cfg->provider.model : "gemini-2.5-flash";

    cJSON *root = cJSON_CreateObject();
    /* model field: "models/<model>" */
    char model_field[128];
    snprintf(model_field, sizeof(model_field), "models/%s", model);
    cJSON_AddStringToObject(root, "model", model_field);

    /* TTL */
    char ttl_str[32];
    snprintf(ttl_str, sizeof(ttl_str), "%ds", GEMINI_CACHE_TTL);
    cJSON_AddStringToObject(root, "ttl", ttl_str);

    /* Gather system instruction and contents from DB */
    const char *sql = "SELECT role, content, tool_calls, tool_call_id FROM entries WHERE id=? AND session_id=?;";
    sqlite3_stmt *cursor;
    if (sqlite3_prepare_v2(db, sql, -1, &cursor, NULL) != SQLITE_OK) {
        cJSON_Delete(root);
        return NULL;
    }

    /* System instruction: concatenate system message contents */
    cJSON *sys_parts = cJSON_CreateArray();
    for (int i = plan->cut; i < plan->count; i++) {
        if (plan->entries[i].role != ROLE_SYSTEM) continue;
        sqlite3_reset(cursor);
        sqlite3_bind_int64(cursor, 1, plan->entries[i].id);
        sqlite3_bind_int64(cursor, 2, session_id);
        if (sqlite3_step(cursor) != SQLITE_ROW) continue;
        const char *c = (const char *)sqlite3_column_text(cursor, 1);
        if (c && c[0]) {
            cJSON *part = cJSON_CreateObject();
            cJSON_AddStringToObject(part, "text", c);
            cJSON_AddItemToArray(sys_parts, part);
        }
    }
    if (cJSON_GetArraySize(sys_parts) > 0) {
        cJSON *sys_instr = cJSON_CreateObject();
        cJSON_AddItemToObject(sys_instr, "parts", sys_parts);
        cJSON_AddItemToObject(root, "systemInstruction", sys_instr);
    } else {
        cJSON_Delete(sys_parts);
    }

    /* Contents: all non-system entries */
    cJSON *contents = cJSON_CreateArray();
    for (int i = plan->cut; i < plan->count; i++) {
        if (plan->entries[i].role == ROLE_SYSTEM) continue;
        sqlite3_reset(cursor);
        sqlite3_bind_int64(cursor, 1, plan->entries[i].id);
        sqlite3_bind_int64(cursor, 2, session_id);
        if (sqlite3_step(cursor) != SQLITE_ROW) continue;

        int role = sqlite3_column_int(cursor, 0);
        const char *content = (const char *)sqlite3_column_text(cursor, 1);
        const char *tool_calls = (const char *)sqlite3_column_text(cursor, 2);
        const char *tool_call_id = (const char *)sqlite3_column_text(cursor, 3);

        cJSON *entry_obj = cJSON_CreateObject();
        cJSON *parts = cJSON_CreateArray();

        if (role == 3) {
            /* Tool result → user role with functionResponse */
            cJSON_AddStringToObject(entry_obj, "role", "user");
            cJSON *fr = cJSON_CreateObject();
            cJSON_AddStringToObject(fr, "name", tool_call_id ? tool_call_id : "");
            cJSON *resp_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(resp_obj, "content", content ? content : "");
            cJSON_AddItemToObject(fr, "response", resp_obj);
            cJSON *part = cJSON_CreateObject();
            cJSON_AddItemToObject(part, "functionResponse", fr);
            cJSON_AddItemToArray(parts, part);
        } else if (role == 2) {
            /* Assistant → model */
            cJSON_AddStringToObject(entry_obj, "role", "model");
            if (content && content[0]) {
                cJSON *part = cJSON_CreateObject();
                cJSON_AddStringToObject(part, "text", content);
                cJSON_AddItemToArray(parts, part);
            }
            /* tool_calls → functionCall parts */
            if (tool_calls && tool_calls[0]) {
                cJSON *tc_arr = cJSON_Parse(tool_calls);
                if (tc_arr && cJSON_IsArray(tc_arr)) {
                    int n = cJSON_GetArraySize(tc_arr);
                    for (int j = 0; j < n; j++) {
                        cJSON *tc = cJSON_GetArrayItem(tc_arr, j);
                        cJSON *name = cJSON_GetObjectItem(tc, "name");
                        cJSON *args = cJSON_GetObjectItem(tc, "args");
                        cJSON *part = cJSON_CreateObject();
                        cJSON *fc = cJSON_CreateObject();
                        cJSON_AddStringToObject(fc, "name",
                                                name && cJSON_IsString(name) ? name->valuestring : "");
                        if (args)
                            cJSON_AddItemToObject(fc, "args", cJSON_Duplicate(args, 1));
                        else
                            cJSON_AddItemToObject(fc, "args", cJSON_CreateObject());
                        cJSON_AddItemToObject(part, "functionCall", fc);
                        cJSON_AddItemToArray(parts, part);
                    }
                }
                cJSON_Delete(tc_arr);
            }
        } else {
            /* User */
            cJSON_AddStringToObject(entry_obj, "role", "user");
            cJSON *part = cJSON_CreateObject();
            cJSON_AddStringToObject(part, "text", content ? content : "");
            cJSON_AddItemToArray(parts, part);
        }

        cJSON_AddItemToObject(entry_obj, "parts", parts);
        cJSON_AddItemToArray(contents, entry_obj);
    }

    cJSON_AddItemToObject(root, "contents", contents);
    sqlite3_finalize(cursor);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* POST to cachedContents, parse response for "name" field.
 * Returns heap-allocated cache name or NULL on failure. */
static char *create_cache_api(const Config *cfg, const char *body) {
    char *url = build_cache_url(cfg);
    if (!url) return NULL;

    /* Auth header */
    size_t key_len = cfg->provider.api_key ? strlen(cfg->provider.api_key) : 0;
    char *auth_hdr = malloc(16 + key_len + 1);
    if (!auth_hdr) { free(url); return NULL; }
    sprintf(auth_hdr, "x-goog-api-key: %s", cfg->provider.api_key ? cfg->provider.api_key : "");

    const char *headers[] = { "Content-Type: application/json", auth_hdr, NULL };
    HttpResponse resp = {0};
    int status = http_post(url, headers, body, &resp);
    free(url);
    free(auth_hdr);

    if (status < 200 || status >= 300 || !resp.data) {
        http_response_free(&resp);
        return NULL;
    }

    /* Parse response for "name" field */
    cJSON *root = cJSON_Parse(resp.data);
    http_response_free(&resp);
    if (!root) return NULL;

    cJSON *name = cJSON_GetObjectItem(root, "name");
    char *result = NULL;
    if (name && cJSON_IsString(name) && name->valuestring[0])
        result = strdup(name->valuestring);
    cJSON_Delete(root);
    return result;
}

/* DELETE a cached content by name */
static void delete_cache_api(const Config *cfg, const char *cache_name) {
    /* URL: {base}/cachedContents/{id} or just the full name path */
    const char *base = cfg->provider.base_url;
    if (!base) return;
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') blen--;
    if (blen > 7 && memcmp(base + blen - 7, "/openai", 7) == 0) blen -= 7;

    /* cache_name is like "cachedContents/abc123" — append to base */
    size_t nlen = strlen(cache_name);
    size_t need = blen + 1 + nlen + 1;
    char *url = malloc(need);
    if (!url) return;
    snprintf(url, need, "%.*s/%s", (int)blen, base, cache_name);

    size_t key_len = cfg->provider.api_key ? strlen(cfg->provider.api_key) : 0;
    char *auth_hdr = malloc(16 + key_len + 1);
    if (!auth_hdr) { free(url); return; }
    sprintf(auth_hdr, "x-goog-api-key: %s", cfg->provider.api_key ? cfg->provider.api_key : "");

    /* Use libcurl directly for DELETE */
    CURL *curl = curl_easy_init();
    if (curl) {
        struct curl_slist *hlist = NULL;
        hlist = curl_slist_append(hlist, auth_hdr);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_perform(curl);
        curl_slist_free_all(hlist);
        curl_easy_cleanup(curl);
    }
    free(url);
    free(auth_hdr);
}

char *gemini_cache_get_or_create(sqlite3 *db, int64_t session_id,
                                 const Config *cfg, const ContextPlan *plan) {
    if (!db || !cfg || !plan) return NULL;
    if (cfg->provider.endpoint_type != ENDPOINT_GEMINI) return NULL;

    /* Sum token estimates for session to check threshold */
    int total_tokens = 0;
    for (int i = plan->cut; i < plan->count; i++)
        total_tokens += plan->entries[i].token_estimate;
    if (total_tokens < GEMINI_CACHE_MIN_TOKENS) return NULL;

    /* Check for existing valid cache in kv */
    char key[64], exp_key[64];
    cache_kv_key(session_id, key, sizeof(key));
    cache_exp_key(session_id, exp_key, sizeof(exp_key));

    char *cached_name = db_kv_get(db, key);
    char *exp_str = db_kv_get(db, exp_key);
    if (cached_name && exp_str) {
        time_t exp_time = (time_t)atoll(exp_str);
        time_t now = time(NULL);
        if (now < exp_time) {
            /* Cache still valid */
            free(exp_str);
            return cached_name; /* caller frees */
        }
        /* Expired — delete old and create new */
        delete_cache_api(cfg, cached_name);
    }
    free(cached_name);
    free(exp_str);

    /* Create new cache */
    char *body = build_cache_request(db, session_id, cfg, plan);
    if (!body) return NULL;

    char *name = create_cache_api(cfg, body);
    free(body);
    if (!name) return NULL;

    /* Store in kv */
    db_kv_set(db, key, name);
    char exp_buf[32];
    snprintf(exp_buf, sizeof(exp_buf), "%lld", (long long)(time(NULL) + GEMINI_CACHE_TTL));
    db_kv_set(db, exp_key, exp_buf);

    return name;
}

void gemini_cache_delete(sqlite3 *db, int64_t session_id, const Config *cfg) {
    if (!db || !cfg) return;

    char key[64];
    cache_kv_key(session_id, key, sizeof(key));
    char *cached_name = db_kv_get(db, key);
    if (!cached_name) return;

    delete_cache_api(cfg, cached_name);
    free(cached_name);

    /* Remove kv entries */
    const char *del_sql = "DELETE FROM kv WHERE key=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, del_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    char exp_key[64];
    cache_exp_key(session_id, exp_key, sizeof(exp_key));
    if (sqlite3_prepare_v2(db, del_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, exp_key, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}
