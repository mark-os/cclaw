#define _POSIX_C_SOURCE 200809L
#include "admin_api.h"
#include "agent_config.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

const char *admin_key_env_name(const char *provider) {
    if (!provider) return NULL;
    if (strcmp(provider, "openrouter") == 0) return "OPENROUTER_API_KEY";
    if (strcmp(provider, "gemini") == 0) return "GEMINI_API_KEY";
    return NULL;
}

int admin_write_env_key(const char *env_file, const char *var_name, const char *value) {
    if (!env_file || !var_name || !value) return -1;

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

    int fd = open(env_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(existing); return -1; }
    fchmod(fd, 0600);
    f = fdopen(fd, "w");
    if (!f) { close(fd); free(existing); return -1; }

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

int admin_set_key(const char *env_file, const char *provider, const char *value) {
    if (!env_file || !value) return -1;

    if (!provider || strcmp(provider, "custom") == 0) {
        /* Expect value as "VAR_NAME=actual_value" */
        const char *eq = strchr(value, '=');
        if (!eq || eq == value) return -1;
        size_t name_len = (size_t)(eq - value);
        char *var = malloc(name_len + 1);
        if (!var) return -1;
        memcpy(var, value, name_len);
        var[name_len] = '\0';
        int rc = admin_write_env_key(env_file, var, eq + 1);
        free(var);
        return rc;
    }

    const char *var_name = admin_key_env_name(provider);
    if (!var_name) return -1;
    return admin_write_env_key(env_file, var_name, value);
}

int admin_set_model(sqlite3 *db, int provider_index, const char *model) {
    if (!db || !model) return -1;
    /* Update default_model on the provider at the given priority index */
    const char *sql =
        "UPDATE providers SET default_model=? WHERE name="
        "(SELECT name FROM providers ORDER BY priority LIMIT 1 OFFSET ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, model, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, provider_index);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int admin_set_endpoint(sqlite3 *db, int provider_index, const char *url) {
    if (!db || !url) return -1;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return -1;
    const char *sql =
        "UPDATE providers SET base_url=? WHERE name="
        "(SELECT name FROM providers ORDER BY priority LIMIT 1 OFFSET ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, provider_index);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int admin_add_host(sqlite3 *db, const char *agent_name, const char *host) {
    return agent_config_add_host(db, agent_name, host);
}

int admin_remove_host(sqlite3 *db, const char *agent_name, const char *host) {
    return agent_config_remove_host(db, agent_name, host);
}

int admin_list_providers(sqlite3 *db, AdminProvider **out, size_t *out_count) {
    if (!db || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    const char *sql = "SELECT name, default_model, base_url FROM providers ORDER BY priority;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    size_t cap = 4, count = 0;
    AdminProvider *providers = calloc(cap, sizeof(AdminProvider));
    if (!providers) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            AdminProvider *tmp = realloc(providers, cap * sizeof(AdminProvider));
            if (!tmp) break;
            providers = tmp;
        }
        const char *m = (const char *)sqlite3_column_text(stmt, 1);
        const char *u = (const char *)sqlite3_column_text(stmt, 2);
        providers[count].index = (int)count;
        providers[count].model = m ? strdup(m) : NULL;
        providers[count].base_url = u ? strdup(u) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);

    *out = providers;
    *out_count = count;
    return 0;
}

void admin_providers_free(AdminProvider *providers, size_t count) {
    if (!providers) return;
    for (size_t i = 0; i < count; i++) {
        free(providers[i].model);
        free(providers[i].base_url);
    }
    free(providers);
}
