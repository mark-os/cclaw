#define _POSIX_C_SOURCE 200809L
#include "config_apply.h"
#include "agent_config.h"
#include "channel.h"
#include "db.h"
#include <cJSON.h>
#include <errno.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Known providers for URL/model defaults */
static const struct { const char *name; const char *base_url; const char *model; } PROVIDERS[] = {
    {"openrouter", "https://openrouter.ai/api/v1", "deepseek/deepseek-v4-flash"},
    {"deepseek", "https://api.deepseek.com/v1", "deepseek-chat"},
    {"openai", "https://api.openai.com/v1", "gpt-4o"},
    {"anthropic", "https://api.anthropic.com/v1", "claude-sonnet-4-20250514"},
    {"gemini", "https://generativelanguage.googleapis.com/v1beta", "gemini-2.5-flash"},
};
#define PROVIDER_COUNT (sizeof(PROVIDERS)/sizeof(PROVIDERS[0]))

char *config_apply(sqlite3 *db, const char *agent_name,
                   const char *tool_name, const char *args_json) {
    if (!tool_name || !args_json) return NULL;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid JSON");

    char *result = NULL;

    if (strcmp(tool_name, "configure_provider") == 0) {
        cJSON *provider = cJSON_GetObjectItem(args, "provider");
        cJSON *api_key = cJSON_GetObjectItem(args, "api_key");
        cJSON *base_url = cJSON_GetObjectItem(args, "base_url");
        cJSON *model = cJSON_GetObjectItem(args, "model");

        if (!cJSON_IsString(provider) || !cJSON_IsString(api_key)) {
            cJSON_Delete(args);
            return strdup("error: provider and api_key required");
        }

        const char *url = NULL, *mdl = NULL;
        for (size_t i = 0; i < PROVIDER_COUNT; i++) {
            if (strcmp(provider->valuestring, PROVIDERS[i].name) == 0) {
                url = PROVIDERS[i].base_url; mdl = PROVIDERS[i].model; break;
            }
        }
        if (cJSON_IsString(base_url) && base_url->valuestring[0]) url = base_url->valuestring;
        if (cJSON_IsString(model) && model->valuestring[0]) mdl = model->valuestring;

        db_kv_set_secret(db, "provider.api_key", api_key->valuestring);
        if (url) db_kv_set(db, "provider.base_url", url);
        if (mdl) db_kv_set(db, "provider.model", mdl);

        char buf[256];
        snprintf(buf, sizeof(buf), "provider configured: %s (model: %s)",
                 provider->valuestring, mdl ? mdl : "(default)");
        result = strdup(buf);

    } else if (strcmp(tool_name, "configure_channel") == 0) {
        cJSON *channel_type = cJSON_GetObjectItem(args, "channel_type");
        cJSON *bot_token = cJSON_GetObjectItem(args, "bot_token");
        cJSON *binary_path = cJSON_GetObjectItem(args, "binary_path");
        cJSON *config_obj = cJSON_GetObjectItem(args, "config");

        if (!cJSON_IsString(channel_type)) {
            cJSON_Delete(args); return strdup("error: channel_type required");
        }
        const char *ctype = channel_type->valuestring;

        if (strcmp(ctype, "telegram") == 0) {
            if (!cJSON_IsString(bot_token) || !bot_token->valuestring[0]) {
                cJSON_Delete(args); return strdup("error: bot_token required for telegram");
            }
            /* Store token in channel_state */
            const char *isql = "INSERT OR REPLACE INTO channel_state(channel_name, key, value)"
                               " VALUES('telegram','bot_token',?);";
            sqlite3_stmt *s;
            if (sqlite3_prepare_v2(db, isql, -1, &s, NULL) == SQLITE_OK) {
                sqlite3_bind_text(s, 1, bot_token->valuestring, -1, SQLITE_STATIC);
                sqlite3_step(s); sqlite3_finalize(s);
            }
            channel_register(db, "telegram", "telegram");
            db_channel_binding_set(db, "telegram", "*", agent_name);
            result = strdup("channel configured: telegram");
        } else if (strcmp(ctype, "cli") == 0) {
            db_channel_binding_set(db, "cli", "*", agent_name);
            result = strdup("channel configured: cli");
        } else {
            if (!cJSON_IsString(binary_path) || !binary_path->valuestring[0]) {
                cJSON_Delete(args); return strdup("error: binary_path required for custom channel");
            }
            channel_register(db, ctype, ctype);
            /* Seed config into channel_state */
            if (cJSON_IsObject(config_obj)) {
                cJSON *kv = NULL;
                cJSON_ArrayForEach(kv, config_obj) {
                    if (cJSON_IsString(kv) && kv->string) {
                        const char *ksql = "INSERT OR REPLACE INTO channel_state(channel_name,key,value)"
                                           " VALUES(?,?,?);";
                        sqlite3_stmt *ks;
                        if (sqlite3_prepare_v2(db, ksql, -1, &ks, NULL) == SQLITE_OK) {
                            sqlite3_bind_text(ks, 1, ctype, -1, SQLITE_STATIC);
                            sqlite3_bind_text(ks, 2, kv->string, -1, SQLITE_STATIC);
                            sqlite3_bind_text(ks, 3, kv->valuestring, -1, SQLITE_STATIC);
                            sqlite3_step(ks); sqlite3_finalize(ks);
                        }
                    }
                }
            }
            db_channel_binding_set(db, ctype, "*", agent_name);
            char buf[256];
            snprintf(buf, sizeof(buf), "channel configured: %s", ctype);
            result = strdup(buf);
        }

    } else if (strcmp(tool_name, "create_agent") == 0) {
        char *args_str = cJSON_PrintUnformatted(args);
        if (args_str) {
            int rc = agent_config_create("agents", db, args_str);
            if (rc == 0) {
                cJSON *name = cJSON_GetObjectItem(args, "name");
                char buf[256];
                snprintf(buf, sizeof(buf), "agent created: %s",
                         cJSON_IsString(name) ? name->valuestring : "unknown");
                result = strdup(buf);
            } else {
                result = strdup("error: failed to create agent");
            }
            free(args_str);
        }

    } else if (strcmp(tool_name, "rename_agent") == 0) {
        cJSON *new_name = cJSON_GetObjectItem(args, "name");
        if (!cJSON_IsString(new_name) || !new_name->valuestring[0]) {
            cJSON_Delete(args); return strdup("error: 'name' required");
        }
        const char *nn = new_name->valuestring;
        for (const char *p = nn; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
                cJSON_Delete(args); return strdup("error: name must be [a-z0-9_-]");
            }
        }

        /* Rename in DB */
        const char *upd = "UPDATE agents SET name=? WHERE name=?;";
        sqlite3_stmt *us;
        if (sqlite3_prepare_v2(db, upd, -1, &us, NULL) == SQLITE_OK) {
            sqlite3_bind_text(us, 1, nn, -1, SQLITE_STATIC);
            sqlite3_bind_text(us, 2, agent_name, -1, SQLITE_STATIC);
            sqlite3_step(us); sqlite3_finalize(us);
        }
        const char *ucfg = "UPDATE agent_config SET agent_name=? WHERE agent_name=?;";
        if (sqlite3_prepare_v2(db, ucfg, -1, &us, NULL) == SQLITE_OK) {
            sqlite3_bind_text(us, 1, nn, -1, SQLITE_STATIC);
            sqlite3_bind_text(us, 2, agent_name, -1, SQLITE_STATIC);
            sqlite3_step(us); sqlite3_finalize(us);
        }
        db_kv_set(db, "cli.agent", nn);
        char buf[256];
        snprintf(buf, sizeof(buf), "agent renamed: %s -> %s", agent_name, nn);
        result = strdup(buf);

    } else if (strcmp(tool_name, "request_config") == 0) {
        cJSON *action = cJSON_GetObjectItem(args, "action");
        if (!cJSON_IsString(action)) {
            cJSON_Delete(args); return strdup("error: 'action' required");
        }
        if (strcmp(action->valuestring, "grant_tool") == 0) {
            cJSON *tool = cJSON_GetObjectItem(args, "tool");
            if (!cJSON_IsString(tool) || !tool->valuestring[0]) {
                cJSON_Delete(args); return strdup("error: 'tool' required");
            }
            if (agent_config_add_tool(db, agent_name, tool->valuestring) == 0) {
                char buf[128]; snprintf(buf, sizeof(buf), "granted tool: %s", tool->valuestring);
                result = strdup(buf);
            } else result = strdup("error: failed to grant tool");
        } else if (strcmp(action->valuestring, "grant_host") == 0) {
            cJSON *host = cJSON_GetObjectItem(args, "host");
            if (!cJSON_IsString(host) || !host->valuestring[0]) {
                cJSON_Delete(args); return strdup("error: 'host' required");
            }
            if (agent_config_add_host(db, agent_name, host->valuestring) == 0) {
                char buf[128]; snprintf(buf, sizeof(buf), "granted host: %s", host->valuestring);
                result = strdup(buf);
            } else result = strdup("error: failed to grant host");
        } else {
            result = strdup("error: action must be grant_tool or grant_host");
        }

    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "error: unknown config tool: %s", tool_name);
        result = strdup(buf);
    }

    cJSON_Delete(args);
    return result;
}
