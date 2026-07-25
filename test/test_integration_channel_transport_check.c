/* Integration test: the `transports: ["persistent"]` -> --check fail-closed
 * guard. A channel that declares the persistent transport must be refused at
 * activation on a libcurl without ws/wss, with a clear message — rather than
 * silently failing at the first conn.open in the live runner. A channel that
 * declares no such transport (Telegram) is unaffected regardless of curl.
 *
 * Forks `cclaw --channel <name> --check` (channel_runner_check lives behind
 * main(), which isn't in libcclaw.a). The persistent-case expectation flips on
 * the runtime libcurl's actual WS capability, so it is meaningful on both a
 * WS-capable box (guard passes) and a libcurl-minimal box (guard fires). The
 * test handler's onInit just returns {} — no conn.open, no network. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <curl/curl.h>
#include "db.h"
#include "util.h"           /* curl_ws_available */
#include "test_util.h"

#define DB_PATH "/tmp/test_transport_check.db"
#define PERSIST_DIR "/tmp/test_transport_check_persist"
#define PLAIN_DIR   "/tmp/test_transport_check_plain"
#define OUT_PATH    "/tmp/test_transport_check_out.txt"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)


static void write_bundle(const char *dir, const char *manifest) {
    mkdir(dir, 0755);
    char path[512];
    snprintf(path, sizeof(path), "%s/extension.json", dir);
    FILE *f = fopen(path, "w"); fputs(manifest, f); fclose(f);
    snprintf(path, sizeof(path), "%s/channel.qjs", dir);
    f = fopen(path, "w"); fputs("function onInit() { return {}; }\n", f); fclose(f);
}

static void seed_channel(sqlite3 *db, const char *name, const char *dir) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO extensions(name,path) VALUES('%s','%s');", name, dir);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO channels(name,extension_name) VALUES('%s','%s');", name, name);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* Fork `cclaw --channel <name> --check`; child stdout+stderr -> OUT_PATH.
 * Returns the exit code (or -1), and copies the captured output into out. */
static int run_check(const char *name, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open(OUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        execl("./build/cclaw", "cclaw", "--channel", name, "--check", (char *)NULL);
        _exit(127);
    }
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    if (out && cap) {
        FILE *f = fopen(OUT_PATH, "r");
        if (f) { size_t n = fread(out, 1, cap - 1, f); out[n] = '\0'; fclose(f); }
    }
    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
}

int main(void) {
    TEST_INIT();
    printf("test_integration_channel_transport_check:\n");
    (void)system("rm -rf /tmp/test_transport_check*");

    write_bundle(PERSIST_DIR,
        "{\"name\":\"discx\",\"version\":\"0.1.0\","
        "\"channel\":{\"type\":\"discord\",\"handler\":\"channel.qjs\","
        "\"transports\":[\"persistent\"]}}");
    write_bundle(PLAIN_DIR,
        "{\"name\":\"tgx\",\"version\":\"0.1.0\","
        "\"channel\":{\"type\":\"telegram\",\"handler\":\"channel.qjs\"}}");

    sqlite3 *db = test_db_open(DB_PATH);
    if (!db) FAIL("db open");
    seed_channel(db, "discx", PERSIST_DIR);
    seed_channel(db, "tgx", PLAIN_DIR);
    db_close(db);

    int have_ws = curl_ws_available();
    printf("  runtime libcurl ws/wss: %s\n", have_ws ? "present" : "absent");

    char out[4096];

    /* Case 1: persistent-declaring channel. */
    int rc = run_check("discx", out, sizeof(out));
    if (have_ws) {
        if (rc != 0) { fprintf(stderr, "  out: %s\n", out); FAIL("persistent channel refused despite WS support"); }
        printf("  PASS: WS present -> persistent channel check passes\n");
    } else {
        if (rc == 0) FAIL("persistent channel passed check on a WS-less libcurl");
        if (!strstr(out, "ws/wss")) { fprintf(stderr, "  out: %s\n", out); FAIL("wrong/absent fail-closed message"); }
        printf("  PASS: WS absent -> persistent channel refused with clear message\n");
    }

    /* Case 2: no transport declared — must pass regardless of WS. */
    rc = run_check("tgx", out, sizeof(out));
    if (rc != 0) { fprintf(stderr, "  out: %s\n", out); FAIL("non-persistent channel wrongly gated on WS"); }
    printf("  PASS: no transport declared -> unconstrained (check passes)\n");

    (void)system("rm -rf /tmp/test_transport_check*");
    printf("all transport-check integration tests passed\n");
    return 0;
}
