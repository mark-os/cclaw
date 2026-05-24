#define _POSIX_C_SOURCE 200809L
#include "telegram.h"
#include "agent.h"
#include "db.h"
#include "http.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_cron.h"
#include "tool_subagent.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
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

/* POST JSON to Telegram API, return parsed cJSON response or NULL.
 * If out_status is non-NULL, writes HTTP status code (or -1 on curl error). */
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

/* V16: Keep-alive thread for Telegram sessions */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const char *lock_holder;
    volatile int stop;
} TgKeepAliveCtx;

static void *tg_keepalive_thread(void *arg) {
    TgKeepAliveCtx *ctx = (TgKeepAliveCtx *)arg;
    while (!ctx->stop) {
        for (int i = 0; i < 60 && !ctx->stop; i++)
            sleep(1);
        if (!ctx->stop)
            session_refresh_lock(ctx->db, ctx->session_id, ctx->lock_holder);
    }
    return NULL;
}

/* T68: Process a single Telegram message via inbox + CAS lock pattern */
static void process_message(cJSON *msg, ToolRegistry *base_reg, const ToolSchema *schemas, size_t tool_count) {
    (void)base_reg;
    (void)schemas;
    (void)tool_count;
    cJSON *chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
    cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
    if (!chat || !text || !cJSON_IsString(text)) return;

    cJSON *chat_id_json = cJSON_GetObjectItemCaseSensitive(chat, "id");
    if (!chat_id_json || !cJSON_IsNumber(chat_id_json)) return;
    int64_t chat_id = (int64_t)chat_id_json->valuedouble;

    /* T26: Route chat_id to session via dedicated mapping table */
    int64_t session_id = db_tg_get_session(g_db, chat_id);
    if (session_id < 0) {
        char session_name[64];
        snprintf(session_name, sizeof(session_name), "tg_%lld", (long long)chat_id);
        session_id = session_create(g_db, session_name, NULL);
        if (session_id < 0) return;
        db_tg_set_session(g_db, chat_id, session_id);
        /* Append system message */
        char *prompt = config_render_system_prompt(g_cfg, session_id);
        Message sys_msg = {.role = ROLE_SYSTEM, .content = prompt};
        entry_append(g_db, session_id, &sys_msg);
        free(prompt);
    }

    /* V18: Insert message into inbox (decoupled from agent execution) */
    inbox_insert(g_db, session_id, "telegram", text->valuestring);

    /* V16: Try to acquire session lock via CAS */
    char lock_holder[64];
    snprintf(lock_holder, sizeof(lock_holder), "tg-%d-%lld", (int)getpid(), (long long)chat_id);

    if (session_try_acquire(g_db, session_id, lock_holder) != 0) {
        /* Session already locked — message stays in inbox for active handler */
        return;
    }

    /* V18: Consume inbox items into session entries */
    inbox_consume_into_entries(g_db, session_id, 100);

    /* Build per-session registry (includes session-persistent JS tools) */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg, g_cfg->shell_timeout, g_cfg->workspace, 0);
    tool_file_read_register(&reg, g_cfg->workspace);
    tool_file_write_register(&reg, g_cfg->workspace);
    tool_js_eval_register(&reg, NULL);
    JsSessionRuntime *js_rt = js_runtime_create();
    JsDefineCtx js_ctx = {.db = g_db, .session_id = session_id, .reg = &reg, .rt = js_rt};
    tool_js_define_register(&reg, &js_ctx);
    tool_js_load_session(g_db, session_id, &reg, js_rt);

    ToolCronCtx cron_ctx = {.db = g_db, .session_id = session_id};
    tool_cron_register(&reg, &cron_ctx);

    char tg_self_path[4096];
    ssize_t tg_self_len = readlink("/proc/self/exe", tg_self_path, sizeof(tg_self_path) - 1);
    if (tg_self_len > 0) tg_self_path[tg_self_len] = '\0';
    else strcpy(tg_self_path, "./build/cclaw");

    SubAgentCtx sa_ctx = {.db = g_db, .session_id = session_id,
                          .depth = 0, .self_path = tg_self_path};
    tool_spawn_agent_register(&reg, &sa_ctx);

    size_t local_tool_count = 0;
    const ToolSchema *local_schemas = tools_schemas(&reg, &local_tool_count);

    /* Start typing indicator thread */
    TypingCtx typing_ctx = {.token = g_cfg->telegram_token, .chat_id = chat_id, .active = 1};
    pthread_t typing_thread;
    pthread_create(&typing_thread, NULL, typing_loop, &typing_ctx);

    /* V16: Start keep-alive thread */
    TgKeepAliveCtx ka_ctx = {.db = g_db, .session_id = session_id,
                             .lock_holder = lock_holder, .stop = 0};
    pthread_t ka_thread;
    pthread_create(&ka_thread, NULL, tg_keepalive_thread, &ka_ctx);

    /* Run agent */
    AgentContext ctx = {0};
    ctx.db = g_db;
    ctx.session_id = session_id;
    ctx.cfg = g_cfg;
    ctx.dispatch = tg_dispatch;
    ctx.dispatch_data = &reg;
    ctx.tools = local_schemas;
    ctx.tool_count = local_tool_count;

    int rc = agent_run(&ctx);

    /* Stop typing indicator */
    typing_ctx.active = 0;
    pthread_join(typing_thread, NULL);

    /* Stop keep-alive */
    ka_ctx.stop = 1;
    pthread_join(ka_thread, NULL);

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
    js_runtime_destroy(js_rt);
    tools_free(&reg);

    /* V16: Release session lock */
    session_release(g_db, session_id, lock_holder);
}

static void *poll_loop(void *arg) {
    (void)arg;
    int64_t offset = 0;
    int failures = 0; /* V2: consecutive failure count for backoff */

    /* T25: Load persisted offset from DB */
    char *saved = db_kv_get(g_db, "tg_offset");
    if (saved) {
        offset = strtoll(saved, NULL, 10);
        free(saved);
    }

    /* Register tools */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg, g_cfg->shell_timeout, g_cfg->workspace, 0);
    tool_file_read_register(&reg, g_cfg->workspace);
    tool_file_write_register(&reg, g_cfg->workspace);
    tool_js_eval_register(&reg, NULL);

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

        int http_status = 0;
        cJSON *resp = tg_call_ex(g_cfg->telegram_token, "getUpdates", body, &http_status);
        free(body);

        if (!resp) {
            /* V2: Exponential backoff on transient errors */
            failures++;
            int delay = tg_backoff_delay(failures);
            /* Respect Retry-After for 429 — we don't have the header here,
             * but backoff naturally covers it with increasing delays */
            for (int i = 0; i < delay && running; i++)
                sleep(1);
            continue;
        }

        cJSON *ok = cJSON_GetObjectItemCaseSensitive(resp, "ok");
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        if (!cJSON_IsTrue(ok) || !cJSON_IsArray(result)) {
            cJSON_Delete(resp);
            /* V2: Treat API-level error as transient */
            failures++;
            int delay = tg_backoff_delay(failures);
            for (int i = 0; i < delay && running; i++)
                sleep(1);
            continue;
        }

        /* Success — reset backoff */
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

/* T85: Public send API for daemon response delivery */
void telegram_send_message(const char *token, int64_t chat_id, const char *text) {
    if (!token || !text) return;
    tg_send_chunked(token, chat_id, text);
}
