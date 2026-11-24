#ifndef CCLAW_TEST_INTEG_H
#define CCLAW_TEST_INTEG_H

/* Harness for integration suites that drive the real build/cclaw against a
 * mock provider: the child's environment, spawning and reaping it, seeding
 * the providers/models rows, and one-shot reads through a fresh connection
 * (a WAL reader must reopen to see another process's writes). */

#include "db.h"
#include "test_util.h"
#include <fcntl.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static inline void integ_nap(void) {   /* 100 ms */
    struct timespec ts = {0, 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
}

typedef struct {
    const char *db;          /* CCLAW_DB_PATH */
    const char *url;         /* mock OpenAI-compatible base URL; NULL: only
                              * the providers row may pick the wire format
                              * (the Anthropic suites) */
    const char *workspace;   /* CCLAW_WORKSPACE, when set */
    const char *home;        /* HOME, when set */
    const char *containment; /* CCLAW_CONTAINMENT; NULL unsets it */
    const char *max_iter;    /* CCLAW_MAX_ITERATIONS, when set */
    int verbose;             /* pass -v to -p runs that resume a session */
} IntegEnv;

static inline void integ_child_env(const IntegEnv *e) {
    setenv("CCLAW_DB_PATH", e->db, 1);
    if (e->workspace) setenv("CCLAW_WORKSPACE", e->workspace, 1);
    if (e->home) setenv("HOME", e->home, 1);
    if (e->max_iter) setenv("CCLAW_MAX_ITERATIONS", e->max_iter, 1);
    if (e->containment) setenv("CCLAW_CONTAINMENT", e->containment, 1);
    else unsetenv("CCLAW_CONTAINMENT");
    if (e->url) {
        setenv("CCLAW_PROVIDER_BASE_URL", e->url, 1);
        setenv("OPENROUTER_API_KEY", "test-key", 1);
        setenv("CCLAW_MODEL", "test-model", 1);
    } else {
        setenv("ANTHROPIC_API_KEY", "test-key", 1);
        unsetenv("CCLAW_PROVIDER_BASE_URL");
        unsetenv("CCLAW_PROVIDER_ENDPOINT_TYPE");
        unsetenv("CCLAW_MODEL");
        unsetenv("OPENROUTER_API_KEY");
    }
}

/* Fork argv[0] with stdin on /dev/null and stdout+stderr on `log`
 * (appended or truncated). The caller waits or stops it. */
static inline pid_t integ_spawn(const IntegEnv *e, const char *log, int append,
                                char *const argv[]) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    int nfd = open("/dev/null", O_RDONLY);
    int lfd = open(log, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (nfd < 0 || lfd < 0) _exit(110);
    dup2(nfd, 0); dup2(lfd, 1); dup2(lfd, 2);
    if (nfd > 2) close(nfd);
    if (lfd > 2) close(lfd);
    integ_child_env(e);
    execv(argv[0], argv);
    _exit(111);
}

/* Exit status, 128 for a signal, -1 when it outlived `tenths` (killed). */
static inline int integ_wait(pid_t pid, int tenths) {
    if (pid < 0) return -1;
    for (int i = 0; i < tenths; i++) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        integ_nap();
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* Signal, give it ten seconds, then SIGKILL. */
static inline void integ_stop(pid_t pid, int sig) {
    if (pid <= 0) return;
    kill(pid, sig);
    for (int i = 0; i < 100 && waitpid(pid, NULL, WNOHANG) == 0; i++) integ_nap();
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static inline pid_t integ_spawn_cli(const IntegEnv *e, const char *exe,
                                    const char *prompt, int64_t sid,
                                    const char *log, int append) {
    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "%lld", (long long)sid);
    char *argv[8];
    int n = 0;
    argv[n++] = (char *)exe;
    if (sid > 0 && e->verbose) argv[n++] = "-v";
    argv[n++] = "-p";
    argv[n++] = (char *)prompt;
    if (sid > 0) { argv[n++] = "-s"; argv[n++] = sbuf; }
    argv[n] = NULL;
    return integ_spawn(e, log, append, argv);
}

/* One `cclaw -p` run (resuming `sid` when > 0), log truncated. Returns its
 * exit status, or -1 when it outlived `tenths`. */
static inline int integ_run_cli_p(const IntegEnv *e, const char *exe,
                                  const char *prompt, int64_t sid,
                                  const char *log, int tenths) {
    return integ_wait(integ_spawn_cli(e, exe, prompt, sid, log, 0), tenths);
}

static inline pid_t integ_spawn_daemon(const IntegEnv *e, const char *exe,
                                       const char *log, int append) {
    char *argv[] = {(char *)exe, "-v", "--daemon", NULL};
    return integ_spawn(e, log, append, argv);
}

static inline void integ_dump_log(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    fprintf(stderr, "--- %s ---\n", path);
    while (fgets(line, sizeof(line), f)) fputs(line, stderr);
    fprintf(stderr, "--- end %s ---\n", path);
    fclose(f);
}

/* Schema + seed, then the mock as the only provider and model: an
 * OpenAI-compatible 'openrouter' row, or a direct 'anthropic' one.
 * context_window 0 leaves the column to its default. */
static inline int integ_seed_provider(const char *db_path, const char *url,
                                      int anthropic, int context_window) {
    sqlite3 *seed = test_db_open(db_path);
    if (!seed) return -1;
    if (db_seed_defaults(seed) != 0) { sqlite3_close(seed); return -1; }
    sqlite3_exec(seed, "DELETE FROM models; DELETE FROM providers;", NULL, NULL, NULL);
    const char *prov = anthropic
        ? "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model)"
          " VALUES('anthropic', ?1, 'anthropic', 'ANTHROPIC_API_KEY', 'claude-test')"
        : "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model)"
          " VALUES('openrouter', ?1, 'openai', 'OPENROUTER_API_KEY',"
          "        'deepseek/deepseek-v4.1-flash')";
    const char *model = anthropic
        ? "INSERT INTO models(id, provider_name, model, status)"
          " VALUES('anthropic/claude-test', 'anthropic', 'claude-test', 'healthy')"
        : "INSERT INTO models(id, provider_name, model, status)"
          " VALUES('openrouter/deepseek/deepseek-v4.1-flash', 'openrouter',"
          "        'deepseek/deepseek-v4.1-flash', 'healthy')";
    sqlite3_stmt *s;
    int rc = -1;
    if (sqlite3_prepare_v2(seed, prov, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, url, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_DONE) rc = 0;
        sqlite3_finalize(s);
    }
    if (rc == 0 && sqlite3_exec(seed, model, NULL, NULL, NULL) != SQLITE_OK) rc = -1;
    if (rc == 0 && context_window > 0) {
        char *sql = sqlite3_mprintf("UPDATE models SET context_window=%d", context_window);
        if (sqlite3_exec(seed, sql, NULL, NULL, NULL) != SQLITE_OK) rc = -1;
        sqlite3_free(sql);
    }
    sqlite3_close(seed);
    return rc;
}

/* One integer through a fresh connection, ?1 bound to `arg` when given;
 * -1 when there is no row. */
static inline int64_t integ_db_int(const char *db_path, const char *sql, const char *arg) {
    sqlite3 *db = db_open(db_path);
    if (!db) return -1;
    sqlite3_stmt *s;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (arg) sqlite3_bind_text(s, 1, arg, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    db_close(db);
    return v;
}

static inline int integ_db_exec(const char *db_path, const char *sql) {
    sqlite3 *db = db_open(db_path);
    if (!db) return -1;
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "sql: %s\n  %s\n", err ? err : "?", sql);
    sqlite3_free(err);
    db_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

/* How many lines of the log at `path` contain `needle`. */
static inline int integ_count_in_log(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

#endif
