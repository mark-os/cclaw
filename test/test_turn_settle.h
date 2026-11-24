/* What the hook suites share (test_turn_settle.c, test_hook_deadline.c): a
 * temp DB with its agents and a model, scripts promoted as real extensions,
 * turns to run them on, and the pump that plays the loop. The including file
 * defines ROOT, a directory of its own (the extension store is <db dir>/
 * extensions), before the include, and its main() doubles as the broker — the
 * children re-exec the test binary. */
#ifndef TEST_TURN_SETTLE_H
#define TEST_TURN_SETTLE_H

#include "advance.h"
#include "agent_setup.h"
#include "child.h"
#include "config.h"
#include "config_registry.h"
#include "db.h"
#include "dispatch.h"
#include "extension_manifest.h"
#include "hook.h"
#include "loop.h"
#include "proc.h"
#include "run_tool.h"
#include "settle.h"
#include "sweep.h"
#include "turn_edit.h"
#include "test_util.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DB_PATH ROOT "/cclaw.db"

static sqlite3 *g_db;

static inline void run(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "FAIL sql: %s\n  %s\n", err ? err : "?", sql);
        abort();
    }
}

static inline void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

/* Promote a one-hook extension owned by (and attached to) `agent`: the
 * hook's event, and any further manifest fields (",\"async\":true"). */
static inline void install_hook(const char *name, const char *agent, const char *event,
                         const char *fields, const char *js) {
    char dir[256], path[320], manifest[640];
    snprintf(dir, sizeof(dir), ROOT "/bundle_%s", name);
    mkdir(dir, 0755);
    snprintf(manifest, sizeof(manifest),
             "{\"name\":\"%s\",\"version\":\"1.0.0\","
             "\"config\":[{\"key\":\"greeting\",\"default\":\"hello\",\"description\":\"a setting the script reads\"}],"
             "\"hooks\":[{\"event\":\"%s\",\"handler\":\"h.qjs\"%s}]}",
             name, event, fields);
    snprintf(path, sizeof(path), "%s/extension.json", dir);
    write_file(path, manifest);
    snprintf(path, sizeof(path), "%s/h.qjs", dir);
    write_file(path, js);
    char *err = NULL;
    if (extension_install(g_db, dir, agent, 0, &err) != 0) {
        fprintf(stderr, "FAIL install %s: %s\n", name, err ? err : "?");
        abort();
    }
    free(err);
}

static inline void install_as(const char *name, const char *agent, int wait, const char *js) {
    install_hook(name, agent, "Stop", wait ? "" : ",\"async\":true", js);
}

static inline void install(const char *name, const char *agent, const char *js) {
    install_as(name, agent, 1, js);
}

/* A turn on session sid: one call of a droppable tool with a `lines`-line
 * result, then — unless `running` — the answer. */
static inline void add_turn(int64_t sid, int lines, int running) {
    Message u = {.role = ROLE_USER, .content = "go"};
    assert(entry_append_with_iteration(g_db, sid, &u, 0) > 0);
    int64_t it = db_next_iteration_id(g_db, sid);
    assert(entry_append_typed(g_db, sid, it, "assistant_message", 0, "looking", NULL,
                              NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
    int64_t call = entry_append_typed(g_db, sid, it, "tool_call", 0, "{}", "c1", "look",
                                      0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(call > 0);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
             " VALUES(%lld,%lld,'c1','look','done');", (long long)sid, (long long)call);
    run(sql);
    size_t cap = (size_t)lines * 48 + 1, len = 0;
    char *body = malloc(cap);
    assert(body);
    for (int i = 1; i <= lines; i++)
        len += (size_t)snprintf(body + len, cap - len,
                                "result line %06d ..........................\n", i);
    assert(entry_append_typed(g_db, sid, it, "tool_result", 0, body, "c1", "look", 0,
                              STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
    free(body);
    if (running) return;
    it = db_next_iteration_id(g_db, sid);
    assert(entry_append_typed(g_db, sid, it, "assistant_message", 0, "done", NULL, NULL,
                              0, STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
}

/* A finished turn on a new top-level session. */
static inline int64_t seed_turn(const char *agent, int lines) {
    int64_t sid = session_create(g_db, "t", agent, -1, 0);
    assert(sid > 0);
    add_turn(sid, lines, 0);
    return sid;
}

static inline int is_state(int64_t sid, const char *want) {
    char *got = db_scalar_text(g_db, "SELECT state FROM sessions WHERE id=?", sid);
    assert(got);
    int same = strcmp(got, want) == 0;
    free(got);
    return same;
}

static inline int64_t count(const char *sql, int64_t sid) {
    return db_scalar_i64(g_db, sql, sid, -1);
}

/* Run the loop's child handling until the session is idle and no child is
 * left. Bounded: a hook that never ends fails the assertion, not the suite. */
static inline void pump_until_idle(int64_t sid) {
    for (int i = 0; i < 240; i++) {
        reap_children();
        child_sweep_deadlines();
        if (is_state(sid, "idle") && child_count() == 0) return;
        usleep(50 * 1000);
    }
    fprintf(stderr, "FAIL: session %lld still held after 12s (children=%d)\n",
            (long long)sid, child_count());
    abort();
}

static inline int fail_count(const char *ext) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db, "SELECT fail_count FROM hooks WHERE extension_name=?1",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, ext, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

#define DROP_C1 "return JSON.stringify({edits:[{op:'drop',call_id:'c1'}," \
                "{op:'note',text:'dropped the lookup'}], record:{by_script:1}});"
#define N_CALLS "SELECT COUNT(*) FROM entries e WHERE e.session_id=?1 AND e.type='tool_call'" \
                " AND e.id IN (WITH RECURSIVE b(id) AS (SELECT leaf_id FROM sessions WHERE id=?1" \
                " UNION ALL SELECT e2.parent_id FROM entries e2 JOIN b ON e2.id=b.id) SELECT id FROM b)"
/* Applied edits: elements of $.turn_edits on the session's entries. */
#define N_EDITS "SELECT COALESCE(SUM(json_array_length(data,'$.turn_edits')),0) FROM entries" \
                " WHERE session_id=?1 AND json_valid(data)"
/* Notes a hook left: system entries whose source names an extension. */
#define N_NOTES "SELECT COUNT(*) FROM entries WHERE session_id=?1 AND type='system'" \
                " AND json_extract(data,'$.source') LIKE 'hook:%'"

static inline double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Everything main() does before the first test, and after the last. */
static AgentSetup g_setup;
static Config *g_cfg;

static inline void fixture_open(void) {
    (void)!system("rm -rf " ROOT);
    mkdir(ROOT, 0755);
    mkdir(ROOT "/ws", 0755);
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    setenv("CCLAW_WORKSPACE", ROOT "/ws", 1);
    setenv("CCLAW_CONTAINMENT", "none", 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);

    g_db = test_db_open(DB_PATH);
    assert(g_db);
    static const char *const agents[] = { "Rv", "Junk", "Boom", "Nil", "Slow", "Slow1",
                                          "Chain", "Obs", "Gone", "Big", "Hist", "Pass",
                                          "Sub", "Ord", "OrdX", "OrdS", "Mid", "MidEdit",
                                          "MidSlow", "Res", "Trip", "CeNone", "CeBoom",
                                          "CeNil", "CeOk", "CeSub" };
    for (size_t i = 0; i < sizeof(agents) / sizeof(agents[0]); i++)
        db_agent_upsert(g_db, agents[i], NULL, NULL);
    run("INSERT INTO tools(name, droppable) VALUES('look',1);"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env)"
        " VALUES('p','http://127.0.0.1:1','openai','K');"
        "INSERT INTO models(id, provider_name, model, context_window)"
        " VALUES('p/m','p','m',1000000);");
    assert(config_set(g_db, "hook_timeout", "20") == 0);
    /* Keep the scripts' scratch out of the shared /tmp tree. */
    assert(config_set(g_db, "tmp_root", ROOT) == 0);

    g_cfg = config_load(g_db);
    assert(g_cfg);
    proc_set_db(g_db);
    proc_set_cfg(g_cfg);
    assert(agent_setup_init(&g_setup, g_db, 0, g_cfg, "Rv") == 0);
    proc_set_tool_setup(&g_setup);
}

static inline void fixture_close(void) {
    agent_setup_destroy(&g_setup);
    config_free(g_cfg);
    db_close(g_db);
    (void)!system("rm -rf " ROOT);
}

#endif
