#define _POSIX_C_SOURCE 200809L
#include "admin_api.h"
#include "agent_config.h"
#include "db.h"
#include "tool_args.h"
#include "tool_request_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *admin_key_env_name(sqlite3 *db, const char *provider) {
    if (!db || !provider) return NULL;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT api_key_env FROM providers WHERE name=? AND api_key_env != ''",
            -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(s, 1, provider, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(s);
    return out;
}

/* Name the credential slot of a provider row that has none, and persist it.
 * api_key_env defaults to '' in the schema and the dashboard's add-provider
 * form leaves it that way, so "save key" had nowhere to put the value and came
 * back as a generic 400. Naming the row also lifts it out of the blank
 * api_key_env path in llm_proc.c, which must never borrow another provider's
 * credential (see ModelCandidate.use_cfg_key). */
static char *provider_key_env_adopt(sqlite3 *db, const char *provider) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM providers WHERE name=?1 AND COALESCE(api_key_env,'')=''",
            -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(s, 1, provider, -1, SQLITE_STATIC);
    int unnamed = (sqlite3_step(s) == SQLITE_ROW);
    sqlite3_finalize(s);
    if (!unnamed) return NULL;      /* no such row, or already named */

    /* <PROVIDER>_API_KEY, anything not [A-Z0-9] folded to '_' */
    size_t plen = strlen(provider);
    char *var = malloc(plen + 9);
    if (!var) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < plen; i++) {
        unsigned char ch = (unsigned char)provider[i];
        var[j++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32)
                 : ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) ? (char)ch
                 : '_';
    }
    memcpy(var + j, "_API_KEY", 9);

    if (sqlite3_prepare_v2(db, "UPDATE providers SET api_key_env=?1 WHERE name=?2",
                           -1, &s, NULL) != SQLITE_OK) { free(var); return NULL; }
    sqlite3_bind_text(s, 1, var, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, provider, -1, SQLITE_STATIC);
    int ok = (sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) > 0);
    sqlite3_finalize(s);
    if (!ok) { free(var); return NULL; }
    return var;
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
        int rc = db_secret_set(db, var, eq + 1, "operator", "system");
        free(var);
        return rc;
    }

    char *var_name = admin_key_env_name(db, provider);
    if (!var_name) var_name = provider_key_env_adopt(db, provider);
    if (!var_name) return -1;
    int rc = db_secret_set(db, var_name, value, "operator", "system");
    free(var_name);
    return rc;
}

/* Both setters address a provider by name, like every other provider action on
 * the dashboard (set_key, add_provider, remove_provider, add_model).
 *
 * They used to take a priority *index* ("0 = primary"), which stopped being
 * true once config_load grew its key-availability scan: the effective primary
 * is the highest-priority provider whose key resolves, so on a box where the
 * priority-0 row has no key, index 0 edited a provider that nothing routes to.
 * An index also silently re-targets whenever a row is added or re-prioritised. */
int admin_set_model(sqlite3 *db, const char *provider, const char *model) {
    if (!db || !provider || !model) return -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "UPDATE providers SET default_model=?1 WHERE name=?2", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, model, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, provider, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int admin_set_endpoint(sqlite3 *db, const char *provider, const char *url) {
    if (!db || !provider || !url) return -1;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "UPDATE providers SET base_url=?1 WHERE name=?2", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, provider, -1, SQLITE_STATIC);
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

int admin_list_models(sqlite3 *db, AdminModel **out, size_t *out_count) {
    if (!db || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    const char *sql =
        "SELECT m.id, m.model, m.provider_name, p.base_url, p.api_key_env,"
        "       m.status, COALESCE(m.context_window, 0),"
        "       MAX(0, COALESCE(m.degraded_until, 0) - unixepoch()),"
        "       COALESCE(m.total_requests, 0),"
        "       COALESCE(m.error_count_5xx, 0), COALESCE(m.error_count_429, 0)"
        " FROM models m JOIN providers p ON m.provider_name = p.name"
        " ORDER BY m.priority;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    size_t cap = 4, count = 0;
    AdminModel *list = calloc(cap, sizeof(AdminModel));
    if (!list) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            AdminModel *tmp = realloc(list, cap * sizeof(AdminModel));
            if (!tmp) {
                admin_models_free(list, count);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }
        AdminModel *m = &list[count];
        memset(m, 0, sizeof(*m));
        const char *v;
        v = (const char *)sqlite3_column_text(stmt, 0); m->id = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 1); m->model = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 2); m->provider = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 3); m->base_url = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 4); m->api_key_env = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 5); m->status = v ? strdup(v) : NULL;
        m->context_window = sqlite3_column_int(stmt, 6);
        m->degraded_left = sqlite3_column_int(stmt, 7);
        m->total_requests = sqlite3_column_int64(stmt, 8);
        m->err_5xx = sqlite3_column_int64(stmt, 9);
        m->err_429 = sqlite3_column_int64(stmt, 10);

        /* Key presence: env first, encrypted kv second — same resolution
         * order llm_req uses at request time. */
        if (m->api_key_env && m->api_key_env[0]) {
            const char *env = getenv(m->api_key_env);
            if (env && env[0]) {
                m->has_key = 1;
            } else {
                char *sec = db_secret_get_system(db, m->api_key_env);
                if (sec) { m->has_key = 1; free(sec); }
            }
        }
        count++;
    }
    sqlite3_finalize(stmt);

    *out = list;
    *out_count = count;
    return 0;
}

void admin_models_free(AdminModel *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i].id);
        free(list[i].model);
        free(list[i].provider);
        free(list[i].base_url);
        free(list[i].api_key_env);
        free(list[i].status);
    }
    free(list);
}

int admin_switch_model(sqlite3 *db, const char *model_id, char *prev, size_t prev_sz) {
    if (!db || !model_id) return -1;
    if (prev && prev_sz) prev[0] = '\0';

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM models WHERE id=?1", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, model_id, -1, SQLITE_STATIC);
    int found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (!found) return -1;

    /* Previous routing head — becomes the first fallback */
    char prev_id[128] = "";
    if (sqlite3_prepare_v2(db,
            "SELECT id FROM models WHERE id != ?1 AND status != 'disabled'"
            " ORDER BY priority LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, model_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (v) snprintf(prev_id, sizeof(prev_id), "%s", v);
        }
        sqlite3_finalize(stmt);
    }
    if (prev && prev_sz) snprintf(prev, prev_sz, "%s", prev_id);

    /* Shift everyone down one, put the chosen model at the head with fresh
     * health — stale degradation must not sideline an explicit switch. */
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    sqlite3_exec(db, "UPDATE models SET priority = priority + 1;", NULL, NULL, NULL);
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "UPDATE models SET priority=0, status='healthy', degraded_until=NULL,"
            " error_count_5xx=0, error_count_429=0 WHERE id=?1;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, model_id, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(stmt);
    }
    /* Context-window resolution (agents.primary_model → models) follows the switch:
     * repoint agents that tracked the old head or had no explicit preference. */
    if (rc == 0 && sqlite3_prepare_v2(db,
            "UPDATE agents SET primary_model=?1 WHERE primary_model IS NULL OR primary_model=?2"
            " OR primary_model=(SELECT model FROM models WHERE id=?2);",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, model_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, prev_id, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return rc;
}

static int admin_list_approvals_by_state(sqlite3 *db, const char *channel_name,
                                         const char *state, int grantable_only,
                                         int limit, AdminApproval **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!db || !state) return -1;

    /* grantable_only restricts to request_config approvals — the only ones
     * admin_grant_from_history can actually apply (the request_changes
     * document). Used for the denial-history/"Grant now" listing; pending
     * approvals (any tool) are never filtered this way. */
    const char *sql = grantable_only ?
        "SELECT a.id, a.session_id, s.agent_name, a.tool_name, a.action, a.args_json"
        " FROM approvals a JOIN sessions s ON s.id = a.session_id"
        " WHERE a.state=?1 AND (?2 IS NULL OR s.channel_name=?2)"
        " AND a.tool_name='request_config'"
        " ORDER BY a.id DESC LIMIT ?3;" :
        "SELECT a.id, a.session_id, s.agent_name, a.tool_name, a.action, a.args_json"
        " FROM approvals a JOIN sessions s ON s.id = a.session_id"
        " WHERE a.state=?1 AND (?2 IS NULL OR s.channel_name=?2)"
        " ORDER BY a.id DESC LIMIT ?3;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, state, -1, SQLITE_STATIC);
    if (channel_name) sqlite3_bind_text(stmt, 2, channel_name, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 2);
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

/* ── Provider CRUD ─────────────────────────────────────────────── */

int admin_add_provider(sqlite3 *db, const char *name, const char *base_url,
                       const char *endpoint_type, const char *api_key_env) {
    if (!db || !name || !name[0] || !base_url || !base_url[0]) return -1;
    const char *sql =
        "INSERT OR IGNORE INTO providers(name, base_url, endpoint_type, api_key_env, priority)"
        " VALUES(?1, ?2, ?3, ?4, (SELECT COALESCE(MAX(priority),0)+1 FROM providers));";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, base_url, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, endpoint_type ? endpoint_type : "openai", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, api_key_env ? api_key_env : "", -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int admin_remove_provider(sqlite3 *db, const char *name) {
    if (!db || !name) return -1;
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    sqlite3_stmt *stmt;
    int rc = -1;
    if (sqlite3_prepare_v2(db, "DELETE FROM models WHERE provider_name=?1",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM providers WHERE name=?1",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return rc;
}

/* ── Model CRUD ────────────────────────────────────────────────── */

int admin_add_model(sqlite3 *db, const char *provider_name, const char *model,
                    int context_window) {
    if (!db || !provider_name || !model || !model[0]) return -1;
    /* id = "model@provider" */
    size_t id_len = strlen(model) + 1 + strlen(provider_name) + 1;
    char *id = malloc(id_len);
    if (!id) return -1;
    snprintf(id, id_len, "%s@%s", model, provider_name);

    const char *sql =
        "INSERT OR IGNORE INTO models(id, provider_name, model, context_window, priority)"
        " VALUES(?1, ?2, ?3, ?4, (SELECT COALESCE(MAX(priority),0)+1 FROM models));";
    sqlite3_stmt *stmt;
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, provider_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, model, -1, SQLITE_STATIC);
        if (context_window > 0)
            sqlite3_bind_int(stmt, 4, context_window);
        else
            sqlite3_bind_null(stmt, 4);
        rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
        sqlite3_finalize(stmt);
    }
    free(id);
    return rc;
}

int admin_remove_model(sqlite3 *db, const char *model_id) {
    if (!db || !model_id) return -1;
    const char *sql = "DELETE FROM models WHERE id=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, model_id, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int admin_toggle_model(sqlite3 *db, const char *model_id) {
    if (!db || !model_id) return -1;
    const char *sql =
        "UPDATE models SET status = CASE WHEN status='disabled' THEN 'healthy' ELSE 'disabled' END"
        " WHERE id=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, model_id, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

/* ── Agent listing ─────────────────────────────────────────────── */

int admin_list_agents(sqlite3 *db, AdminAgent **out, size_t *out_count) {
    if (!db || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    const char *sql = "SELECT name, primary_model, secondary_model, max_iterations, sandbox_profile FROM agents ORDER BY name;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    size_t cap = 4, count = 0;
    AdminAgent *list = calloc(cap, sizeof(AdminAgent));
    if (!list) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            AdminAgent *tmp = realloc(list, cap * sizeof(AdminAgent));
            if (!tmp) { admin_agents_free(list, count); sqlite3_finalize(stmt); return -1; }
            list = tmp;
        }
        const char *n = (const char *)sqlite3_column_text(stmt, 0);
        const char *pm = (const char *)sqlite3_column_text(stmt, 1);
        const char *sm = (const char *)sqlite3_column_text(stmt, 2);
        const char *sp = (const char *)sqlite3_column_text(stmt, 4);
        list[count].name = n ? strdup(n) : NULL;
        list[count].primary_model = pm ? strdup(pm) : NULL;
        list[count].secondary_model = sm ? strdup(sm) : NULL;
        list[count].max_iterations = sqlite3_column_int(stmt, 3);
        list[count].sandbox_profile = sp ? strdup(sp) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);

    *out = list;
    *out_count = count;
    return 0;
}

void admin_agents_free(AdminAgent *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i].name);
        free(list[i].primary_model);
        free(list[i].secondary_model);
        free(list[i].sandbox_profile);
    }
    free(list);
}

int admin_set_agent_model(sqlite3 *db, const char *agent_name,
                          const char *primary_model, const char *secondary_model) {
    if (!db || !agent_name) return -1;
    const char *sql = "UPDATE agents SET primary_model=?2, secondary_model=?3 WHERE name=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    if (primary_model && primary_model[0])
        sqlite3_bind_text(stmt, 2, primary_model, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 2);
    if (secondary_model && secondary_model[0])
        sqlite3_bind_text(stmt, 3, secondary_model, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int64_t admin_create_session(sqlite3 *db, const char *agent_name) {
    if (!db || !agent_name) return -1;
    return session_create(db, NULL, agent_name, -1, 0);
}

/* ── Session listing ───────────────────────────────────────────── */

int admin_list_sessions(sqlite3 *db, int limit, AdminSession **out, size_t *out_count) {
    if (!db || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    const char *sql =
        "SELECT id, agent_name, channel_name, chat_id, state,"
        "       datetime(created_at, 'unixepoch')"
        " FROM sessions ORDER BY id DESC LIMIT ?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, limit > 0 ? limit : 50);

    size_t cap = 8, count = 0;
    AdminSession *list = calloc(cap, sizeof(AdminSession));
    if (!list) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            AdminSession *tmp = realloc(list, cap * sizeof(AdminSession));
            if (!tmp) { admin_sessions_free(list, count); sqlite3_finalize(stmt); return -1; }
            list = tmp;
        }
        const char *an = (const char *)sqlite3_column_text(stmt, 1);
        const char *cn = (const char *)sqlite3_column_text(stmt, 2);
        const char *ci = (const char *)sqlite3_column_text(stmt, 3);
        const char *st = (const char *)sqlite3_column_text(stmt, 4);
        const char *ca = (const char *)sqlite3_column_text(stmt, 5);
        list[count].id = sqlite3_column_int64(stmt, 0);
        list[count].agent_name = an ? strdup(an) : NULL;
        list[count].channel_name = cn ? strdup(cn) : NULL;
        list[count].chat_id = ci ? strdup(ci) : NULL;
        list[count].state = st ? strdup(st) : NULL;
        list[count].created_at = ca ? strdup(ca) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);

    *out = list;
    *out_count = count;
    return 0;
}

void admin_sessions_free(AdminSession *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i].agent_name);
        free(list[i].channel_name);
        free(list[i].chat_id);
        free(list[i].state);
        free(list[i].created_at);
    }
    free(list);
}

int admin_attach_session_channel(sqlite3 *db, int64_t session_id,
                                 const char *channel_name, const char *chat_id) {
    /* A pin needs a real chat — channel-wide defaults are
     * channels.default_agent (admin_set_channel_route), not a route. */
    if (!db || !channel_name || session_id <= 0) return -1;
    if (!chat_id || !chat_id[0]) return -1;

    const char *sql =
        "INSERT INTO channel_routes(channel_name, chat_id, session_id)"
        " VALUES(?1, ?2, ?3)"
        " ON CONFLICT(channel_name, chat_id)"
        " DO UPDATE SET session_id=excluded.session_id;";
    sqlite3_stmt *stmt;
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, channel_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, chat_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, session_id);
        rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(stmt);
    }

    /* Also update the session itself */
    if (rc == 0) {
        const char *usql =
            "UPDATE sessions SET channel_name=?1, chat_id=?2 WHERE id=?3;";
        if (sqlite3_prepare_v2(db, usql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, channel_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, chat_id, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 3, session_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return rc;
}

/* ── Channel listing ───────────────────────────────────────────── */

int admin_list_channels(sqlite3 *db, AdminChannel **out, size_t *out_count) {
    if (!db || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    const char *sql =
        "SELECT c.name, c.extension_name, c.type, c.status,"
        "       c.default_agent, NULL"
        " FROM channels c"
        " ORDER BY c.name;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    size_t cap = 4, count = 0;
    AdminChannel *list = calloc(cap, sizeof(AdminChannel));
    if (!list) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            AdminChannel *tmp = realloc(list, cap * sizeof(AdminChannel));
            if (!tmp) { admin_channels_free(list, count); sqlite3_finalize(stmt); return -1; }
            list = tmp;
        }
        const char *n = (const char *)sqlite3_column_text(stmt, 0);
        const char *en = (const char *)sqlite3_column_text(stmt, 1);
        const char *t = (const char *)sqlite3_column_text(stmt, 2);
        const char *s = (const char *)sqlite3_column_text(stmt, 3);
        const char *ra = (const char *)sqlite3_column_text(stmt, 4);
        list[count].name = n ? strdup(n) : NULL;
        list[count].extension_name = en ? strdup(en) : NULL;
        list[count].type = t ? strdup(t) : NULL;
        list[count].status = s ? strdup(s) : NULL;
        list[count].route_agent = ra ? strdup(ra) : NULL;
        list[count].route_session = sqlite3_column_type(stmt, 5) != SQLITE_NULL
                                    ? sqlite3_column_int64(stmt, 5) : 0;
        count++;
    }
    sqlite3_finalize(stmt);

    *out = list;
    *out_count = count;
    return 0;
}

void admin_channels_free(AdminChannel *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i].name);
        free(list[i].extension_name);
        free(list[i].type);
        free(list[i].status);
        free(list[i].route_agent);
    }
    free(list);
}

/* Channel-wide default agent (open-door policy) — channels.default_agent,
 * not a route: routes pin sessions, this decides who serves new chats. */
int admin_set_channel_route(sqlite3 *db, const char *channel_name,
                            const char *agent_name) {
    if (!db || !channel_name) return -1;
    const char *sql = "UPDATE channels SET default_agent=?2 WHERE name=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, channel_name, -1, SQLITE_STATIC);
    if (agent_name && agent_name[0])
        sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 2);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0)
             ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
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

        /* Same all-or-nothing apply as apply_grant (main.c) — one code path
         * for the document, no drift between the two grant routes. */
        if (strcmp(action, "request_changes") == 0)
            rc = request_config_changes_apply(db, agent, args_json, 0, NULL);
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
