#define _POSIX_C_SOURCE 200809L
#include "telegram.h"
#include "agent.h"
#include "db.h"
#include "http.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
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

/* Build Telegram API URL: https://api.telegram.org/bot<token>/<method> */
static char *tg_url(const char *token, const char *method) {
    const char *prefix = "https://api.telegram.org/bot";
    size_t len = strlen(prefix) + strlen(token) + 1 + strlen(method) + 1;
    char *url = malloc(len);
    if (!url) return NULL;
    snprintf(url, len, "%s%s/%s", prefix, token, method);
    return url;
}

/* POST JSON to Telegram API, return parsed cJSON response or NULL */
static cJSON *tg_call(const char *token, const char *method, const char *body) {
    char *url = tg_url(token, method);
    if (!url) return NULL;

    const char *headers[] = {"Content-Type: application/json", NULL};
    HttpResponse resp;
    int status = http_post(url, headers, body, &resp);
    free(url);

    if (status < 200 || status >= 300 || !resp.data) {
        http_response_free(&resp);
        return NULL;
    }

    cJSON *json = cJSON_Parse(resp.data);
    http_response_free(&resp);
    return json;
}

/* V11: Find split point within text[0..max_len-1].
 * Priority: paragraph (\n\n), then newline, then sentence (. ! ?), then max_len. */
size_t tg_find_split(const char *text, size_t len, size_t max_len) {
    if (len <= max_len) return len;

    /* Search backwards from max_len for paragraph break */
    for (size_t i = max_len; i > 0; i--) {
        if (text[i] == '\n' && i > 0 && text[i - 1] == '\n')
            return i + 1;
    }
    /* Search backwards for single newline */
    for (size_t i = max_len; i > 0; i--) {
        if (text[i] == '\n')
            return i + 1;
    }
    /* Search backwards for sentence end (. ! ? followed by space or end) */
    for (size_t i = max_len; i > 0; i--) {
        if ((text[i - 1] == '.' || text[i - 1] == '!' || text[i - 1] == '?') &&
            (i == len || text[i] == ' ' || text[i] == '\n'))
            return i;
    }
    /* Hard cut at max_len */
    return max_len;
}

/* V11: Send text chunked at TG_MAX_MSG_LEN, split at paragraph/sentence boundaries */
static void tg_send_chunked(const char *token, int64_t chat_id, const char *text) {
    size_t total = strlen(text);
    size_t offset = 0;

    while (offset < total) {
        size_t remaining = total - offset;
        size_t chunk_len = tg_find_split(text + offset, remaining, TG_MAX_MSG_LEN);

        /* Build sendMessage body with this chunk */
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

/* Typing indicator context */
typedef struct {
    const char *token;
    int64_t chat_id;
    volatile int active;
} TypingCtx;

/* Send typing indicator every 4s until ctx->active is cleared */
static void *typing_loop(void *arg) {
    TypingCtx *ctx = (TypingCtx *)arg;
    while (ctx->active) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddNumberToObject(body, "chat_id", (double)ctx->chat_id);
        cJSON_AddStringToObject(body, "action", "typing");
        char *json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (json) {
            cJSON *resp = tg_call(ctx->token, "sendChatAction", json);
            cJSON_Delete(resp);
            free(json);
        }
        /* Sleep 4s in 200ms increments so we can exit quickly */
        for (int i = 0; i < 20 && ctx->active; i++) {
            struct timespec ts = {0, 200000000}; /* 200ms */
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* Tool dispatch via registry */
static char *tg_dispatch(const char *name, const char *arguments, void *user_data) {
    ToolRegistry *reg = (ToolRegistry *)user_data;
    ToolEntry *e = tools_lookup(reg, name);
    if (!e) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
        return err;
    }
    return e->handler(arguments, e->user_data);
}

/* Process a single Telegram message: route to session, run agent, reply */
static void process_message(cJSON *msg, ToolRegistry *reg, const ToolSchema *schemas, size_t tool_count) {
    cJSON *chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
    cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
    if (!chat || !text || !cJSON_IsString(text)) return;

    cJSON *chat_id_json = cJSON_GetObjectItemCaseSensitive(chat, "id");
    if (!chat_id_json || !cJSON_IsNumber(chat_id_json)) return;
    int64_t chat_id = (int64_t)chat_id_json->valuedouble;

    /* Route chat_id to session (T26 will refine — for now use chat_id as session name) */
    char session_name[64];
    snprintf(session_name, sizeof(session_name), "tg_%lld", (long long)chat_id);

    /* Find or create session for this chat */
    int count = 0;
    Session *sessions = session_list(g_db, &count);
    int64_t session_id = -1;
    if (sessions) {
        for (int i = 0; i < count; i++) {
            if (sessions[i].name && strcmp(sessions[i].name, session_name) == 0) {
                session_id = sessions[i].id;
                break;
            }
        }
        session_list_free(sessions, count);
    }
    if (session_id < 0) {
        session_id = session_create(g_db, session_name);
        if (session_id < 0) return;
        /* Append system message */
        Message sys_msg = {.role = ROLE_SYSTEM, .content = "You are CClaw, a helpful AI assistant."};
        entry_append(g_db, session_id, &sys_msg);
    }

    /* Append user message */
    Message user_msg = {.role = ROLE_USER, .content = text->valuestring};
    entry_append(g_db, session_id, &user_msg);

    /* Start typing indicator thread */
    TypingCtx typing_ctx = {.token = g_cfg->telegram_token, .chat_id = chat_id, .active = 1};
    pthread_t typing_thread;
    pthread_create(&typing_thread, NULL, typing_loop, &typing_ctx);

    /* Run agent */
    AgentContext ctx = {0};
    ctx.db = g_db;
    ctx.session_id = session_id;
    ctx.cfg = g_cfg;
    ctx.dispatch = tg_dispatch;
    ctx.dispatch_data = reg;
    ctx.tools = schemas;
    ctx.tool_count = tool_count;

    int rc = agent_run(&ctx);

    /* Stop typing indicator */
    typing_ctx.active = 0;
    pthread_join(typing_thread, NULL);

    /* Get response text */
    char *reply_text = NULL;
    if (rc == 0) {
        int branch_count = 0;
        Entry *entries = session_get_branch(g_db, session_id, &branch_count);
        if (entries) {
            for (int i = branch_count - 1; i >= 0; i--) {
                if (entries[i].message.role == ROLE_ASSISTANT && entries[i].message.content) {
                    reply_text = strdup(entries[i].message.content);
                    break;
                }
            }
            entry_branch_free(entries, branch_count);
        }
    }
    if (!reply_text) reply_text = strdup("error: agent failed");

    /* V11: Send reply chunked at 4096 chars */
    tg_send_chunked(g_cfg->telegram_token, chat_id, reply_text);
    free(reply_text);
}

static void *poll_loop(void *arg) {
    (void)arg;
    int64_t offset = 0;

    /* T25: Load persisted offset from DB */
    char *saved = db_kv_get(g_db, "tg_offset");
    if (saved) {
        offset = strtoll(saved, NULL, 10);
        free(saved);
    }

    /* Register tools */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg);
    tool_file_read_register(&reg, g_cfg->workspace);
    tool_file_write_register(&reg, g_cfg->workspace);

    size_t tool_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &tool_count);

    while (running) {
        /* Build getUpdates request */
        cJSON *req = cJSON_CreateObject();
        if (offset > 0) cJSON_AddNumberToObject(req, "offset", (double)offset);
        cJSON_AddNumberToObject(req, "timeout", 30);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) { sleep(1); continue; }

        cJSON *resp = tg_call(g_cfg->telegram_token, "getUpdates", body);
        free(body);

        if (!resp) { sleep(5); continue; }

        cJSON *ok = cJSON_GetObjectItemCaseSensitive(resp, "ok");
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        if (!cJSON_IsTrue(ok) || !cJSON_IsArray(result)) {
            cJSON_Delete(resp);
            sleep(5);
            continue;
        }

        cJSON *update;
        cJSON_ArrayForEach(update, result) {
            if (!running) break;

            cJSON *uid = cJSON_GetObjectItemCaseSensitive(update, "update_id");
            if (cJSON_IsNumber(uid)) {
                int64_t id = (int64_t)uid->valuedouble;
                if (id >= offset) offset = id + 1;
            }

            cJSON *msg = cJSON_GetObjectItemCaseSensitive(update, "message");
            if (msg) process_message(msg, &reg, schemas, tool_count);
        }

        /* T25: Persist offset so we don't re-process on restart */
        if (offset > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)offset);
            db_kv_set(g_db, "tg_offset", buf);
        }

        cJSON_Delete(resp);
    }

    tools_free(&reg);
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
