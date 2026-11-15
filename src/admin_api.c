#define _POSIX_C_SOURCE 200809L
#include "admin_api.h"
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
 * api_key_env defaults to '' in the schema (a keyless gateway leaves it that
 * way), so a later "replace the key" would have nowhere to put the value. Naming the row also lifts it out of the blank
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

static int admin_list_approvals_by_state(sqlite3 *db, const char *channel_name,
                                         const char *state, int limit,
                                         AdminApproval **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!db || !state) return -1;

    const char *sql =
        "SELECT a.id, a.session_id, s.agent_name, a.tool_name, a.park_reason,"
        " a.args_json"
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
        list[count].park_reason = ac ? strdup(ac) : NULL;
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
    return admin_list_approvals_by_state(db, channel_name, "pending", 0, out, out_count);
}

void admin_approvals_free(AdminApproval *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i].agent_name);
        free(list[i].tool_name);
        free(list[i].park_reason);
        free(list[i].args_json);
    }
    free(list);
}

int admin_grant_from_history(sqlite3 *db, int64_t approval_id, const char *decided_via) {
    if (!db) return -1;

    const char *sql = "SELECT session_id, tool_name, args_json FROM approvals WHERE id=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, approval_id);

    int64_t session_id = -1;
    char *tool_name = NULL, *args_json = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        session_id = sqlite3_column_int64(stmt, 0);
        const char *tn = (const char *)sqlite3_column_text(stmt, 1);
        const char *aj = (const char *)sqlite3_column_text(stmt, 2);
        if (tn) tool_name = strdup(tn);
        if (aj) args_json = strdup(aj);
    }
    sqlite3_finalize(stmt);

    int rc = -1;
    /* tool_name alone identifies the document (v47 rekey) — request_config
     * is the only tool whose parked args a grant can be replayed from. */
    if (!tool_name || strcmp(tool_name, "request_config") != 0 || !args_json)
        goto done;

    {
        char *agent = session_get_agent_name(db, session_id);
        if (!agent) goto done;

        /* The decision is recorded first, as its own approved row, so the
         * grants it applies can name it (grants.approval_id) and the change
         * log can label them with it. Not one transaction: the apply may
         * probe a provider over the network, which must not run under a
         * held write lock — so a failed apply removes the row again instead
         * (nothing references it: the apply rolled its own writes back).
         * Same all-or-nothing apply as apply_grant (resolve.c). */
        int64_t new_id = 0;
        const char *isql =
            "INSERT INTO approvals(session_id, tool_name, park_reason, args_json,"
            " resolve, state, decided_via)"
            " VALUES(?1,'request_config','approval_required',?2,"
            " 'apply','approved',?3);";
        sqlite3_stmt *ins;
        if (sqlite3_prepare_v2(db, isql, -1, &ins, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ins, 1, session_id);
            sqlite3_bind_text(ins, 2, args_json, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 3, decided_via ? decided_via : "operator", -1, SQLITE_STATIC);
            if (sqlite3_step(ins) == SQLITE_DONE) new_id = sqlite3_last_insert_rowid(db);
            sqlite3_finalize(ins);
        }
        if (new_id > 0) {
            char label[48], outer[96] = "";
            db_get_context(db, outer, sizeof(outer));
            snprintf(label, sizeof(label), "approval:%lld", (long long)new_id);
            db_set_context(db, label);
            rc = request_config_changes_apply(db, agent, args_json, 0, 0, new_id, NULL);
            db_set_context(db, outer[0] ? outer : NULL);
            if (rc != 0) {
                sqlite3_stmt *del;
                if (sqlite3_prepare_v2(db, "DELETE FROM approvals WHERE id=?1",
                                       -1, &del, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(del, 1, new_id);
                    sqlite3_step(del);
                    sqlite3_finalize(del);
                }
            }
        }
        free(agent);
    }

done:
    free(tool_name); free(args_json);
    return rc;
}
