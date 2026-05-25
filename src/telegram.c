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

/* V53: Check if chat_id is in admin_chat_ids[]. */
int telegram_is_admin(const Config *cfg, int64_t chat_id) {
    if (!cfg || !cfg->admin_chat_ids) return 0;
    for (size_t i = 0; i < cfg->admin_chat_id_count; i++) {
        if (cfg->admin_chat_ids[i] == chat_id) return 1;
    }
    return 0;
}

/* V53: Check if text is an admin command (starts with /key, /config, /whitelist) */
static int is_admin_command(const char *text) {
    return (strncmp(text, "/key", 4) == 0 ||
            strncmp(text, "/config", 7) == 0 ||
            strncmp(text, "/whitelist", 10) == 0);
}

/* T140: Parse admin command and return command type.
 * Returns: 1=key, 2=config, 3=whitelist, 0=unknown */
int telegram_parse_admin_command(const char *text, const char **args_out) {
    if (strncmp(text, "/key", 4) == 0) {
        if (args_out) *args_out = text[4] == ' ' ? text + 5 : NULL;
        return 1;
    }
    if (strncmp(text, "/config", 7) == 0) {
        if (args_out) *args_out = text[7] == ' ' ? text + 8 : NULL;
        return 2;
    }
    if (strncmp(text, "/whitelist", 10) == 0) {
        if (args_out) *args_out = text[10] == ' ' ? text + 11 : NULL;
        return 3;
    }
    return 0;
}

/* T141: Pending /key dialog state (one per admin at a time) */
#define KEY_DIALOG_MAX 4
typedef struct {
    int64_t chat_id;
    char provider[32]; /* selected provider name */
} KeyDialog;
static KeyDialog key_dialogs[KEY_DIALOG_MAX];
static int key_dialog_count;

static KeyDialog *key_dialog_find(int64_t chat_id) {
    for (int i = 0; i < key_dialog_count; i++)
        if (key_dialogs[i].chat_id == chat_id) return &key_dialogs[i];
    return NULL;
}

static void key_dialog_remove(int64_t chat_id) {
    for (int i = 0; i < key_dialog_count; i++) {
        if (key_dialogs[i].chat_id == chat_id) {
            key_dialogs[i] = key_dialogs[--key_dialog_count];
            return;
        }
    }
}

/* T142: Pending /config model dialog state */
#define CFG_DIALOG_MAX 4
typedef struct {
    int64_t chat_id;
    int provider_index; /* 0=primary, 1+=fallback */
} CfgDialog;
static CfgDialog cfg_dialogs[CFG_DIALOG_MAX];
static int cfg_dialog_count;

static CfgDialog *cfg_dialog_find(int64_t chat_id) {
    for (int i = 0; i < cfg_dialog_count; i++)
        if (cfg_dialogs[i].chat_id == chat_id) return &cfg_dialogs[i];
    return NULL;
}

static void cfg_dialog_remove(int64_t chat_id) {
    for (int i = 0; i < cfg_dialog_count; i++) {
        if (cfg_dialogs[i].chat_id == chat_id) {
            cfg_dialogs[i] = cfg_dialogs[--cfg_dialog_count];
            return;
        }
    }
}

/* T143: Pending /config endpoint dialog state */
#define EP_DIALOG_MAX 4
typedef struct {
    int64_t chat_id;
    int provider_index;
} EpDialog;
static EpDialog ep_dialogs[EP_DIALOG_MAX];
static int ep_dialog_count;

static EpDialog *ep_dialog_find(int64_t chat_id) {
    for (int i = 0; i < ep_dialog_count; i++)
        if (ep_dialogs[i].chat_id == chat_id) return &ep_dialogs[i];
    return NULL;
}

static void ep_dialog_remove(int64_t chat_id) {
    for (int i = 0; i < ep_dialog_count; i++) {
        if (ep_dialogs[i].chat_id == chat_id) {
            ep_dialogs[i] = ep_dialogs[--ep_dialog_count];
            return;
        }
    }
}

/* T144: Pending /whitelist dialog state */
#define WL_DIALOG_MAX 4
typedef struct {
    int64_t chat_id;
    char agent_name[64];
    int removing; /* 1 = remove mode, 0 = add mode */
} WlDialog;
static WlDialog wl_dialogs[WL_DIALOG_MAX];
static int wl_dialog_count;

static WlDialog *wl_dialog_find(int64_t chat_id) {
    for (int i = 0; i < wl_dialog_count; i++)
        if (wl_dialogs[i].chat_id == chat_id) return &wl_dialogs[i];
    return NULL;
}

static void wl_dialog_remove(int64_t chat_id) {
    for (int i = 0; i < wl_dialog_count; i++) {
        if (wl_dialogs[i].chat_id == chat_id) {
            wl_dialogs[i] = wl_dialogs[--wl_dialog_count];
            return;
        }
    }
}

/* T141: Get env file path from config or default */
static const char *get_env_file_path(void) {
    if (g_cfg && g_cfg->env_file && g_cfg->env_file[0])
        return g_cfg->env_file;
    return "/etc/cclaw/env";
}

/* T141: Map provider selection to env var name */
const char *telegram_key_env_name(const char *provider) {
    if (strcmp(provider, "openrouter") == 0) return "OPENROUTER_API_KEY";
    if (strcmp(provider, "gemini") == 0) return "GEMINI_API_KEY";
    return NULL; /* custom: user provides VAR=value */
}

/* T141: Write key to env file. Returns 0 on success. V52: key ⊥ logged/DB/inbox */
int telegram_write_env_key(const char *env_file, const char *var_name, const char *value) {
    /* Read existing content */
    char *existing = NULL;
    size_t existing_len = 0;
    FILE *f = fopen(env_file, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        if (len > 0) {
            fseek(f, 0, SEEK_SET);
            existing = malloc((size_t)len + 1);
            if (existing) {
                existing_len = fread(existing, 1, (size_t)len, f);
                existing[existing_len] = '\0';
            }
        }
        fclose(f);
    }

    /* Build new content: replace existing line or append */
    f = fopen(env_file, "w");
    if (!f) { free(existing); return -1; }

    size_t var_len = strlen(var_name);
    int replaced = 0;
    if (existing) {
        char *line = existing;
        while (*line) {
            char *eol = strchr(line, '\n');
            size_t line_len = eol ? (size_t)(eol - line) : strlen(line);
            if (line_len > var_len && line[var_len] == '=' &&
                strncmp(line, var_name, var_len) == 0) {
                fprintf(f, "%s=%s\n", var_name, value);
                replaced = 1;
            } else if (line_len > 0) {
                fwrite(line, 1, line_len, f);
                fputc('\n', f);
            }
            line += line_len + (eol ? 1 : 0);
            if (!eol) break;
        }
        free(existing);
    }
    if (!replaced) fprintf(f, "%s=%s\n", var_name, value);
    fclose(f);
    return 0;
}

/* T141: Send inline keyboard for provider selection */
static void key_send_provider_keyboard(const char *token, int64_t chat_id) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(body, "text", "Select provider for API key:");

    cJSON *markup = cJSON_CreateObject();
    cJSON *rows = cJSON_CreateArray();
    cJSON *row = cJSON_CreateArray();

    cJSON *btn1 = cJSON_CreateObject();
    cJSON_AddStringToObject(btn1, "text", "OpenRouter");
    cJSON_AddStringToObject(btn1, "callback_data", "key:openrouter");
    cJSON_AddItemToArray(row, btn1);

    cJSON *btn2 = cJSON_CreateObject();
    cJSON_AddStringToObject(btn2, "text", "Gemini");
    cJSON_AddStringToObject(btn2, "callback_data", "key:gemini");
    cJSON_AddItemToArray(row, btn2);

    cJSON *btn3 = cJSON_CreateObject();
    cJSON_AddStringToObject(btn3, "text", "Custom");
    cJSON_AddStringToObject(btn3, "callback_data", "key:custom");
    cJSON_AddItemToArray(row, btn3);

    cJSON_AddItemToArray(rows, row);
    cJSON_AddItemToObject(markup, "inline_keyboard", rows);
    cJSON_AddItemToObject(body, "reply_markup", markup);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json) {
        cJSON *resp = tg_call(token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T141: Send ForceReply prompt for key input */
static void key_send_force_reply(const char *token, int64_t chat_id, const char *provider) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);

    char prompt[128];
    if (strcmp(provider, "custom") == 0)
        snprintf(prompt, sizeof(prompt), "Send the key as VAR_NAME=value (e.g. MY_KEY=sk-...):");
    else
        snprintf(prompt, sizeof(prompt), "Send the API key for %s:", provider);
    cJSON_AddStringToObject(body, "text", prompt);

    cJSON *markup = cJSON_CreateObject();
    cJSON_AddTrueToObject(markup, "force_reply");
    cJSON_AddTrueToObject(markup, "selective");
    cJSON_AddItemToObject(body, "reply_markup", markup);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json) {
        cJSON *resp = tg_call(token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T141: Handle callback_query for /key provider selection */
static void handle_key_callback(const char *data, int64_t chat_id, const char *callback_id) {
    /* Answer callback to dismiss loading indicator */
    if (callback_id) {
        cJSON *ans = cJSON_CreateObject();
        cJSON_AddStringToObject(ans, "callback_query_id", callback_id);
        char *json = cJSON_PrintUnformatted(ans);
        cJSON_Delete(ans);
        if (json) {
            cJSON *resp = tg_call(g_cfg->telegram_token, "answerCallbackQuery", json);
            cJSON_Delete(resp);
            free(json);
        }
    }

    /* Extract provider from "key:<provider>" */
    const char *provider = data + 4; /* skip "key:" */

    /* Register pending dialog */
    key_dialog_remove(chat_id);
    if (key_dialog_count < KEY_DIALOG_MAX) {
        KeyDialog *d = &key_dialogs[key_dialog_count++];
        d->chat_id = chat_id;
        snprintf(d->provider, sizeof(d->provider), "%s", provider);
    }

    key_send_force_reply(g_cfg->telegram_token, chat_id, provider);
}

/* T141: Handle key text reply from admin. Returns 1 if consumed, 0 if not a key dialog. */
static int handle_key_reply(int64_t chat_id, const char *text) {
    KeyDialog *d = key_dialog_find(chat_id);
    if (!d) return 0;

    char provider[32];
    snprintf(provider, sizeof(provider), "%s", d->provider);
    key_dialog_remove(chat_id);

    const char *env_file = get_env_file_path();
    int rc;

    if (strcmp(provider, "custom") == 0) {
        /* Expect VAR_NAME=value format */
        const char *eq = strchr(text, '=');
        if (!eq || eq == text) {
            telegram_send_message(g_cfg->telegram_token, chat_id,
                "Invalid format. Expected VAR_NAME=value");
            return 1;
        }
        size_t name_len = (size_t)(eq - text);
        char *var_name = malloc(name_len + 1);
        if (!var_name) return 1;
        memcpy(var_name, text, name_len);
        var_name[name_len] = '\0';
        rc = telegram_write_env_key(env_file, var_name, eq + 1);
        free(var_name);
    } else {
        const char *var_name = telegram_key_env_name(provider);
        if (!var_name) {
            telegram_send_message(g_cfg->telegram_token, chat_id, "Unknown provider.");
            return 1;
        }
        rc = telegram_write_env_key(env_file, var_name, text);
    }

    if (rc == 0)
        telegram_send_message(g_cfg->telegram_token, chat_id, "Key saved.");
    else
        telegram_send_message(g_cfg->telegram_token, chat_id,
            "Failed to write env file. Check permissions.");
    return 1;
}

/* T142: Send inline keyboard listing configured providers for model change */
static void config_send_provider_keyboard(const char *token, int64_t chat_id) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(body, "text", "Select provider to change model:");

    cJSON *markup = cJSON_CreateObject();
    cJSON *rows = cJSON_CreateArray();
    cJSON *row = cJSON_CreateArray();

    /* Primary provider (index 0) */
    char label[128];
    snprintf(label, sizeof(label), "Primary: %s",
             g_cfg->provider.model ? g_cfg->provider.model : "(none)");
    cJSON *btn = cJSON_CreateObject();
    cJSON_AddStringToObject(btn, "text", label);
    cJSON_AddStringToObject(btn, "callback_data", "cfg_model:0");
    cJSON_AddItemToArray(row, btn);
    cJSON_AddItemToArray(rows, row);

    /* Fallback providers */
    for (size_t i = 0; i < g_cfg->fallback_count; i++) {
        row = cJSON_CreateArray();
        snprintf(label, sizeof(label), "Fallback %zu: %s", i + 1,
                 g_cfg->fallback_providers[i].model ? g_cfg->fallback_providers[i].model : "(none)");
        char cb_data[32];
        snprintf(cb_data, sizeof(cb_data), "cfg_model:%zu", i + 1);
        btn = cJSON_CreateObject();
        cJSON_AddStringToObject(btn, "text", label);
        cJSON_AddStringToObject(btn, "callback_data", cb_data);
        cJSON_AddItemToArray(row, btn);
        cJSON_AddItemToArray(rows, row);
    }

    cJSON_AddItemToObject(markup, "inline_keyboard", rows);
    cJSON_AddItemToObject(body, "reply_markup", markup);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json) {
        cJSON *resp = tg_call(token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T142: Handle callback_query for /config model provider selection */
static void handle_config_model_callback(const char *data, int64_t chat_id, const char *callback_id) {
    /* Answer callback to dismiss loading indicator */
    if (callback_id) {
        cJSON *ans = cJSON_CreateObject();
        cJSON_AddStringToObject(ans, "callback_query_id", callback_id);
        char *json = cJSON_PrintUnformatted(ans);
        cJSON_Delete(ans);
        if (json) {
            cJSON *resp = tg_call(g_cfg->telegram_token, "answerCallbackQuery", json);
            cJSON_Delete(resp);
            free(json);
        }
    }

    /* Extract provider index from "cfg_model:<index>" */
    int idx = atoi(data + 10); /* skip "cfg_model:" */

    /* Register pending dialog */
    cfg_dialog_remove(chat_id);
    if (cfg_dialog_count < CFG_DIALOG_MAX) {
        CfgDialog *d = &cfg_dialogs[cfg_dialog_count++];
        d->chat_id = chat_id;
        d->provider_index = idx;
    }

    /* Send ForceReply for model name */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);

    const char *current = NULL;
    if (idx == 0)
        current = g_cfg->provider.model;
    else if ((size_t)(idx - 1) < g_cfg->fallback_count)
        current = g_cfg->fallback_providers[idx - 1].model;

    char prompt[256];
    snprintf(prompt, sizeof(prompt), "Enter new model name%s%s%s:",
             current ? " (current: " : "", current ? current : "", current ? ")" : "");
    cJSON_AddStringToObject(body, "text", prompt);

    cJSON *markup = cJSON_CreateObject();
    cJSON_AddTrueToObject(markup, "force_reply");
    cJSON_AddTrueToObject(markup, "selective");
    cJSON_AddItemToObject(body, "reply_markup", markup);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json) {
        cJSON *resp = tg_call(g_cfg->telegram_token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T142: Handle model name reply from admin. Returns 1 if consumed, 0 if not a config dialog. */
static int handle_config_reply(int64_t chat_id, const char *text) {
    CfgDialog *d = cfg_dialog_find(chat_id);
    if (!d) return 0;

    int idx = d->provider_index;
    cfg_dialog_remove(chat_id);

    const char *config_path = daemon_get_config_path();
    if (!config_path || !config_path[0]) {
        telegram_send_message(g_cfg->telegram_token, chat_id,
            "No config file path set. Cannot update model.");
        return 1;
    }

    if (config_update_model(config_path, idx, text) != 0) {
        telegram_send_message(g_cfg->telegram_token, chat_id,
            "Failed to update config file.");
        return 1;
    }

    /* Update model string in-place (daemon forks re-read config from disk) */
    char **model_ptr = NULL;
    if (idx == 0)
        model_ptr = (char **)&g_cfg->provider.model;
    else if ((size_t)(idx - 1) < g_cfg->fallback_count)
        model_ptr = (char **)&g_cfg->fallback_providers[idx - 1].model;
    if (model_ptr) {
        free(*model_ptr);
        *model_ptr = strdup(text);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Model updated to: %s", text);
    telegram_send_message(g_cfg->telegram_token, chat_id, msg);
    return 1;
}

/* T143: Send inline keyboard listing providers for endpoint change */
static void config_send_endpoint_keyboard(const char *token, int64_t chat_id) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(body, "text", "Select provider to change endpoint:");

    cJSON *markup = cJSON_CreateObject();
    cJSON *rows = cJSON_CreateArray();
    cJSON *row = cJSON_CreateArray();

    char label[256];
    snprintf(label, sizeof(label), "Primary: %s",
             g_cfg->provider.base_url ? g_cfg->provider.base_url : "(none)");
    cJSON *btn = cJSON_CreateObject();
    cJSON_AddStringToObject(btn, "text", label);
    cJSON_AddStringToObject(btn, "callback_data", "cfg_ep:0");
    cJSON_AddItemToArray(row, btn);
    cJSON_AddItemToArray(rows, row);

    for (size_t i = 0; i < g_cfg->fallback_count; i++) {
        row = cJSON_CreateArray();
        snprintf(label, sizeof(label), "Fallback %zu: %s", i + 1,
                 g_cfg->fallback_providers[i].base_url ? g_cfg->fallback_providers[i].base_url : "(none)");
        char cb_data[32];
        snprintf(cb_data, sizeof(cb_data), "cfg_ep:%zu", i + 1);
        btn = cJSON_CreateObject();
        cJSON_AddStringToObject(btn, "text", label);
        cJSON_AddStringToObject(btn, "callback_data", cb_data);
        cJSON_AddItemToArray(row, btn);
        cJSON_AddItemToArray(rows, row);
    }

    cJSON_AddItemToObject(markup, "inline_keyboard", rows);
    cJSON_AddItemToObject(body, "reply_markup", markup);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json) {
        cJSON *resp = tg_call(token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T143: Handle callback_query for /config endpoint provider selection */
static void handle_config_endpoint_callback(const char *data, int64_t chat_id, const char *callback_id) {
    if (callback_id) {
        cJSON *ans = cJSON_CreateObject();
        cJSON_AddStringToObject(ans, "callback_query_id", callback_id);
        char *json = cJSON_PrintUnformatted(ans);
        cJSON_Delete(ans);
        if (json) {
            cJSON *resp = tg_call(g_cfg->telegram_token, "answerCallbackQuery", json);
            cJSON_Delete(resp);
            free(json);
        }
    }

    int idx = atoi(data + 6); /* skip "cfg_ep:" */

    ep_dialog_remove(chat_id);
    if (ep_dialog_count < EP_DIALOG_MAX) {
        EpDialog *d = &ep_dialogs[ep_dialog_count++];
        d->chat_id = chat_id;
        d->provider_index = idx;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);

    const char *current = NULL;
    if (idx == 0)
        current = g_cfg->provider.base_url;
    else if ((size_t)(idx - 1) < g_cfg->fallback_count)
        current = g_cfg->fallback_providers[idx - 1].base_url;

    char prompt[512];
    snprintf(prompt, sizeof(prompt), "Enter new base URL (must start with http:// or https://)%s%s%s:",
             current ? "\n(current: " : "", current ? current : "", current ? ")" : "");
    cJSON_AddStringToObject(body, "text", prompt);

    cJSON *mk = cJSON_CreateObject();
    cJSON_AddTrueToObject(mk, "force_reply");
    cJSON_AddTrueToObject(mk, "selective");
    cJSON_AddItemToObject(body, "reply_markup", mk);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json) {
        cJSON *resp = tg_call(g_cfg->telegram_token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T143: Handle endpoint URL reply from admin. Returns 1 if consumed, 0 if not. */
static int handle_endpoint_reply(int64_t chat_id, const char *text) {
    EpDialog *d = ep_dialog_find(chat_id);
    if (!d) return 0;

    int idx = d->provider_index;
    ep_dialog_remove(chat_id);

    const char *config_path = daemon_get_config_path();
    if (!config_path || !config_path[0]) {
        telegram_send_message(g_cfg->telegram_token, chat_id,
            "No config file path set. Cannot update endpoint.");
        return 1;
    }

    if (config_update_endpoint(config_path, idx, text) != 0) {
        telegram_send_message(g_cfg->telegram_token, chat_id,
            "Failed to update endpoint. Ensure URL starts with http:// or https://");
        return 1;
    }

    /* Update base_url in-place */
    char **url_ptr = NULL;
    if (idx == 0)
        url_ptr = (char **)&g_cfg->provider.base_url;
    else if ((size_t)(idx - 1) < g_cfg->fallback_count)
        url_ptr = (char **)&g_cfg->fallback_providers[idx - 1].base_url;
    if (url_ptr) {
        free(*url_ptr);
        *url_ptr = strdup(text);
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "Endpoint updated to: %s", text);
    telegram_send_message(g_cfg->telegram_token, chat_id, msg);
    return 1;
}

/* T144: Send inline keyboard listing agents for /whitelist */
static void wl_send_agent_keyboard(const char *token, int64_t chat_id, int removing) {
    size_t count = 0;
    char **names = agent_discover("agents", &count);
    if (!names || count == 0) {
        telegram_send_message(token, chat_id, "No agents found in agents/ directory.");
        agent_discover_free(names, count);
        return;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(body, "text",
        removing ? "Select agent to remove host from:" : "Select agent to add host to:");

    cJSON *markup = cJSON_CreateObject();
    cJSON *rows = cJSON_CreateArray();
    for (size_t i = 0; i < count && i < 10; i++) {
        cJSON *row = cJSON_CreateArray();
        cJSON *btn = cJSON_CreateObject();
        cJSON_AddStringToObject(btn, "text", names[i]);
        char cb[96];
        snprintf(cb, sizeof(cb), "wl:%s:%d", names[i], removing);
        cJSON_AddStringToObject(btn, "callback_data", cb);
        cJSON_AddItemToArray(row, btn);
        cJSON_AddItemToArray(rows, row);
    }
    cJSON_AddItemToObject(markup, "inline_keyboard", rows);
    cJSON_AddItemToObject(body, "reply_markup", markup);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    agent_discover_free(names, count);
    if (json) {
        cJSON *resp = tg_call(token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T144: Handle callback_query for /whitelist agent selection */
static void handle_wl_callback(const char *data, int64_t chat_id, const char *callback_id) {
    if (callback_id) {
        cJSON *ans = cJSON_CreateObject();
        cJSON_AddStringToObject(ans, "callback_query_id", callback_id);
        char *json = cJSON_PrintUnformatted(ans);
        cJSON_Delete(ans);
        if (json) {
            cJSON *resp = tg_call(g_cfg->telegram_token, "answerCallbackQuery", json);
            cJSON_Delete(resp);
            free(json);
        }
    }

    /* Parse "wl:<agent_name>:<removing>" */
    const char *p = data + 3; /* skip "wl:" */
    const char *colon = strrchr(p, ':');
    if (!colon) return;

    size_t name_len = (size_t)(colon - p);
    if (name_len == 0 || name_len >= 64) return;

    int removing = atoi(colon + 1);

    /* Show current hosts */
    char agent_name[64];
    memcpy(agent_name, p, name_len);
    agent_name[name_len] = '\0';

    size_t host_count = 0;
    char **hosts = agent_config_get_hosts("agents", agent_name, &host_count);

    char msg[2048];
    int off = snprintf(msg, sizeof(msg), "Agent: %s\nCurrent allowed_hosts: ", agent_name);
    if (host_count == 0) {
        off += snprintf(msg + off, sizeof(msg) - (size_t)off, "(none)");
    } else {
        for (size_t i = 0; i < host_count && (size_t)off < sizeof(msg) - 64; i++)
            off += snprintf(msg + off, sizeof(msg) - (size_t)off, "%s%s",
                            i > 0 ? ", " : "", hosts[i]);
    }
    if (hosts) {
        for (size_t i = 0; i < host_count; i++) free(hosts[i]);
        free(hosts);
    }

    snprintf(msg + off, sizeof(msg) - (size_t)off, "\n\nEnter hostname to %s:",
             removing ? "remove" : "add");

    /* Register pending dialog */
    wl_dialog_remove(chat_id);
    if (wl_dialog_count < WL_DIALOG_MAX) {
        WlDialog *d = &wl_dialogs[wl_dialog_count++];
        d->chat_id = chat_id;
        snprintf(d->agent_name, sizeof(d->agent_name), "%s", agent_name);
        d->removing = removing;
    }

    /* Send ForceReply */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(body, "text", msg);
    cJSON *mk = cJSON_CreateObject();
    cJSON_AddTrueToObject(mk, "force_reply");
    cJSON_AddTrueToObject(mk, "selective");
    cJSON_AddItemToObject(body, "reply_markup", mk);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json) {
        cJSON *resp = tg_call(g_cfg->telegram_token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T144: Handle whitelist host reply from admin. Returns 1 if consumed, 0 if not. */
static int handle_whitelist_reply(int64_t chat_id, const char *text) {
    WlDialog *d = wl_dialog_find(chat_id);
    if (!d) return 0;

    char agent_name[64];
    snprintf(agent_name, sizeof(agent_name), "%s", d->agent_name);
    int removing = d->removing;
    wl_dialog_remove(chat_id);

    int rc;
    if (removing)
        rc = agent_config_remove_host("agents", agent_name, text);
    else
        rc = agent_config_add_host("agents", agent_name, text);

    char msg[256];
    if (rc == 0)
        snprintf(msg, sizeof(msg), "Host \"%s\" %s for agent \"%s\".",
                 text, removing ? "removed" : "added", agent_name);
    else
        snprintf(msg, sizeof(msg), "Failed to update allowed_hosts for agent \"%s\".", agent_name);
    telegram_send_message(g_cfg->telegram_token, chat_id, msg);
    return 1;
}

/* T148: Send approval request to a single admin chat with Approve/Deny buttons */
static void approval_send_to_admin(const char *token, int64_t chat_id, const Approval *a) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);

    char text_buf[512];
    snprintf(text_buf, sizeof(text_buf),
             "\xf0\x9f\x94\x91 Approval Request #%lld\n"
             "Agent: %s\nType: %s\nPayload: %s",
             (long long)a->id,
             a->agent_name ? a->agent_name : "unknown",
             a->type ? a->type : "unknown",
             a->payload ? a->payload : "{}");
    cJSON_AddStringToObject(body, "text", text_buf);

    cJSON *markup = cJSON_CreateObject();
    cJSON *rows = cJSON_CreateArray();
    cJSON *row = cJSON_CreateArray();

    char approve_data[32], deny_data[32];
    snprintf(approve_data, sizeof(approve_data), "approve:%lld", (long long)a->id);
    snprintf(deny_data, sizeof(deny_data), "deny:%lld", (long long)a->id);

    cJSON *btn_approve = cJSON_CreateObject();
    cJSON_AddStringToObject(btn_approve, "text", "\xe2\x9c\x85 Approve");
    cJSON_AddStringToObject(btn_approve, "callback_data", approve_data);
    cJSON_AddItemToArray(row, btn_approve);

    cJSON *btn_deny = cJSON_CreateObject();
    cJSON_AddStringToObject(btn_deny, "text", "\xe2\x9d\x8c Deny");
    cJSON_AddStringToObject(btn_deny, "callback_data", deny_data);
    cJSON_AddItemToArray(row, btn_deny);

    cJSON_AddItemToArray(rows, row);
    cJSON_AddItemToObject(markup, "inline_keyboard", rows);
    cJSON_AddItemToObject(body, "reply_markup", markup);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json) {
        cJSON *resp = tg_call(token, "sendMessage", json);
        cJSON_Delete(resp);
        free(json);
    }
}

/* T148: Check for unnotified pending approvals and deliver to all admins */
static void telegram_check_approvals(void) {
    if (!g_cfg || !g_cfg->telegram_token || !g_db) return;
    if (g_cfg->admin_chat_id_count == 0) return;

    int count = 0;
    Approval *list = approval_list_unnotified(g_db, &count);
    if (!list) return;

    for (int i = 0; i < count; i++) {
        for (size_t j = 0; j < g_cfg->admin_chat_id_count; j++) {
            approval_send_to_admin(g_cfg->telegram_token,
                                   g_cfg->admin_chat_ids[j], &list[i]);
        }
        approval_mark_notified(g_db, list[i].id);
    }
    approval_list_free(list, count);
}

/* T149: Apply approved change based on approval type. Returns 0 on success. */
static int apply_approval(const Approval *a) {
    if (!a->type || !a->payload) return -1;

    cJSON *payload = cJSON_Parse(a->payload);
    if (!payload) return -1;

    int rc = -1;
    if (strcmp(a->type, "whitelist_host") == 0) {
        cJSON *host = cJSON_GetObjectItemCaseSensitive(payload, "host");
        if (cJSON_IsString(host) && a->agent_name)
            rc = agent_config_add_host("agents", a->agent_name, host->valuestring);
    } else if (strcmp(a->type, "model_change") == 0) {
        cJSON *model = cJSON_GetObjectItemCaseSensitive(payload, "model");
        cJSON *pidx = cJSON_GetObjectItemCaseSensitive(payload, "provider_index");
        int idx = (pidx && cJSON_IsNumber(pidx)) ? pidx->valueint : 0;
        const char *config_path = daemon_get_config_path();
        if (cJSON_IsString(model) && config_path && config_path[0])
            rc = config_update_model(config_path, idx, model->valuestring);
    } else if (strcmp(a->type, "tool_enable") == 0) {
        /* Tool enable is a config-level change — accepted but no-op for now */
        rc = 0;
    } else if (strcmp(a->type, "create_agent") == 0) {
        /* T150: create agent on disk + seed DB */
        rc = agent_config_create("agents", g_db, a->payload);
    }

    cJSON_Delete(payload);
    return rc;
}

/* T148/T149: Handle approve/deny callback from admin inline keyboard */
static void handle_approval_callback(const char *data, int64_t chat_id, const char *callback_id) {
    int approving = (strncmp(data, "approve:", 8) == 0);
    const char *id_str = data + (approving ? 8 : 5); /* "approve:" or "deny:" */
    int64_t approval_id = strtoll(id_str, NULL, 10);
    if (approval_id <= 0) return;

    const char *status = approving ? "approved" : "denied";
    int rc = approval_resolve(g_db, approval_id, status, chat_id);

    /* Answer callback query */
    if (callback_id) {
        char ans[128];
        snprintf(ans, sizeof(ans),
                 "{\"callback_query_id\":\"%s\",\"text\":\"Approval #%lld %s\"}",
                 callback_id, (long long)approval_id, status);
        cJSON *r = tg_call(g_cfg->telegram_token, "answerCallbackQuery", ans);
        cJSON_Delete(r);
    }

    if (rc != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Failed to resolve approval #%lld (already resolved?).",
                 (long long)approval_id);
        telegram_send_message(g_cfg->telegram_token, chat_id, msg);
        return;
    }

    /* Fetch full approval details for apply + inbox post */
    Approval *a = approval_get(g_db, approval_id);
    if (!a) return;

    /* T149: If approved, apply the change */
    char inbox_msg[256];
    if (approving) {
        int apply_rc = apply_approval(a);
        if (apply_rc == 0)
            snprintf(inbox_msg, sizeof(inbox_msg),
                     "Approval #%lld approved: %s %s",
                     (long long)approval_id, a->type, a->payload ? a->payload : "");
        else
            snprintf(inbox_msg, sizeof(inbox_msg),
                     "Approval #%lld approved but failed to apply: %s",
                     (long long)approval_id, a->type);
    } else {
        snprintf(inbox_msg, sizeof(inbox_msg),
                 "Approval #%lld denied: %s",
                 (long long)approval_id, a->type);
    }

    /* Post result to agent inbox (V25) */
    if (a->session_id > 0) {
        inbox_insert(g_db, a->session_id, "approval", inbox_msg);
        /* Transition waiting→idle and signal daemon to wake agent */
        const char *wake_sql =
            "UPDATE sessions SET state='idle' WHERE id=? AND state='waiting';";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(g_db, wake_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, a->session_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        daemon_signal_session(a->session_id);
    }

    /* Notify admin */
    char admin_msg[128];
    snprintf(admin_msg, sizeof(admin_msg), "Approval #%lld %s.",
             (long long)approval_id, status);
    telegram_send_message(g_cfg->telegram_token, chat_id, admin_msg);

    approval_free(a);
}

/* T140: Dispatch admin command to handler. */
static void dispatch_admin_command(const char *text, int64_t chat_id) {
    const char *args = NULL;
    int cmd = telegram_parse_admin_command(text, &args);

    switch (cmd) {
    case 1: /* /key — T141 */
        key_send_provider_keyboard(g_cfg->telegram_token, chat_id);
        break;
    case 2: /* /config — T142/T143 */
        if (args && strncmp(args, "model", 5) == 0)
            config_send_provider_keyboard(g_cfg->telegram_token, chat_id);
        else if (args && strncmp(args, "endpoint", 8) == 0)
            config_send_endpoint_keyboard(g_cfg->telegram_token, chat_id);
        else
            telegram_send_message(g_cfg->telegram_token, chat_id,
                "Usage: /config model | /config endpoint");
        break;
    case 3: /* /whitelist — T144 */
        if (args && strncmp(args, "remove", 6) == 0)
            wl_send_agent_keyboard(g_cfg->telegram_token, chat_id, 1);
        else
            wl_send_agent_keyboard(g_cfg->telegram_token, chat_id, 0);
        break;
    default:
        telegram_send_message(g_cfg->telegram_token, chat_id,
            "Unknown admin command.");
        break;
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

    /* V53: intercept admin commands before inbox_insert */
    if (is_admin_command(text->valuestring)) {
        if (!telegram_is_admin(g_cfg, chat_id)) return; /* silent ignore */
        dispatch_admin_command(text->valuestring, chat_id);
        return;
    }

    /* T141/V52: intercept key reply from admin (pending dialog) */
    if (telegram_is_admin(g_cfg, chat_id) && handle_key_reply(chat_id, text->valuestring))
        return;

    /* T142: intercept config model reply from admin */
    if (telegram_is_admin(g_cfg, chat_id) && handle_config_reply(chat_id, text->valuestring))
        return;

    /* T143: intercept config endpoint reply from admin */
    if (telegram_is_admin(g_cfg, chat_id) && handle_endpoint_reply(chat_id, text->valuestring))
        return;

    /* T144: intercept whitelist reply from admin */
    if (telegram_is_admin(g_cfg, chat_id) && handle_whitelist_reply(chat_id, text->valuestring))
        return;

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

            /* T141: handle callback_query (inline keyboard responses) */
            cJSON *cbq = cJSON_GetObjectItemCaseSensitive(update, "callback_query");
            if (cbq) {
                cJSON *cb_data = cJSON_GetObjectItemCaseSensitive(cbq, "data");
                cJSON *cb_from = cJSON_GetObjectItemCaseSensitive(cbq, "from");
                cJSON *cb_id = cJSON_GetObjectItemCaseSensitive(cbq, "id");
                if (cb_data && cJSON_IsString(cb_data) && cb_from) {
                    cJSON *from_id = cJSON_GetObjectItemCaseSensitive(cb_from, "id");
                    if (from_id && cJSON_IsNumber(from_id)) {
                        int64_t from_chat_id = (int64_t)from_id->valuedouble;
                        if (telegram_is_admin(g_cfg, from_chat_id)) {
                            const char *cb_id_str = (cb_id && cJSON_IsString(cb_id)) ? cb_id->valuestring : NULL;
                            if (strncmp(cb_data->valuestring, "key:", 4) == 0)
                                handle_key_callback(cb_data->valuestring, from_chat_id, cb_id_str);
                            else if (strncmp(cb_data->valuestring, "cfg_model:", 10) == 0)
                                handle_config_model_callback(cb_data->valuestring, from_chat_id, cb_id_str);
                            else if (strncmp(cb_data->valuestring, "cfg_ep:", 7) == 0)
                                handle_config_endpoint_callback(cb_data->valuestring, from_chat_id, cb_id_str);
                            else if (strncmp(cb_data->valuestring, "wl:", 3) == 0)
                                handle_wl_callback(cb_data->valuestring, from_chat_id, cb_id_str);
                            else if (strncmp(cb_data->valuestring, "approve:", 8) == 0 ||
                                     strncmp(cb_data->valuestring, "deny:", 5) == 0)
                                handle_approval_callback(cb_data->valuestring, from_chat_id, cb_id_str);
                        }
                    }
                }
            }
        }

        if (offset > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)offset);
            db_kv_set(g_db, "tg_offset", buf);
        }

        cJSON_Delete(resp);

        /* T148: deliver any unnotified pending approvals to admins */
        telegram_check_approvals();
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
