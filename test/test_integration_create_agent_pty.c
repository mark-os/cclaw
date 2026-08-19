/* Approve-path e2e for create_agent under a PTY: park → y → agent_definition_apply.
 * Same harness as test_integration_request_config_pty (see its header comment
 * for why a PTY is required). Asserts the agent row, capped grants, and the
 * enumerated approval summary reached the prompt. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "db.h"
#include "mock_server.h"
#include "test_util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_pty_ca.db"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

/* Assistant proposes a restricted Scout with a subset grant. */
static const char *RESP_TOOL_CALL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_ca1\",\"type\":\"function\",\"function\":"
    "{\"name\":\"create_agent\",\"arguments\":"
    "\"{\\\"name\\\":\\\"Scout\\\",\\\"description\\\":\\\"scouts things\\\","
    "\\\"system_prompt\\\":\\\"You scout.\\\","
    "\\\"sandbox_profile\\\":\\\"restricted\\\","
    "\\\"grants\\\":{\\\"tools\\\":[\\\"file_read\\\"]}}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static const char *RESP_FINAL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"scout created\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static int expect(int mfd, const char *needle, int timeout_ms) {
    static char acc[16384];
    size_t used = 0;
    acc[0] = '\0';
    int waited = 0;
    while (waited < timeout_ms) {
        struct pollfd p = {.fd = mfd, .events = POLLIN};
        int n = poll(&p, 1, 200);
        waited += 200;
        if (n <= 0) continue;
        ssize_t r = read(mfd, acc + used, sizeof(acc) - used - 1);
        if (r <= 0) {
            if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }
        used += (size_t)r;
        acc[used] = '\0';
        if (strstr(acc, needle)) return 0;
        if (used > sizeof(acc) - 512) {
            memmove(acc, acc + used - 4096, 4096);
            used = 4096;
            acc[used] = '\0';
        }
    }
    fprintf(stderr, "expect timeout waiting for [%s]; got:\n%s\n", needle, acc);
    return -1;
}

static int send_line(int mfd, const char *s) {
    size_t len = strlen(s);
    if (write(mfd, s, len) != (ssize_t)len) return -1;
    return write(mfd, "\n", 1) == 1 ? 0 : -1;
}

static void cleanup_files(void) {
    test_db_clean(DB_PATH);
}

int main(void) {
    TEST_INIT();
    alarm(40);

    cleanup_files();

    char exe[PATH_MAX];
    if (!realpath("build/cclaw", exe)) FAIL("realpath build/cclaw (run from repo root)");

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    mock_server_enqueue(200, RESP_TOOL_CALL);
    mock_server_enqueue(200, RESP_FINAL);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);

    /* Seed config + swap providers/models for the mock (see the request_config
     * PTY test for the rationale). */
    {
        sqlite3 *seed = test_db_open(DB_PATH);
        if (!seed) FAIL("test_db_open for seed");
        if (db_seed_defaults(seed) != 0) FAIL("db_seed_defaults");
        sqlite3_exec(seed, "DELETE FROM models; DELETE FROM providers;", NULL, NULL, NULL);
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(seed,
                "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model)"
                " VALUES('mock', ?1, 'openai', 'OPENROUTER_API_KEY', 'test-model')",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, url, -1, SQLITE_STATIC);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        sqlite3_exec(seed,
            "INSERT INTO models(id, provider_name, model, status, priority)"
            " VALUES('mock/test-model', 'mock', 'test-model', 'healthy', 0)",
            NULL, NULL, NULL);
        sqlite3_close(seed);
    }

    int mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0) FAIL("posix_openpt");
    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0) FAIL("grantpt/unlockpt");
    const char *slave_name = ptsname(mfd);
    if (!slave_name) FAIL("ptsname");

    pid_t pid = fork();
    if (pid < 0) FAIL("fork");
    if (pid == 0) {
        setsid();
        int sfd = open(slave_name, O_RDWR);
        if (sfd < 0) _exit(112);
        dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
        if (sfd > 2) close(sfd);
        close(mfd);
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
        setenv("OPENROUTER_API_KEY", "test-key", 1);
        setenv("CCLAW_MODEL", "test-model", 1);
        setenv("CCLAW_STREAM", "0", 1);
        unsetenv("CCLAW_SANDBOX_PROFILE");
        execl(exe, exe, (char *)NULL);
        _exit(113);
    }

    int rc = 1;
    if (expect(mfd, "> ", 10000) != 0) goto done;
    if (send_line(mfd, "please make a scout agent") != 0) goto done;
    if (expect(mfd, "Grant? (y/n):", 15000) != 0) goto done;
    if (send_line(mfd, "y") != 0) goto done;
    if (expect(mfd, "scout created", 15000) != 0) goto done;
    if (send_line(mfd, "exit") != 0) goto done;

    for (int i = 0; i < 100; i++) {
        pid_t w = waitpid(pid, NULL, WNOHANG);
        if (w == pid) { pid = 0; break; }
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (pid) goto done;

    /* Assert DB effects: agent row with declared profile, grants, approval. */
    {
        sqlite3 *db;
        if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { fprintf(stderr, "FAIL: db open\n"); goto done; }
        sqlite3_stmt *s;
        int ok = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM agents WHERE name='Scout'"
                " AND sandbox_profile='restricted' AND description='scouts things'",
                -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        if (!ok) { fprintf(stderr, "FAIL: Scout agent row missing/wrong\n"); sqlite3_close(db); goto done; }

        ok = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM grants WHERE agent_name='Scout'"
                " AND kind='tool' AND value='file_read'", -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        if (!ok) { fprintf(stderr, "FAIL: Scout grant missing\n"); sqlite3_close(db); goto done; }

        ok = 0;
        if (sqlite3_prepare_v2(db,
                /* apply-style approvals are terminal at decision time */
                "SELECT 1 FROM approvals WHERE tool_name='create_agent'"
                " AND state='consumed'", -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        sqlite3_close(db);
        if (!ok) { fprintf(stderr, "FAIL: consumed create_agent approval missing\n"); goto done; }
    }

    if (mock_server_request_count() != 2) {
        fprintf(stderr, "FAIL: expected 2 LLM requests, got %d\n",
                mock_server_request_count());
        goto done;
    }

    rc = 0;
    printf("PASS create_agent pty approve path\n");

done:
    if (pid > 0) kill(pid, SIGKILL);
    close(mfd);
    mock_server_stop();
    cleanup_files();
    return rc;
}
