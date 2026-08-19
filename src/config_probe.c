/* config_probe.c — probe-at-apply for routing-changing config documents.
 * See config_probe.h for the contract and why it exists. */
#define _POSIX_C_SOURCE 200809L
#include "config_probe.h"

#include "llm_proc.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One-row, one-column query over (args_json, agent). Result strdup'd. */
static char *q_json(sqlite3 *db, const char *sql, const char *p1, const char *p2) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(s, 1, p1, -1, SQLITE_STATIC);
    if (p2) sqlite3_bind_text(s, 2, p2, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(s);
    return out;
}

/* Run one statement bound as (?1, ?2). Returns 0 on SQLITE_DONE. */
static int exec2(sqlite3 *db, const char *sql, const char *p1, const char *p2) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, p1, -1, SQLITE_STATIC);
    if (p2) sqlite3_bind_text(s, 2, p2, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(s);
    return rc;
}

char *config_probe_snapshot(sqlite3 *db, const char *agent, const char *args_json) {
    if (!db || !agent || !args_json) return NULL;
    /* Only the rows this document can move. A wider snapshot would restore
     * state the operator changed by other means in the meantime. */
    return q_json(db,
        "SELECT json_object("
        " 'agent', (SELECT json_object('models', COALESCE((SELECT"
        "               json_group_array(model_id) FROM (SELECT model_id"
        "               FROM agent_models WHERE agent_name=?2 ORDER BY pos)),"
        "               json_array()))"
        "             FROM agents WHERE name=?2),"
        " 'provider', (SELECT json_object('name', name, 'base_url', base_url,"
        "                                 'api_key_env', api_key_env)"
        "                FROM providers"
        "               WHERE name = json_extract(?1,'$.changes.provider.provider')),"
        " 'models', (SELECT json_group_array(json_object("
        "                 'id', id, 'provider_name', provider_name, 'model', model,"
        "                 'context_window', context_window,"
        "                 'max_output_tokens', max_output_tokens,"
        "                 'capabilities', capabilities,"
        "                 'status', status))"
        "              FROM models WHERE id IN (SELECT json_extract(value,'$.id')"
        "                FROM json_each(?1,'$.changes.models'))))",
        args_json, agent);
}

int config_probe_needed(sqlite3 *db, const char *agent, const char *args_json) {
    if (!db || !agent || !args_json) return 0;
    /* "Affects which model serves requests" — read against the state the
     * document just wrote: the agent's routing list moved, a model row on
     * that list was touched, or the provider behind one of them changed
     * transport. A grants/config-value document matches none of these. */
    char *v = q_json(db,
        "SELECT 1 FROM agents a WHERE a.name=?2 AND ("
        "   json_extract(?1,'$.changes.agent.models') IS NOT NULL"
        "   OR EXISTS(SELECT 1 FROM json_each(?1,'$.changes.models') j"
        "              WHERE json_extract(j.value,'$.id') IN"
        "                (SELECT model_id FROM agent_models WHERE agent_name=?2))"
        "   OR EXISTS(SELECT 1 FROM models m"
        "              WHERE m.id IN (SELECT model_id FROM agent_models"
        "                             WHERE agent_name=?2)"
        "                AND m.provider_name ="
        "                    json_extract(?1,'$.changes.provider.provider')))",
        args_json, agent);
    int needed = v != NULL;
    free(v);
    return needed;
}

int config_probe_restore(sqlite3 *db, const char *agent, const char *args_json,
                         const char *snap_json) {
    if (!db || !agent || !args_json || !snap_json) return -1;
    int rc = 0;
    sqlite3_exec(db, "SAVEPOINT probe_revert", NULL, NULL, NULL);

    /* Agent routing list: whole-list restore from the snapshot array. */
    if (rc == 0)
        rc = exec2(db,
            "DELETE FROM agent_models WHERE agent_name=?2"
            " AND json_type(?1,'$.agent.models')='array'",
            snap_json, agent);
    if (rc == 0)
        rc = exec2(db,
            "INSERT INTO agent_models(agent_name, model_id, pos)"
            " SELECT ?2, value, key FROM json_each(?1,'$.agent.models')",
            snap_json, agent);

    /* Provider: restored if it existed, dropped if the document created it. */
    if (rc == 0)
        rc = exec2(db,
            "UPDATE providers SET base_url = json_extract(?1,'$.provider.base_url'),"
            " api_key_env = json_extract(?1,'$.provider.api_key_env')"
            " WHERE name = json_extract(?1,'$.provider.name')",
            snap_json, NULL);
    if (rc == 0)
        rc = exec2(db,
            "DELETE FROM providers WHERE name = json_extract(?2,'$.changes.provider.provider')"
            " AND json_extract(?1,'$.provider.name') IS NULL",
            snap_json, args_json);

    /* Models: same shape — restore what was there, delete what was created.
     * Per-column subselects mirror the apply path's write. */
    if (rc == 0)
        rc = exec2(db,
            "UPDATE models SET"
            "  context_window = (SELECT json_extract(j.value,'$.context_window')"
            "     FROM json_each(?1,'$.models') j WHERE json_extract(j.value,'$.id')=models.id),"
            "  max_output_tokens = (SELECT json_extract(j.value,'$.max_output_tokens')"
            "     FROM json_each(?1,'$.models') j WHERE json_extract(j.value,'$.id')=models.id),"
            "  capabilities = (SELECT json_extract(j.value,'$.capabilities')"
            "     FROM json_each(?1,'$.models') j WHERE json_extract(j.value,'$.id')=models.id),"
            "  status = (SELECT json_extract(j.value,'$.status')"
            "     FROM json_each(?1,'$.models') j WHERE json_extract(j.value,'$.id')=models.id)"
            " WHERE id IN (SELECT json_extract(value,'$.id') FROM json_each(?1,'$.models'))",
            snap_json, NULL);
    if (rc == 0)
        rc = exec2(db,
            "DELETE FROM models WHERE id IN"
            "  (SELECT json_extract(value,'$.id') FROM json_each(?2,'$.changes.models'))"
            " AND id NOT IN (SELECT json_extract(value,'$.id') FROM json_each(?1,'$.models'))",
            snap_json, args_json);

    if (rc == 0) sqlite3_exec(db, "RELEASE probe_revert", NULL, NULL, NULL);
    else sqlite3_exec(db, "ROLLBACK TO probe_revert; RELEASE probe_revert",
                      NULL, NULL, NULL);
    return rc;
}

/* Human-readable "what you are back on", read from the snapshot. */
static char *restored_text(sqlite3 *db, const char *snap_json) {
    char *t = q_json(db,
        "SELECT 'models='"
        "  ||COALESCE(NULLIF((SELECT group_concat(value,' > ')"
        "      FROM json_each(?1,'$.agent.models')),''),'(default)')"
        "  ||CASE WHEN json_extract(?1,'$.provider.name') IS NOT NULL"
        "     THEN ', provider '||json_extract(?1,'$.provider.name')||' -> '"
        "          ||json_extract(?1,'$.provider.base_url')"
        "     ELSE '' END",
        snap_json, NULL);
    return t ? t : strdup("the previous configuration");
}

int config_probe_verify(sqlite3 *db, const char *agent, const char *args_json,
                        const char *snap_json, int64_t session_id, char **verdict) {
    if (verdict) *verdict = NULL;
    if (!db || !agent || !args_json || session_id <= 0) return 0;
    if (!config_probe_needed(db, agent, args_json)) return 0;

    char served[128] = "", reason[256] = "";
    if (llm_probe_agent(db, agent, session_id, served, sizeof(served),
                        reason, sizeof(reason)) == 0) {
        if (verdict) {
            char buf[256];
            snprintf(buf, sizeof(buf), "probed OK — %s served the test request",
                     served[0] ? served : "the new model");
            *verdict = strdup(buf);
        }
        return 0;
    }

    char *back = restored_text(db, snap_json ? snap_json : "{}");
    int restored = snap_json ? config_probe_restore(db, agent, args_json, snap_json) : -1;
    LOG_WARN_("config probe failed (agent=%s): %s — %s", agent, reason,
              restored == 0 ? "reverted" : "REVERT FAILED");
    if (verdict) {
        char buf[512];
        if (restored == 0)
            snprintf(buf, sizeof(buf), "probe failed: %s — reverted to %s", reason, back);
        else
            snprintf(buf, sizeof(buf),
                     "probe failed: %s — AND the revert failed; routing may be "
                     "broken, check the models/providers rows", reason);
        *verdict = strdup(buf);
    }
    free(back);
    return -1;
}
