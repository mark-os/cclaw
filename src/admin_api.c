#define _POSIX_C_SOURCE 200809L
#include "admin_api.h"
#include "agent_config.h"
#include "db.h"
#include "tool_parse.h"
#include <stdlib.h>
#include <string.h>

const char *admin_key_env_name(const char *provider) {
    if (!provider) return NULL;
    if (strcmp(provider, "openrouter") == 0) return "OPENROUTER_API_KEY";
    if (strcmp(provider, "gemini") == 0) return "GEMINI_API_KEY";
    return NULL;
}

int admin_set_key(sqlite3 *db, const char *provider, const char *value) {
    if (!db || !value) return -1;

    if (!provider || strcmp(provider, "custom") == 0) {
        /* Expect value as "VAR_NAME=actual_value" */
        const char *eq = strchr(value, '=');
        if (!eq || eq == value) return -1;
        size_t name_len = (size_t)(eq - value);
        char *var = malloc(name_len + 1);
        if (!var) return -1;
        memcpy(var, value, name_len);
        var[name_len] = '\0';
        int rc = db_secret_set(db, var, eq + 1, "operator", "active", "system");
        free(var);
        return rc;
    }

    const char *var_name = admin_key_env_name(provider);
    if (!var_name) return -1;
    return db_secret_set(db, var_name, value, "operator", "active", "system");
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

int admin_list_grants(sqlite3 *db, const char *agent_name,
                      AdminGrant **out, size_t *out_count) {
    if (!db || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;
    if (!agent_name) return -1;

    const char *sql =
        "SELECT rowid, kind, value FROM grants WHERE agent_name=?1"
        " AND (expires_at IS NULL OR expires_at > unixepoch())"
        " ORDER BY kind, value;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);

    size_t cap = 8, count = 0;
    AdminGrant *list = calloc(cap, sizeof(AdminGrant));
    if (!list) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            AdminGrant *tmp = realloc(list, cap * sizeof(AdminGrant));
            if (!tmp) { admin_grants_free(list, count); sqlite3_finalize(stmt); return -1; }
            list = tmp;
        }
        const char *k = (const char *)sqlite3_column_text(stmt, 1);
        const char *v = (const char *)sqlite3_column_text(stmt, 2);
        list[count].id = sqlite3_column_int64(stmt, 0);
        list[count].kind = k ? strdup(k) : NULL;
        list[count].value = v ? strdup(v) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);

    *out = list;
    *out_count = count;
    return 0;
}

void admin_grants_free(AdminGrant *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i].kind);
        free(list[i].value);
    }
    free(list);
}

int admin_revoke_grant_by_id(sqlite3 *db, int64_t grant_id) {
    if (!db) return -1;
    const char *sql = "SELECT agent_name, kind, value FROM grants WHERE rowid=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, grant_id);

    char *agent = NULL, *kind = NULL, *value = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *a = (const char *)sqlite3_column_text(stmt, 0);
        const char *k = (const char *)sqlite3_column_text(stmt, 1);
        const char *v = (const char *)sqlite3_column_text(stmt, 2);
        if (a) agent = strdup(a);
        if (k) kind = strdup(k);
        if (v) value = strdup(v);
    }
    sqlite3_finalize(stmt);

    int rc = -1;
    if (agent && kind && value) rc = agent_config_revoke(db, agent, kind, value);
    free(agent); free(kind); free(value);
    return rc;
}

int admin_grant_capability(sqlite3 *db, const char *agent_name,
                           const char *kind, const char *value) {
    return agent_config_grant(db, agent_name, kind, value, 0);
}

int admin_list_tool_names(sqlite3 *db, char ***out, size_t *out_count) {
    if (!db || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    const char *sql = "SELECT name FROM tools ORDER BY name;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    size_t cap = 8, count = 0;
    char **names = calloc(cap, sizeof(char *));
    if (!names) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(names, cap * sizeof(char *));
            if (!tmp) { admin_tool_names_free(names, count); sqlite3_finalize(stmt); return -1; }
            names = tmp;
        }
        const char *n = (const char *)sqlite3_column_text(stmt, 0);
        names[count++] = n ? strdup(n) : NULL;
    }
    sqlite3_finalize(stmt);

    *out = names;
    *out_count = count;
    return 0;
}

void admin_tool_names_free(char **names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
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
            if (!tmp) {
                admin_providers_free(providers, count);
                sqlite3_finalize(stmt);
                return -1;
            }
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

static int admin_list_approvals_by_state(sqlite3 *db, const char *channel_name,
                                         const char *state, int grantable_only,
                                         int limit, AdminApproval **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!db || !channel_name || !state) return -1;

    /* grantable_only restricts to request_config's own actions (grant_tool/
     * grant_host/grant_path) — the only ones admin_grant_from_history can
     * actually apply. Used for the denial-history/"Grant now" listing;
     * pending approvals (any tool) are never filtered this way. */
    const char *sql = grantable_only ?
        "SELECT a.id, a.session_id, s.agent_name, a.tool_name, a.action, a.args_json"
        " FROM approvals a JOIN sessions s ON s.id = a.session_id"
        " WHERE a.state=?1 AND s.channel_name=?2 AND a.tool_name='request_config'"
        " ORDER BY a.id DESC LIMIT ?3;" :
        "SELECT a.id, a.session_id, s.agent_name, a.tool_name, a.action, a.args_json"
        " FROM approvals a JOIN sessions s ON s.id = a.session_id"
        " WHERE a.state=?1 AND s.channel_name=?2"
        " ORDER BY a.id DESC LIMIT ?3;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, channel_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, limit > 0 ? limit : -1);

    size_t cap = 4, count = 0;
    AdminApproval *list = calloc(cap, sizeof(AdminApproval));
    if (!list) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            AdminApproval *tmp = realloc(list, cap * sizeof(AdminApproval));
            if (!tmp) { admin_approvals_free(list, count); sqlite3_finalize(stmt); return -1; }
            list = tmp;
        }
        const char *ag = (const char *)sqlite3_column_text(stmt, 2);
        const char *tn = (const char *)sqlite3_column_text(stmt, 3);
        const char *ac = (const char *)sqlite3_column_text(stmt, 4);
        const char *aj = (const char *)sqlite3_column_text(stmt, 5);
        list[count].id = sqlite3_column_int64(stmt, 0);
        list[count].session_id = sqlite3_column_int64(stmt, 1);
        list[count].agent_name = ag ? strdup(ag) : NULL;
        list[count].tool_name = tn ? strdup(tn) : NULL;
        list[count].action = ac ? strdup(ac) : NULL;
        list[count].args_json = aj ? strdup(aj) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);

    *out = list;
    *out_count = count;
    return 0;
}

int admin_list_pending_approvals(sqlite3 *db, const char *channel_name,
                                 AdminApproval **out, size_t *out_count) {
    return admin_list_approvals_by_state(db, channel_name, "pending", 0, 0, out, out_count);
}

int admin_list_denied_approvals(sqlite3 *db, const char *channel_name, int limit,
                                AdminApproval **out, size_t *out_count) {
    return admin_list_approvals_by_state(db, channel_name, "denied", 1, limit, out, out_count);
}

void admin_approvals_free(AdminApproval *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i].agent_name);
        free(list[i].tool_name);
        free(list[i].action);
        free(list[i].args_json);
    }
    free(list);
}

int admin_grant_from_history(sqlite3 *db, int64_t approval_id) {
    if (!db) return -1;

    const char *sql = "SELECT session_id, tool_name, action, args_json FROM approvals WHERE id=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, approval_id);

    int64_t session_id = -1;
    char *tool_name = NULL, *action = NULL, *args_json = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        session_id = sqlite3_column_int64(stmt, 0);
        const char *tn = (const char *)sqlite3_column_text(stmt, 1);
        const char *ac = (const char *)sqlite3_column_text(stmt, 2);
        const char *aj = (const char *)sqlite3_column_text(stmt, 3);
        if (tn) tool_name = strdup(tn);
        if (ac) action = strdup(ac);
        if (aj) args_json = strdup(aj);
    }
    sqlite3_finalize(stmt);

    int rc = -1;
    if (!tool_name || strcmp(tool_name, "request_config") != 0 || !action || !args_json)
        goto done;

    {
        char *agent = session_get_agent_name(db, session_id);
        if (!agent) goto done;

        ToolArgs ta;
        tool_parse(args_json, &ta);
        if (strcmp(action, "grant_tool") == 0) {
            const char *v = targ_str(&ta, "tool");
            if (v) rc = agent_config_grant(db, agent, "tool", v, 0);
        } else if (strcmp(action, "grant_host") == 0) {
            const char *v = targ_str(&ta, "host");
            if (v) rc = agent_config_grant(db, agent, "host", v, 0);
        } else if (strcmp(action, "grant_path") == 0) {
            const char *v = targ_str(&ta, "path");
            const char *m = targ_str(&ta, "mode");
            const char *kind = (m && strcmp(m, "write") == 0) ? "write_path" : "read_path";
            if (v) rc = agent_config_grant(db, agent, kind, v, 0);
        }
        tool_parse_free(&ta);
        free(agent);
    }

    if (rc == 0) {
        const char *isql =
            "INSERT INTO approvals(session_id, tool_name, action, args_json, resolve, state, decided_via)"
            " VALUES(?1,'request_config',?2,?3,'apply','approved','channel:telegram:history_grant');";
        sqlite3_stmt *ins;
        if (sqlite3_prepare_v2(db, isql, -1, &ins, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ins, 1, session_id);
            sqlite3_bind_text(ins, 2, action, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 3, args_json, -1, SQLITE_STATIC);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }
    }

done:
    free(tool_name); free(action); free(args_json);
    return rc;
}
