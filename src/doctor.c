#define _GNU_SOURCE
#include "doctor.h"
#include "cclaw.h"
#include "config.h"
#include "db.h"
#include "http.h"
#include "log.h"
#include "types.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
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

/* resolve_db_path — duplicated from main.c (doctor runs before normal init) */
static char *doctor_resolve_db_path(void) {
    const char *env = getenv("CCLAW_DB_PATH");
    if (env) return strdup(env);
    const char *home = getenv("HOME");
    if (home) {
        size_t len = strlen(home);
        char *p = malloc(len + sizeof("/.cclaw/cclaw.db"));
        if (p) { sprintf(p, "%s/.cclaw/cclaw.db", home); return p; }
    }
    return strdup("cclaw.db");
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

    /* Check for WAL/SHM siblings */
    char wal[1024], shm[1024];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    snprintf(shm, sizeof(shm), "%s-shm", db_path);
    int has_wal = (stat(wal, &st) == 0);
    int has_shm = (stat(shm, &st) == 0);
    if (has_wal || has_shm)
        printf("  note: WAL siblings present (wal=%s shm=%s)\n",
               has_wal ? "yes" : "no", has_shm ? "yes" : "no");

    /* Try to open */
    sqlite3 *db = db_open(db_path);
    if (!db) {
        print_fail("open", "db_open() returned NULL");
        return;
    }
    print_ok("open", NULL);

    /* Schema compat */
    if (db_schema_compat(db)) {
        char sv[64];
        snprintf(sv, sizeof(sv), "schema_version=%d (current)", CCLAW_SCHEMA_VERSION);
        print_ok("schema", sv);
    } else {
        char sv[64];
        snprintf(sv, sizeof(sv), "stale (this build expects v%d)", CCLAW_SCHEMA_VERSION);
        print_fail("schema", sv);
        db_close(db);
        return;
    }

    *out_db = db;
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
           cfg->provider.endpoint_type == ENDPOINT_GEMINI ? "gemini" :
           cfg->provider.endpoint_type == ENDPOINT_RESPONSES ? "responses" :
           cfg->provider.endpoint_type == ENDPOINT_GEMINI_INTERACTIONS ? "gemini_interactions" :
           "openai");

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

/* ── Main ────────────────────────────────────────────────────────── */

int doctor_main(void) {
    printf("cclaw --doctor\n");

    check_version();

    char *db_path = doctor_resolve_db_path();
    sqlite3 *db = NULL;
    check_db(db_path, &db);

    /* Set CCLAW_DB for config_load's default_workspace resolution */
    if (db) setenv("CCLAW_DB", db_path, 1);

    Config *cfg = NULL;
    check_config(db, &cfg);
    check_provider(cfg);
    check_userns();
    check_workspace(cfg);
    check_channels(db);

    printf("\ndone.\n");

    if (cfg) config_free(cfg);
    if (db) db_close(db);
    free(db_path);
    return 0;
}
