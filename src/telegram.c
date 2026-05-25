#define _POSIX_C_SOURCE 200809L
#include "telegram.h"
#include "config.h"
#include "db.h"
#include "daemon.h"
#include "http.h"
#include "agent_config.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define TG_MAX_MSG_LEN 4096

static pthread_t poll_thread;
static volatile int running;
static const Config *g_cfg;
static sqlite3 *g_db;

/* Build Telegram API URL: <base>/bot<token>/<method> */
static char *tg_url(const char *token, const char *method) {
    const char *base = g_cfg && g_cfg->telegram_base_url ? g_cfg->telegram_base_url : "https://api.telegram.org";
    size_t len = strlen(base) + 4 + strlen(token) + 1 + strlen(method) + 1;
    char *url = malloc(len);
    if (!url) return NULL;
    snprintf(url, len, "%s/bot%s/%s", base, token, method);
    return url;
}

/* POST JSON to Telegram API, return parsed cJSON response or NULL. */
static cJSON *tg_call_ex(const char *token, const char *method, const char *body, int *out_status) {
    char *url = tg_url(token, method);
    if (!url) { if (out_status) *out_status = -1; return NULL; }

    const char *headers[] = {"Content-Type: application/json", NULL};
    HttpResponse resp;
    int status = http_post(url, headers, body, &resp);
    free(url);

    if (out_status) *out_status = status;

    if (status < 200 || status >= 300 || !resp.data) {
        http_response_free(&resp);
        return NULL;
    }

    cJSON *json = cJSON_Parse(resp.data);
    http_response_free(&resp);
    return json;
}

static cJSON *tg_call(const char *token, const char *method, const char *body) {
    return tg_call_ex(token, method, body, NULL);
}

/* V2: Compute backoff sleep in seconds. Doubles each failure, capped at 60s. */
int tg_backoff_delay(int consecutive_failures) {
    int delay = 1;
    for (int i = 0; i < consecutive_failures - 1 && delay < 60; i++)
        delay *= 2;
    return delay < 60 ? delay : 60;
}

/* V11: Find split point within text[0..max_len-1]. */
size_t tg_find_split(const char *text, size_t len, size_t max_len) {
    if (len <= max_len) return len;

    for (size_t i = max_len; i > 0; i--) {
        if (text[i] == '\n' && i > 0 && text[i - 1] == '\n')
            return i + 1;
    }
    for (size_t i = max_len; i > 0; i--) {
        if (text[i] == '\n')
            return i + 1;
    }
    for (size_t i = max_len; i > 0; i--) {
        if ((text[i - 1] == '.' || text[i - 1] == '!' || text[i - 1] == '?') &&
            (i == len || text[i] == ' ' || text[i] == '\n'))
            return i;
    }
    return max_len;
}

/* V11: Send text chunked at TG_MAX_MSG_LEN */
static void tg_send_chunked(const char *token, int64_t chat_id, const char *text) {
    size_t total = strlen(text);
    size_t offset = 0;

    while (offset < total) {
        size_t remaining = total - offset;
        size_t chunk_len = tg_find_split(text + offset, remaining, TG_MAX_MSG_LEN);

        cJSON *body = cJSON_CreateObject();
        cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);

        char *chunk = malloc(chunk_len + 1);
        if (!chunk) break;
        memcpy(chunk, text + offset, chunk_len);
        chunk[chunk_len] = '\0';
        cJSON_AddStringToObject(body, "text", chunk);
        free(chunk);

        char *json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (json) {
            cJSON *resp = tg_call(token, "sendMessage", json);
            cJSON_Delete(resp);
            free(json);
        }

        offset += chunk_len;
    }
}

/* Process a single Telegram message: route to session, inbox, signal daemon */
static void process_message(cJSON *msg) {
    cJSON *chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
    cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
    if (!chat || !text || !cJSON_IsString(text)) return;

    cJSON *chat_id_json = cJSON_GetObjectItemCaseSensitive(chat, "id");
    if (!chat_id_json || !cJSON_IsNumber(chat_id_json)) return;
    int64_t chat_id = (int64_t)chat_id_json->valuedouble;

    /* Route chat_id to session */
    int64_t session_id = db_tg_get_session(g_db, chat_id);
    if (session_id < 0) {
        char session_name[64];
        snprintf(session_name, sizeof(session_name), "tg_%lld", (long long)chat_id);
        session_id = session_create(g_db, session_name, NULL, -1, 0);
        if (session_id < 0) return;
        db_tg_set_session(g_db, chat_id, session_id);
        /* Append system message for new session */
        char *prompt = agent_build_system_prompt(g_db, NULL, session_id,
                                                "agents", g_cfg);
        Message sys_msg = {.role = ROLE_SYSTEM, .content = prompt};
        entry_append(g_db, session_id, &sys_msg);
        free(prompt);
    }

    /* Write to inbox and signal daemon */
    inbox_insert(g_db, session_id, "telegram", text->valuestring);
    daemon_signal_session(session_id);
}

static void *poll_loop(void *arg) {
    (void)arg;
    int64_t offset = 0;
    int failures = 0;

    /* Load persisted offset */
    char *saved = db_kv_get(g_db, "tg_offset");
    if (saved) {
        offset = strtoll(saved, NULL, 10);
        free(saved);
    }

    while (running) {
        cJSON *req = cJSON_CreateObject();
        if (offset > 0) cJSON_AddNumberToObject(req, "offset", (double)offset);
        cJSON_AddNumberToObject(req, "timeout", 30);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) { sleep(1); continue; }

        int http_status = 0;
        cJSON *resp = tg_call_ex(g_cfg->telegram_token, "getUpdates", body, &http_status);
        free(body);

        if (!resp) {
            failures++;
            int delay = tg_backoff_delay(failures);
            for (int i = 0; i < delay && running; i++)
                sleep(1);
            continue;
        }

        cJSON *ok = cJSON_GetObjectItemCaseSensitive(resp, "ok");
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        if (!cJSON_IsTrue(ok) || !cJSON_IsArray(result)) {
            cJSON_Delete(resp);
            failures++;
            int delay = tg_backoff_delay(failures);
            for (int i = 0; i < delay && running; i++)
                sleep(1);
            continue;
        }

        failures = 0;

        cJSON *update;
        cJSON_ArrayForEach(update, result) {
            if (!running) break;

            cJSON *uid = cJSON_GetObjectItemCaseSensitive(update, "update_id");
            if (cJSON_IsNumber(uid)) {
                int64_t id = (int64_t)uid->valuedouble;
                if (id >= offset) offset = id + 1;
            }

            cJSON *msg = cJSON_GetObjectItemCaseSensitive(update, "message");
            if (msg) process_message(msg);
        }

        if (offset > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)offset);
            db_kv_set(g_db, "tg_offset", buf);
        }

        cJSON_Delete(resp);
    }

    return NULL;
}

int telegram_start(const Config *cfg, sqlite3 *db) {
    if (!cfg->telegram_token || cfg->telegram_token[0] == '\0') return -1;

    g_cfg = cfg;
    g_db = db;
    running = 1;

    if (pthread_create(&poll_thread, NULL, poll_loop, NULL) != 0) {
        running = 0;
        return -1;
    }
    return 0;
}

void telegram_stop(void) {
    running = 0;
    pthread_join(poll_thread, NULL);
}

/* Public send API for daemon response delivery */
void telegram_send_message(const char *token, int64_t chat_id, const char *text) {
    if (!token || !text) return;
    tg_send_chunked(token, chat_id, text);
}
