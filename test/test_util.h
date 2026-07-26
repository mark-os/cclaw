#ifndef CCLAW_TEST_UTIL_H
#define CCLAW_TEST_UTIL_H

#include "db.h"
#include "run_tool.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_web_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* First statement of every test main: line-buffer stdout so progress output
 * survives the Makefile timeout-kill (see AGENTS.md Testing Guidelines). */
#define TEST_INIT() setvbuf(stdout, NULL, _IOLBF, 0)

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

/* Tier-leaf drivers for sandboxed tools: extract args_json into wire params
 * exactly as the dispatching parent does (tool_args_extract on a :memory:
 * db; NULL schema — every top-level key ships, file_edit's edits flatten),
 * build a RunToolParsed, run the tier leaf in-process (host mode, no
 * namespace). Malformed args_json returns the dispatch-gate-equivalent
 * "error: invalid tool arguments". Result is malloc'd; caller frees. */
static inline char *test_run_tier(int tier, const char *tool,
                                  const char *args_json,
                                  const FileReadCtx *fctx,
                                  const WebFetchCtx *wctx) {
    sqlite3 *db = db_open(":memory:");
    if (!db) return test_strdup_("error: test db");
    ToolWireArg *wa = NULL;
    size_t wn = 0;
    int rc = tool_args_extract(db, tool, NULL, args_json, &wa, &wn);
    sqlite3_close(db);
    if (rc != 0) return test_strdup_("error: invalid tool arguments");

    RunToolParsed q;
    memset(&q, 0, sizeof(q));
    q.tier = tier;
    q.tool_name = (char *)tool;
    /* ToolWireArg and struct RunToolParam are field-identical on purpose. */
    q.params = (struct RunToolParam *)wa;
    q.param_count = wn;
    if (fctx) {
        q.workspace = (char *)fctx->workspace;
        q.cwd_path  = (char *)fctx->cwd_path;
        q.sandbox      = fctx->sb.sandbox;
        q.mount_cwd    = fctx->sb.mount_cwd;
        q.read_paths   = fctx->sb.read_paths;
        q.read_count   = fctx->sb.read_path_count;
        q.write_paths  = fctx->sb.write_paths;
        q.write_count  = fctx->sb.write_path_count;
    }
    if (wctx) {
        q.workspace = (char *)wctx->workspace;
        q.cwd_path  = (char *)wctx->cwd_path;
        q.host_rules = wctx->allowed_hosts;
        q.host_count = wctx->allowed_host_count;
        q.sandbox = wctx->host_mode ? 0 : 1;
    }
    char *r = NULL;
    switch (tier) {
    case RUNTOOL_TIER_FILE: r = tool_file_tier_run(&q); break;
    case RUNTOOL_TIER_WEB:  r = tool_web_tier_run(&q); break;
    case RUNTOOL_TIER_JS:   r = tool_js_tier_run(&q); break;
    }
    tool_wire_args_free(wa, wn);
    return r ? r : test_strdup_("error: no tier");
}

static inline char *test_file_tool_run(const char *tool, const char *args_json,
                                       const FileReadCtx *ctx) {
    return test_run_tier(RUNTOOL_TIER_FILE, tool, args_json, ctx, NULL);
}

static inline char *test_web_fetch_run(const char *args_json,
                                       const WebFetchCtx *ctx) {
    return test_run_tier(RUNTOOL_TIER_WEB, "web_fetch", args_json, NULL, ctx);
}

static inline char *test_js_eval_run_json(const char *args_json) {
    return test_run_tier(RUNTOOL_TIER_JS, "js_eval", args_json, NULL, NULL);
}

#endif /* CCLAW_TEST_UTIL_H */
