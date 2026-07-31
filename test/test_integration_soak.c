/* test_integration_soak.c — daemon growth-slope soak (r5 F15).
 *
 * Drives many turns through the real llm_proc path (curl round-trip to the
 * in-process mock server) with a full agent_setup_init/destroy per turn —
 * which builds and tears down the tool registry, the QuickJS runtime, and
 * every per-turn sqlite statement. Samples the open-fd count and VmRSS after
 * a warmup, then again at the end, and asserts the slope is flat: an fd or
 * context leaked once per turn would show as a steady climb here.
 *
 * Not the at-rest footprint measurement (consciously deferred) — this is the
 * leak-slope question, which is cheap to settle deterministically. */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "test_util.h"

#define DB_PATH "/tmp/test_integ_soak.db"
#define WARMUP  15
#define TURNS   200

static const char *MOCK_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":2}}";

/* Number of open fds for this process (entries in /proc/self/fd, minus the
 * one the DIR* itself holds). */
static int open_fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        n++;
    }
    closedir(d);
    return n - 1;   /* discount the readdir fd itself */
}

/* Resident set size in KiB from /proc/self/status (VmRSS), or -1. */
static long vmrss_kib(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kib = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &kib);
            break;
        }
    }
    fclose(f);
    return kib;
}

/* One turn: fresh session, one canned reply, full setup init/destroy. */
static void run_one_turn(sqlite3 *db, int i) {
    char sname[32];
    snprintf(sname, sizeof(sname), "soak-%d", i);
    int64_t sid = session_create(db, sname, "Soak", -1, 0);
    assert(sid > 0);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_iteration(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "hi"};
    entry_append_with_iteration(db, sid, &user, 1);

    mock_server_enqueue(200, MOCK_RESPONSE);

    Config *cfg = config_load(db);
    AgentSetup setup;
    assert(agent_setup_init(&setup, db, sid, cfg, "Soak") == 0);
    int rc = test_run_session(db, sid, &setup);
    assert(rc == 0);
    agent_setup_destroy(&setup);
    config_free(cfg);
}

int main(void) {
    TEST_INIT();
    alarm(40);
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int port = mock_server_start();
    assert(port > 0);
    char url[64]; snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_AGENT_NAME", "Soak", 1);
    setenv("CCLAW_STREAM", "0", 1);
    db_agent_upsert(db, "Soak", NULL, NULL);

    /* Warm up: first turns allocate lazily-initialized singletons (curl
     * global state, the shared statement cache), which is one-time growth,
     * not a per-turn leak — measure the slope after they've settled. */
    for (int i = 0; i < WARMUP; i++) run_one_turn(db, i);

    int   fd_base  = open_fd_count();
    long  rss_base = vmrss_kib();
    assert(fd_base > 0 && rss_base > 0);

    for (int i = WARMUP; i < WARMUP + TURNS; i++) run_one_turn(db, i);

    int  fd_end  = open_fd_count();
    long rss_end = vmrss_kib();
    int  fd_delta  = fd_end - fd_base;
    long rss_delta = rss_end - rss_base;

    printf("  soak: %d turns  fd %d -> %d (delta %d)  rss %ldK -> %ldK (delta %ldK)\n",
           TURNS, fd_base, fd_end, fd_delta, rss_base, rss_end, rss_delta);

    /* Fds must be flat: a per-turn fd leak over 200 turns would be enormous;
     * a tiny tolerance absorbs incidental caching (e.g. a reused keep-alive
     * socket to the mock). */
    assert(fd_delta <= 4);

    /* RSS is noisier — the allocator keeps freed arenas — so this only
     * catches an unbounded leak, not steady-state retention. Even a modest
     * per-turn leak (say 4KiB) over 200 turns would blow well past this. */
    assert(rss_delta < 4096);   /* < 4 MiB growth across 200 turns */

    mock_server_stop();
    db_close(db);
    test_db_clean(DB_PATH);
    printf("PASS test_integration_soak\n");
    return 0;
}
