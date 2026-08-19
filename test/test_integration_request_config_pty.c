/* Approve-path e2e for request_config under a PTY.
 *
 * -p mode auto-denies all approvals, so the interactive approve loop (park →
 * y/n prompt → resolve_approval → apply_grant) is unreachable by unit tests
 * and by the other integration tests. Run build/cclaw under a PTY so
 * isatty(stdin) is true, script a request_changes tool call from the mock
 * LLM, answer 'y' at the prompt, and assert the grant landed as kind
 * 'read_path' (the request_config_changes_apply path is the only place it's
 * exercised end to end). */
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

#define DB_PATH   "/tmp/cclaw_pty_rc.db"
#define GRANT_DIR "/tmp/cclaw_pty_grant_dir"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

/* Assistant asks for read access outside the workspace, with a reason. */
static const char *RESP_TOOL_CALL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_pty1\",\"type\":\"function\",\"function\":"
    "{\"name\":\"request_config\",\"arguments\":"
    "\"{"
    "\\\"changes\\\":{\\\"grants\\\":{\\\"read_paths\\\":[\\\"" GRANT_DIR "\\\"]}},"
    "\\\"reason\\\":\\\"pty integration test\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

/* Second call: repoint the LLM provider — must park and, on approve, upsert
 * the providers row from the parked args (request_config_changes_apply).
 * args carry only the secret NAME (api_key_env), never key material. */
static const char *RESP_TOOL_CALL2 =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_pty2\",\"type\":\"function\",\"function\":"
    "{\"name\":\"request_config\",\"arguments\":"
    "\"{"
    "\\\"changes\\\":{\\\"provider\\\":{\\\"provider\\\":\\\"myllm\\\","
    "\\\"base_url\\\":\\\"https://myllm.example.com/v1\\\","
    "\\\"api_key_env\\\":\\\"MYLLM_API_KEY\\\"}},"
    "\\\"reason\\\":\\\"pty provider test\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

/* Third call: malformed arguments (valid JSON, not an object — raw garbage
 * is already rejected at response ingestion) — the dispatch gate must fail
 * closed with a model-visible error tool-result (no approval, no execution). */
static const char *RESP_TOOL_CALL3 =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_pty3\",\"type\":\"function\",\"function\":"
    "{\"name\":\"request_config\",\"arguments\":\"[1,2]\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static const char *RESP_FINAL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"grant received\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

/* Read from the PTY master until `needle` appears (across reads) or
 * timeout_ms elapses. Returns 0 on match. Echo is left on, so our own
 * writes also flow past here — harmless for substring matching. */
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
            break; /* child gone */
        }
        used += (size_t)r;
        acc[used] = '\0';
        if (strstr(acc, needle)) return 0;
        if (used > sizeof(acc) - 512) { /* keep a tail window */
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
    alarm(40); /* below make's 45s wrapper: fail loud, not hung */

    cleanup_files();
    mkdir(GRANT_DIR, 0755);

    char exe[PATH_MAX];
    if (!realpath("build/cclaw", exe)) FAIL("realpath build/cclaw (run from repo root)");

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    mock_server_enqueue(200, RESP_TOOL_CALL);
    mock_server_enqueue(200, RESP_TOOL_CALL2);
    mock_server_enqueue(200, RESP_TOOL_CALL3);
    mock_server_enqueue(200, RESP_FINAL);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);

    /* Pre-seed providers/models pointing at the mock so load_candidates()
     * (llm_proc.c) picks it up. db_seed_defaults() (called by the child on
     * first run, gated on an empty config table) would otherwise seed the
     * real OpenRouter provider, which outranks the CCLAW_PROVIDER_BASE_URL
     * env fallback (only used when nmodels==0). Run the real seed here so
     * the child's config table is non-empty and skips reseeding, then swap
     * the provider/model rows for ones pointing at the mock server. */
    {
        sqlite3 *seed = test_db_open(DB_PATH);
        if (!seed) FAIL("test_db_open for seed");
        if (db_seed_defaults(seed) != 0) FAIL("db_seed_defaults");
        sqlite3_exec(seed, "DELETE FROM models; DELETE FROM providers;", NULL, NULL, NULL);
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(seed,
                "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model)"
                " VALUES('openrouter', ?1, 'openai', 'OPENROUTER_API_KEY', 'deepseek/deepseek-v4-flash')",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, url, -1, SQLITE_STATIC);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        sqlite3_exec(seed,
            "INSERT INTO models(id, provider_name, model, status)"
            " VALUES('openrouter/deepseek/deepseek-v4-flash', 'openrouter',"
        "        'deepseek/deepseek-v4-flash', 'healthy')",
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
        /* Child: make the PTY slave our controlling terminal + stdio. */
        setsid();
        int sfd = open(slave_name, O_RDWR); /* acquires ctty */
        if (sfd < 0) _exit(112);
        dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
        if (sfd > 2) close(sfd);
        close(mfd);
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
        setenv("OPENROUTER_API_KEY", "test-key", 1);
        /* The provider document names this credential; config-time
         * validation refuses a document whose key resolves nowhere. */
        setenv("MYLLM_API_KEY", "test-myllm-key", 1);
        setenv("CCLAW_MODEL", "test-model", 1);
        setenv("CCLAW_STREAM", "0", 1);
        unsetenv("CCLAW_SANDBOX_PROFILE");
        execl(exe, exe, (char *)NULL);
        _exit(113);
    }

    /* Drive: banner prompt → user turn → approval prompt → y → final → exit */
    int rc = 1;
    if (expect(mfd, "> ", 10000) != 0) goto done;
    if (send_line(mfd, "please get access") != 0) goto done;
    if (expect(mfd, "Grant? (y/n):", 15000) != 0) goto done;
    if (send_line(mfd, "y") != 0) goto done;
    if (expect(mfd, "Grant? (y/n):", 15000) != 0) goto done;
    if (send_line(mfd, "y") != 0) goto done;
    if (expect(mfd, "grant received", 15000) != 0) goto done;
    if (send_line(mfd, "exit") != 0) goto done;

    for (int i = 0; i < 100; i++) { /* reap with deadline */
        pid_t w = waitpid(pid, NULL, WNOHANG);
        if (w == pid) { pid = 0; break; }
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (pid) goto done;

    /* Assert DB effects: read_path grant, approved approval, resolved call. */
    {
        sqlite3 *db;
        if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { fprintf(stderr, "FAIL: db open\n"); goto done; }
        sqlite3_stmt *s;
        int ok = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM grants WHERE agent_name='Assistant'"
                " AND kind='read_path' AND value=?1", -1, &s, NULL) == SQLITE_OK) {
            char canon[PATH_MAX];
            sqlite3_bind_text(s, 1, realpath(GRANT_DIR, canon) ? canon : GRANT_DIR,
                              -1, SQLITE_TRANSIENT);
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        if (!ok) { fprintf(stderr, "FAIL: read_path grant missing\n"); sqlite3_close(db); goto done; }

        ok = 0;
        if (sqlite3_prepare_v2(db,
                /* apply-style approvals are terminal at decision time */
                "SELECT 1 FROM approvals WHERE action='request_changes' AND state='consumed'"
                " AND json_extract(args_json,'$.reason')='pty integration test'",
                -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        if (!ok) { fprintf(stderr, "FAIL: consumed approval with reason missing\n"); sqlite3_close(db); goto done; }

        ok = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM tool_calls WHERE call_id='call_pty1' AND status='done'",
                -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        if (!ok) { fprintf(stderr, "FAIL: frozen tool_call not resolved\n"); sqlite3_close(db); goto done; }

        /* request_changes (provider) applied: providers row upserted from
         * parked args, api_key_env explicitly supplied. */
        ok = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM providers WHERE name='myllm'"
                " AND base_url='https://myllm.example.com/v1'"
                " AND api_key_env='MYLLM_API_KEY'",
                -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        sqlite3_close(db);
        if (!ok) { fprintf(stderr, "FAIL: request_changes providers row missing\n"); goto done; }

        /* Malformed-args call: gate wrote an error tool-result, the call is
         * done with decided_via bad_args, and no approval was created. */
        if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { fprintf(stderr, "FAIL: db reopen\n"); goto done; }
        ok = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM tool_calls WHERE call_id='call_pty3' AND status='done'",
                -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        if (!ok) { fprintf(stderr, "FAIL: bad-args call not resolved\n"); sqlite3_close(db); goto done; }
        ok = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM entries WHERE role=3 AND content LIKE '%not a valid JSON object%'",
                -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) == SQLITE_ROW);
            sqlite3_finalize(s);
        }
        if (!ok) { fprintf(stderr, "FAIL: bad-args error entry missing\n"); sqlite3_close(db); goto done; }
        ok = 1;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM approvals WHERE tool_call_id='call_pty3'",
                -1, &s, NULL) == SQLITE_OK) {
            ok = (sqlite3_step(s) != SQLITE_ROW);
            sqlite3_finalize(s);
        }
        sqlite3_close(db);
        if (!ok) { fprintf(stderr, "FAIL: bad-args call parked an approval\n"); goto done; }
    }

    if (mock_server_request_count() != 4) {
        fprintf(stderr, "FAIL: expected 4 LLM requests, got %d\n",
                mock_server_request_count());
        goto done;
    }

    printf("  PASS approve_grant_path_via_pty\n");
    printf("All request_config PTY tests passed.\n");
    rc = 0;

done:
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    close(mfd);
    mock_server_stop();
    cleanup_files();
    return rc;
}
