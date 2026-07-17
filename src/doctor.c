#define _GNU_SOURCE
#include "doctor.h"
#include "cclaw.h"
#include "config.h"
#include "config_registry.h"
#include "db.h"
#include "http.h"
#include "log.h"
#include "types.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

static void print_ok(const char *label, const char *detail) {
    if (detail)
        printf("  ok   %s: %s\n", label, detail);
    else
        printf("  ok   %s\n", label);
}

static void print_fail(const char *label, const char *detail) {
    if (detail)
        printf("  FAIL %s: %s\n", label, detail);
    else
        printf("  FAIL %s\n", label);
}

/* Mask a secret: show first 6 chars + "...(len=N)". Writes into buf. */
static void mask_secret(const char *secret, char *buf, size_t cap) {
    if (!secret || !secret[0]) {
        snprintf(buf, cap, "(not set)");
        return;
    }
    size_t len = strlen(secret);
    size_t show = len < 6 ? len : 6;
    snprintf(buf, cap, "%.*s...(len=%zu)", (int)show, secret, len);
}

/* ── Check 1: Version / Host ────────────────────────────────────── */

static void check_version(void) {
    printf("\n[version]\n");
    printf("  commit: %s\n", VERSION_COMMIT);
    printf("  built:  %s\n", BUILD_DATE);

    struct utsname u;
    if (uname(&u) == 0) {
        char detail[256];
        snprintf(detail, sizeof(detail), "%s %s %s", u.sysname, u.machine, u.release);
        print_ok("host", detail);
    } else {
        print_fail("host", "uname() failed");
    }
}

/* ── Check 2: Database ──────────────────────────────────────────── */

static void check_db(const char *db_path, sqlite3 **out_db) {
    printf("\n[database]\n");
    printf("  path: %s\n", db_path);

    *out_db = NULL;

    struct stat st;
    if (stat(db_path, &st) != 0) {
        print_fail("exists", strerror(errno));
        return;
    }
    char detail[128];
    snprintf(detail, sizeof(detail), "size=%lld bytes", (long long)st.st_size);
    print_ok("exists", detail);

    /* WAL/SHM siblings with sizes — a large -wal means checkpointing lags */
    char wal[1024], shm[1024];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    snprintf(shm, sizeof(shm), "%s-shm", db_path);
    long long wal_sz = (stat(wal, &st) == 0) ? (long long)st.st_size : -1;
    long long shm_sz = (stat(shm, &st) == 0) ? (long long)st.st_size : -1;
    if (wal_sz >= 0 || shm_sz >= 0)
        printf("  wal: %lld bytes, shm: %lld bytes\n", wal_sz, shm_sz);

    /* Try to open */
    sqlite3 *db = db_open(db_path);
    if (!db) {
        print_fail("open", "db_open() returned NULL");
        return;
    }
    print_ok("open", NULL);

    /* Journal mode (expect wal) */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *mode = (const char *)sqlite3_column_text(s, 0);
            printf("  journal_mode: %s\n", mode ? mode : "?");
        }
        sqlite3_finalize(s);
    }

    /* Schema state — read-only report; doctor diagnoses, startup migrates */
    int uv = 0;
    char sv[96];
    switch (db_schema_state(db, &uv)) {
    case DB_SCHEMA_CURRENT:
        snprintf(sv, sizeof(sv), "v%d (current)", uv);
        print_ok("schema", sv);
        break;
    case DB_SCHEMA_FRESH:
        snprintf(sv, sizeof(sv), "empty (initialized at v%d on first run)",
                 CCLAW_SCHEMA_VERSION);
        print_ok("schema", sv);
        break;
    case DB_SCHEMA_UPGRADABLE:
        snprintf(sv, sizeof(sv), "v%d (auto-upgrades to v%d at next startup)",
                 uv, CCLAW_SCHEMA_VERSION);
        print_ok("schema", sv);
        break;
    case DB_SCHEMA_FUTURE:
        snprintf(sv, sizeof(sv), "v%d — from a newer build (this one expects v%d)",
                 uv, CCLAW_SCHEMA_VERSION);
        print_fail("schema", sv);
        db_close(db);
        return;
    case DB_SCHEMA_TOO_OLD:
        snprintf(sv, sizeof(sv), "v%d — below the schema floor, delete the DB", uv);
        print_fail("schema", sv);
        db_close(db);
        return;
    }

    /* Row counts (tables may not exist yet on a fresh DB) */
    static const char *count_tables[] = { "agents", "sessions", "entries" };
    printf(" ");
    for (size_t i = 0; i < sizeof(count_tables) / sizeof(count_tables[0]); i++) {
        char csql[64];
        snprintf(csql, sizeof(csql), "SELECT COUNT(*) FROM %s", count_tables[i]);
        if (sqlite3_prepare_v2(db, csql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW)
                printf(" %s=%lld", count_tables[i],
                       (long long)sqlite3_column_int64(s, 0));
            sqlite3_finalize(s);
        } else {
            printf(" %s=n/a", count_tables[i]);
        }
    }
    printf("\n");

    *out_db = db;
}

/* ── Check: Models ──────────────────────────────────────────────── */

static void check_models(sqlite3 *db) {
    printf("\n[models]\n");
    if (!db) {
        printf("  skipped: no DB handle\n");
        return;
    }

    char *win = config_get(db, "context_window");
    printf("  default context_window: %s\n", win ? win : "(unset)");
    free(win);

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT id, provider_name, context_window, status"
            " FROM models ORDER BY priority", -1, &s, NULL) != SQLITE_OK) {
        printf("  (no models table)\n");
        return;
    }
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *id     = (const char *)sqlite3_column_text(s, 0);
        const char *prov   = (const char *)sqlite3_column_text(s, 1);
        const char *status = (const char *)sqlite3_column_text(s, 3);
        char ctx[24];
        if (sqlite3_column_type(s, 2) == SQLITE_NULL)
            snprintf(ctx, sizeof(ctx), "default");
        else
            snprintf(ctx, sizeof(ctx), "%d", sqlite3_column_int(s, 2));
        printf("  %s: provider=%s ctx=%s status=%s\n",
               id ? id : "?", prov ? prov : "?", ctx, status ? status : "?");
        count++;
    }
    sqlite3_finalize(s);
    if (count == 0)
        printf("  (none — falls back to config provider)\n");
}

/* ── Check 3: Config (redacted) ─────────────────────────────────── */

static void check_config(sqlite3 *db, Config **out_cfg) {
    printf("\n[config]\n");
    *out_cfg = NULL;

    if (!db) {
        print_fail("load", "no DB handle (skipped)");
        return;
    }

    Config *cfg = config_load(db);
    if (!cfg) {
        print_fail("load", "config_load() returned NULL");
        return;
    }
    print_ok("load", NULL);

    /* Provider */
    printf("  provider.base_url: %s\n", cfg->provider.base_url ? cfg->provider.base_url : "(not set)");
    printf("  provider.model:    %s\n", cfg->provider.model ? cfg->provider.model : "(not set)");
    printf("  provider.endpoint: %s\n",
           cfg->provider.endpoint_type == ENDPOINT_GEMINI ? "gemini" : "openai");

    char masked[128];
    mask_secret(cfg->provider.api_key, masked, sizeof(masked));
    printf("  provider.api_key:  %s\n", masked);

    printf("  fallback_count:    %zu\n", cfg->fallback_count);
    printf("  workspace:         %s\n", cfg->workspace ? cfg->workspace : "(not set)");
    printf("  log_level:         %s\n", log_level_name(cfg->log_level));

    *out_cfg = cfg;
}

/* ── Check 4: Provider probe ────────────────────────────────────── */

static void check_provider(const Config *cfg) {
    printf("\n[provider_probe]\n");

    if (!cfg || !cfg->provider.api_key || !cfg->provider.api_key[0]) {
        printf("  skipped: no api_key configured\n");
        return;
    }
    if (!cfg->provider.base_url || !cfg->provider.base_url[0]) {
        print_fail("probe", "no base_url configured");
        return;
    }

    /* Build URL: {base_url}/models */
    char url[1024];
    snprintf(url, sizeof(url), "%s/models", cfg->provider.base_url);

    /* Build auth header */
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", cfg->provider.api_key);
    const char *headers[] = { auth, NULL };

    HttpRequestOpts opts = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .timeout = 5,
    };
    HttpResponse resp = {0};
    int status = http_do(&opts, &resp);

    if (status == -1) {
        char detail[320];
        snprintf(detail, sizeof(detail), "connect error: %s", resp.err_detail);
        print_fail("probe", detail);
    } else if (status == -2) {
        print_fail("probe", "timeout (5s)");
    } else if (status == 200) {
        print_ok("probe", "HTTP 200 — key valid");
    } else if (status == 401) {
        print_fail("probe", "HTTP 401 — bad/expired key");
    } else {
        char detail[64];
        snprintf(detail, sizeof(detail), "HTTP %d", status);
        print_fail("probe", detail);
    }

    http_response_free(&resp);
}

/* ── Check 5: User namespace availability ───────────────────────── */

static void check_userns(void) {
    printf("\n[userns]\n");

    pid_t pid = fork();
    if (pid < 0) {
        print_fail("fork", strerror(errno));
        return;
    }

    if (pid == 0) {
        /* Child: try the exact unshare flags sandbox.c uses */
        int rc = unshare(CLONE_NEWUSER | CLONE_NEWNS);
        _exit(rc == 0 ? 0 : 1);
    }

    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0) {
        print_fail("probe", "waitpid failed");
        return;
    }

    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
        print_ok("unshare", "CLONE_NEWUSER|CLONE_NEWNS available");
    else
        print_fail("unshare", "CLONE_NEWUSER|CLONE_NEWNS not available "
                   "(sandbox will fail-closed)");
}

/* ── Check 6: Workspace ─────────────────────────────────────────── */

static void check_workspace(const Config *cfg) {
    printf("\n[workspace]\n");

    if (!cfg || !cfg->workspace || !cfg->workspace[0]) {
        print_fail("configured", "workspace not set");
        return;
    }
    printf("  path: %s\n", cfg->workspace);

    struct stat st;
    if (stat(cfg->workspace, &st) != 0) {
        print_fail("exists", strerror(errno));
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        print_fail("exists", "not a directory");
        return;
    }
    print_ok("exists", NULL);

    /* Writable check: access() + probe file create/unlink */
    if (access(cfg->workspace, W_OK) != 0) {
        print_fail("writable", strerror(errno));
        return;
    }

    char probe[1024];
    snprintf(probe, sizeof(probe), "%s/.cclaw_doctor_probe", cfg->workspace);
    int fd = open(probe, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        print_fail("writable", "cannot create probe file");
        return;
    }
    close(fd);
    unlink(probe);
    print_ok("writable", NULL);
}

/* ── Check 7: Channels ──────────────────────────────────────────── */

static void check_channels(sqlite3 *db) {
    printf("\n[channels]\n");

    if (!db) {
        printf("  skipped: no DB handle\n");
        return;
    }

    sqlite3_stmt *stmt;
    const char *sql = "SELECT name, type, status, pid FROM channels";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        /* Table might not exist in a fresh/empty DB */
        printf("  (no channels table or empty)\n");
        return;
    }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *type = (const char *)sqlite3_column_text(stmt, 1);
        const char *status = (const char *)sqlite3_column_text(stmt, 2);
        int64_t pid = sqlite3_column_int64(stmt, 3);

        const char *alive = "";
        if (pid > 0) {
            alive = (kill((pid_t)pid, 0) == 0) ? " (alive)" : " (stale)";
        }

        printf("  %s: type=%s status=%s pid=%lld%s\n",
               name ? name : "?",
               type ? type : "?",
               status ? status : "?",
               (long long)pid, alive);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0)
        printf("  (none registered)\n");
}

/* ── Check 8: Denied tool calls (audit trail) ───────────────────── */

/* Gate-refusal markers written to tool_calls.resolved_by. Approval denials
 * are counted from approvals.state='denied' instead — a decided approval
 * stores only its decided_via (cli/telegram/...) in resolved_by, same value
 * for approve and deny. */
#define DENIAL_SET "('not_granted','policy:deny','hook:deny','denied','block_window')"

static void check_denials(sqlite3 *db) {
    printf("\n[audit]\n");
    if (!db) {
        printf("  skipped: no DB handle\n");
        return;
    }

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT resolved_by, COUNT(*) FROM tool_calls"
            " WHERE resolved_by IN " DENIAL_SET
            " AND resolved_at >= unixepoch()-7*86400"
            " GROUP BY resolved_by ORDER BY 2 DESC", -1, &s, NULL) != SQLITE_OK) {
        printf("  (no tool_calls table)\n");
        return;
    }
    printf("  gate denials (7d):");
    int any = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        printf(" %s=%lld", sqlite3_column_text(s, 0),
               (long long)sqlite3_column_int64(s, 1));
        any = 1;
    }
    sqlite3_finalize(s);
    printf(any ? "\n" : " none\n");

    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM approvals WHERE state='denied'"
            " AND requested_at >= unixepoch()-7*86400", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW)
            printf("  approval denials (7d): %lld\n",
                   (long long)sqlite3_column_int64(s, 0));
        sqlite3_finalize(s);
    }

    /* Most recent denials of either kind, agent + tool + how. */
    if (sqlite3_prepare_v2(db,
            "SELECT * FROM ("
            "  SELECT tc.resolved_at AS t, s.agent_name AS agent, tc.name AS tool,"
            "         tc.resolved_by AS via"
            "  FROM tool_calls tc LEFT JOIN sessions s ON s.id=tc.session_id"
            "  WHERE tc.resolved_by IN " DENIAL_SET
            "  UNION ALL"
            "  SELECT a.requested_at, s.agent_name, COALESCE(a.tool_name, a.action),"
            "         'approval:' || COALESCE(a.decided_via,'?')"
            "  FROM approvals a LEFT JOIN sessions s ON s.id=a.session_id"
            "  WHERE a.state='denied'"
            ") ORDER BY t DESC LIMIT 10", -1, &s, NULL) != SQLITE_OK)
        return;
    int shown = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!shown) printf("  recent:\n");
        time_t t = (time_t)sqlite3_column_int64(s, 0);
        char when[32] = "?";
        struct tm tm;
        if (t > 0 && localtime_r(&t, &tm))
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        const char *agent = (const char *)sqlite3_column_text(s, 1);
        const char *tool  = (const char *)sqlite3_column_text(s, 2);
        const char *via   = (const char *)sqlite3_column_text(s, 3);
        printf("    %s  agent=%s  tool=%s  via=%s\n", when,
               agent ? agent : "(default)", tool ? tool : "?", via ? via : "?");
        shown++;
    }
    sqlite3_finalize(s);
    if (!shown) printf("  recent: none\n");
}

/* ── Check 9: Syslog listener ───────────────────────────────────── */

static void check_syslog(void) {
    printf("\n[syslog]\n");

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        print_fail("socket", strerror(errno));
        return;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/dev/log");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        print_ok("listener", "/dev/log accepting connections");
    } else {
        printf("  WARN syslog: no daemon listening on /dev/log (%s)\n", strerror(errno));
        printf("       daemon logs will be lost — install busybox-syslogd or rsyslog\n");
    }

    close(fd);
}

/* ── Main ────────────────────────────────────────────────────────── */

int doctor_main(void) {
    printf("cclaw --doctor\n");

    check_version();

    char *db_path = util_resolve_db_path();
    sqlite3 *db = NULL;
    check_db(db_path, &db);

    Config *cfg = NULL;
    check_config(db, &cfg);
    check_models(db);
    check_provider(cfg);
    check_userns();
    check_workspace(cfg);
    check_channels(db);
    check_denials(db);
    check_syslog();

    printf("\ndone.\n");

    if (cfg) config_free(cfg);
    if (db) db_close(db);
    free(db_path);
    return 0;
}
