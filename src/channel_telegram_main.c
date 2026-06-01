/* T247/T248/T249: Standalone Telegram channel process.
 * Uses channel API (channel_emit for incoming, channel_next_outbox for delivery).
 * Launched by daemon via channels table. Receives: argv[1]=db_path, argv[2]=channel_name.
 */
#define _POSIX_C_SOURCE 200809L
#include "channel_api.h"
#include "http.h"
#include "cJSON.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define TG_MAX_MSG_LEN 4096
#define OUTBOX_POLL_MS 500

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* ── Telegram API helpers ──────────────────────────────────────── */

static char *tg_url(const char *base, const char *token, const char *method) {
    size_t len = strlen(base) + 5 + strlen(token) + 1 + strlen(method) + 1;
    char *url = malloc(len);
    if (!url) return NULL;
    snprintf(url, len, "%s/bot%s/%s", base, token, method);
    return url;
}

static cJSON *tg_call(const char *base, const char *token,
                       const char *method, const char *body, int *out_status) {
    char *url = tg_url(base, token, method);
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

/* V2: Exponential backoff capped at 60s */
static int backoff_delay(int failures) {
    int d = 1;
    for (int i = 0; i < failures - 1 && d < 60; i++) d *= 2;
    return d < 60 ? d : 60;
}

/* V11: Find split point for chunked sending */
static size_t find_split(const char *text, size_t len, size_t max_len) {
    if (len <= max_len) return len;
    for (size_t i = max_len; i > 0; i--)
        if (text[i] == '\n' && i > 0 && text[i - 1] == '\n') return i + 1;
    for (size_t i = max_len; i > 0; i--)
        if (text[i] == '\n') return i + 1;
    for (size_t i = max_len; i > 0; i--)
        if ((text[i-1] == '.' || text[i-1] == '!' || text[i-1] == '?') &&
            (i == len || text[i] == ' ' || text[i] == '\n')) return i;
    return max_len;
}

/* V11: Send text chunked at TG_MAX_MSG_LEN, sleep 3s between chunks */
static void tg_send_chunked(const char *base, const char *token,
                             int64_t chat_id, const char *text) {
    size_t total = strlen(text);
    size_t offset = 0;
    int first = 1;

    while (offset < total && running) {
        if (!first) sleep(3); /* V11: ≥3s between chunks */
        first = 0;

        size_t remaining = total - offset;
        size_t chunk_len = find_split(text + offset, remaining, TG_MAX_MSG_LEN);

        cJSON *body = cJSON_CreateObject();
        cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
        char *chunk = malloc(chunk_len + 1);
        if (!chunk) { cJSON_Delete(body); break; }
        memcpy(chunk, text + offset, chunk_len);
        chunk[chunk_len] = '\0';
        cJSON_AddStringToObject(body, "text", chunk);
        free(chunk);

        char *json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (json) {
            cJSON *r = tg_call(base, token, "sendMessage", json, NULL);
            cJSON_Delete(r);
            free(json);
        }
        offset += chunk_len;
    }
}

/* ── T248: getUpdates poll loop ────────────────────────────────── */

static void poll_updates(ChannelCtx *ctx, const char *base, const char *token) {
    /* Load offset from channel_state */
    char *saved = channel_get_config(ctx, "tg_offset");
    int64_t offset = saved ? strtoll(saved, NULL, 10) : 0;
    free(saved);

    int failures = 0;

    while (running) {
        cJSON *req = cJSON_CreateObject();
        if (offset > 0) cJSON_AddNumberToObject(req, "offset", (double)offset);
        cJSON_AddNumberToObject(req, "timeout", 30);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) { sleep(1); continue; }

        int http_status = 0;
        cJSON *resp = tg_call(base, token, "getUpdates", body, &http_status);
        free(body);

        if (!resp) {
            failures++;
            int delay = backoff_delay(failures);
            for (int i = 0; i < delay && running; i++) sleep(1);
            continue;
        }

        cJSON *ok = cJSON_GetObjectItemCaseSensitive(resp, "ok");
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        if (!cJSON_IsTrue(ok) || !cJSON_IsArray(result)) {
            cJSON_Delete(resp);
            failures++;
            int delay = backoff_delay(failures);
            for (int i = 0; i < delay && running; i++) sleep(1);
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
            if (!msg) continue;

            cJSON *chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
            if (!chat || !text || !cJSON_IsString(text)) continue;

            cJSON *chat_id_j = cJSON_GetObjectItemCaseSensitive(chat, "id");
            if (!chat_id_j || !cJSON_IsNumber(chat_id_j)) continue;

            /* Build payload: {channel_id, text, from} */
            char channel_id[32];
            snprintf(channel_id, sizeof(channel_id), "%lld",
                     (long long)(int64_t)chat_id_j->valuedouble);

            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "channel_id", channel_id);
            cJSON_AddStringToObject(payload, "text", text->valuestring);

            cJSON *from = cJSON_GetObjectItemCaseSensitive(msg, "from");
            if (from) {
                cJSON *fname = cJSON_GetObjectItemCaseSensitive(from, "first_name");
                if (cJSON_IsString(fname))
                    cJSON_AddStringToObject(payload, "from", fname->valuestring);
            }

            char *pstr = cJSON_PrintUnformatted(payload);
            cJSON_Delete(payload);
            if (pstr) {
                channel_emit(ctx, "message", pstr);
                free(pstr);
            }
        }

        /* Persist offset */
        if (offset > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)offset);
            channel_set_config(ctx, "tg_offset", buf);
        }

        cJSON_Delete(resp);

        /* T249: Drain outbox between polls */
        ChannelOutboxRow *row;
        while (running && (row = channel_next_outbox(ctx)) != NULL) {
            /* Parse payload to get chat_id + text */
            cJSON *pj = cJSON_Parse(row->payload);
            if (pj) {
                cJSON *cid = cJSON_GetObjectItem(pj, "chat_id");
                cJSON *txt = cJSON_GetObjectItem(pj, "text");
                if (cJSON_IsNumber(cid) && cJSON_IsString(txt)) {
                    int64_t chat_id = (int64_t)cid->valuedouble;
                    tg_send_chunked(base, token, chat_id, txt->valuestring);
                    channel_ack_outbox(ctx, row->id);
                } else if (cJSON_IsString(cid) && cJSON_IsString(txt)) {
                    int64_t chat_id = strtoll(cid->valuestring, NULL, 10);
                    tg_send_chunked(base, token, chat_id, txt->valuestring);
                    channel_ack_outbox(ctx, row->id);
                } else {
                    channel_fail_outbox(ctx, row->id, "invalid payload format");
                }
                cJSON_Delete(pj);
            } else {
                channel_fail_outbox(ctx, row->id, "JSON parse error");
            }
            channel_outbox_row_free(row);
        }
    }
}

/* ── main ──────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: channel_telegram <db_path> <channel_name>\n");
        return 1;
    }

    const char *db_path = argv[1];
    const char *channel_name = argv[2];

    /* T250: Graceful shutdown on SIGTERM */
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    ChannelCtx *ctx = channel_ctx_open(db_path, channel_name);
    if (!ctx) {
        fprintf(stderr, "[channel_telegram] failed to open DB: %s\n", db_path);
        return 1;
    }

    /* Read bot token from channel_state */
    char *token = channel_get_config(ctx, "bot_token");
    if (!token || token[0] == '\0') {
        fprintf(stderr, "[channel_telegram] no bot_token in channel_state\n");
        free(token);
        channel_ctx_free(ctx);
        return 1;
    }

    /* Optional: custom base URL (for testing) */
    char *base = channel_get_config(ctx, "base_url");
    if (!base || base[0] == '\0') {
        free(base);
        base = strdup("https://api.telegram.org");
    }

    fprintf(stderr, "[channel_telegram] started (channel=%s)\n", channel_name);

    poll_updates(ctx, base, token);

    fprintf(stderr, "[channel_telegram] shutting down\n");
    free(token);
    free(base);
    channel_ctx_free(ctx);
    return 0;
}
