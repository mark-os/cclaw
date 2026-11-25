#ifndef CCLAW_TEST_UTIL_H
#define CCLAW_TEST_UTIL_H

#include "db.h"
#include "run_tool.h"
#include "tool_file.h"
#include "tool_js.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Plans the shipped rows get on a host with namespaces, for tests that
 * build a --run-tool request by hand (the limits are the tests' own). */
#define TEST_PLAN_STANDARD ((ContainPlan){ {FILES_PRIVATE, NET_FIREWALLED, 64, 60}, FE_MOUNTNS, NE_NETNS, 0 })
#define TEST_PLAN_OFFLINE  ((ContainPlan){ {FILES_PRIVATE, NET_NONE, 64, 60}, FE_MOUNTNS, NE_NETNS, 0 })
#define TEST_PLAN_NONE     ((ContainPlan){ {FILES_OPEN, NET_DIRECT, 0, 0}, FE_NONE, NE_NONE, 0 })

/* First statement of every test main: line-buffer stdout so progress output
 * survives the Makefile timeout-kill (see AGENTS.md Testing Guidelines). */
#define TEST_INIT() setvbuf(stdout, NULL, _IOLBF, 0)

/* Run one test function and name it. A test reports failure by assert (the
 * abort names the file and line), so reaching the next line is the pass:
 * no per-test PASS printf, no counters. */
#define RUN(fn) do { printf("  %s\n", #fn); fn(); } while (0)

/* One-value reads for assertions, over SQL with no parameters. */
#define test_qi(db, sql) db_scalar_i64((db), (sql), 0, -1)
#define test_qs(db, sql) db_scalar_text((db), (sql), 0)   /* caller frees */

/* Remove a test DB and all its derived artifacts: <base>, <base>-wal,
 * <base>-shm. ENOENT is fine — stale debris from a killed run must not
 * survive into the next one. */
static inline void test_db_clean(const char *base) {
    char sib[512];
    unlink(base);
    snprintf(sib, sizeof(sib), "%s-wal", base);
    unlink(sib);
    snprintf(sib, sizeof(sib), "%s-shm", base);
    unlink(sib);
}

/* strdup without feature-macro dependence (tests include libc headers in
 * arbitrary order). */
static inline char *test_strdup_(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Convenience: open + ensure schema. */
static inline sqlite3 *test_db_open(const char *path) {
    sqlite3 *db = db_open(path);
    if (db) db_ensure_schema(db);
    return db;
}

/* Open + schema + seed.sql (matches main.c startup). For tests exercising
 * provider-table-driven lookups (admin key mapping, request_config provider
 * defaults). NOT the default: seeded models rows would make llm routing
 * target real provider URLs instead of the mock server. */
static inline sqlite3 *test_db_open_seeded(const char *path) {
    sqlite3 *db = test_db_open(path);
    if (db) db_seed_defaults(db);
    return db;
}

/* agent_name columns are enforced FKs (v31) — a test seeding rows for an
 * agent must create the parent row first, like every runtime path does. */
static inline void test_seed_agent(sqlite3 *db, const char *name) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO agents(name) VALUES(?)",
                           -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* sessions/cron_jobs (channel_name, chat_id) is a declared link to a
 * channel_chats row, which itself needs its channels row (v61) — a test that
 * stamps a chat creates both first, the way the gate's first contact does.
 * Unrouted, with the column defaults. */
static inline void test_seed_chat(sqlite3 *db, const char *channel, const char *chat_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO channels(name, status) VALUES(?1, 'active');",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, channel, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO channel_chats(channel_name, chat_id) VALUES(?1, ?2);",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, channel, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, chat_id, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

/* Append a model at the tail of an agent's routing list (creating the
 * models/providers rows is the caller's business). */
static inline void test_agent_add_model(sqlite3 *db, const char *agent,
                                        const char *model_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO agent_models(agent_name, model_id, pos)"
            " VALUES(?1, ?2, COALESCE((SELECT MAX(pos)+1 FROM agent_models"
            "                          WHERE agent_name=?1 AND purpose='chat'), 0))",
            -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* A summary in the shape the deleted C compaction left in existing
 * databases (the shape a test of how such a DB is read needs): a compaction
 * entry hung off last_kept_id, first_after_id re-pointed at it with its
 * original parent remembered. The leaf does not move (entries_leaf_ai skips
 * compaction rows). Returns the summary's id, or -1. */
static inline int64_t test_legacy_compact(sqlite3 *db, int64_t session_id,
                                          int64_t last_kept_id, int64_t first_after_id,
                                          const char *summary) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO entries (parent_id, session_id, type, role, content, stop_reason,"
            " token_estimate, content_bytes, tool_call_count)"
            " VALUES (?1, ?2, 'compaction', 4, ?3, 0, " SQL_TOKEN_ESTIMATE("octet_length(?3)")
            ", octet_length(?3), 0);",
            -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(s, 1, last_kept_id);
    sqlite3_bind_int64(s, 2, session_id);
    sqlite3_bind_text(s, 3, summary, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return -1;
    int64_t id = sqlite3_last_insert_rowid(db);
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET original_parent_id = parent_id, parent_id = ?1"
            " WHERE id = ?2 AND session_id = ?3;", -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_int64(s, 2, first_after_id);
    sqlite3_bind_int64(s, 3, session_id);
    rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? id : -1;
}

/* Child-leaf drivers for sandboxed tools: extract args_json into wire params
 * exactly as the dispatching parent does (tool_args_extract on a :memory:
 * db; NULL schema — every top-level key ships, file_edit's edits flatten),
 * build a RunToolParsed, run the child leaf in-process (uncontained, no
 * namespace). Malformed args_json returns the dispatch-gate-equivalent
 * "error: invalid tool arguments". Result is malloc'd; caller frees. */
static inline char *test_run_child_status(ExecVehicle vehicle, const char *tool,
                                          const char *args_json,
                                          const FileReadCtx *fctx,
                                          int *is_error) {
    int sink = 0;
    if (!is_error) is_error = &sink;
    *is_error = 0;
    sqlite3 *db = db_open(":memory:");
    if (!db) { *is_error = 1; return test_strdup_("error: test db"); }
    ToolWireArg *wa = NULL;
    size_t wn = 0;
    int rc = tool_args_extract(db, tool, NULL, args_json, &wa, &wn);
    sqlite3_close(db);
    if (rc != 0) { *is_error = 1; return test_strdup_("error: invalid tool arguments"); }

    RunToolParsed q;
    memset(&q, 0, sizeof(q));
    q.vehicle = vehicle;
    q.tool_name = (char *)tool;
    /* ToolWireArg and struct RunToolParam are field-identical on purpose. */
    q.params = (struct RunToolParam *)wa;
    q.param_count = wn;
    if (fctx) {
        q.workspace = (char *)fctx->workspace;
        q.cwd_path  = (char *)fctx->cwd_path;
        q.plan.fe      = fctx->fe;
        q.read_paths   = fctx->sb.read_paths;
        q.read_count   = fctx->sb.read_path_count;
        q.write_paths  = fctx->sb.write_paths;
        q.write_count  = fctx->sb.write_path_count;
    }
    char *r = NULL;
    if (vehicle == EXEC_FILE)    r = tool_file_child_run(&q, is_error);
    else if (vehicle == EXEC_JS) r = tool_js_child_run(&q, is_error);
    tool_wire_args_free(wa, wn);
    if (!r) { *is_error = 1; r = test_strdup_("error: no such child vehicle"); }
    return r;
}

/* Status-agnostic form for tests that only look at the text. */
static inline char *test_run_child(ExecVehicle vehicle, const char *tool,
                                   const char *args_json,
                                   const FileReadCtx *fctx) {
    return test_run_child_status(vehicle, tool, args_json, fctx, NULL);
}

static inline char *test_file_tool_run(const char *tool, const char *args_json,
                                       const FileReadCtx *ctx) {
    return test_run_child(EXEC_FILE, tool, args_json, ctx);
}

static inline char *test_js_eval_run_json(const char *args_json) {
    return test_run_child(EXEC_JS, "js_eval", args_json, NULL);
}

#endif /* CCLAW_TEST_UTIL_H */
