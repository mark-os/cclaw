/* The terminal shows every assistant message of a turn, once.
 *
 * A turn used to print only its last message. Text the model wrote alongside
 * tool calls — often the answer itself — was stored and never shown. This
 * drives the real binary against a mock LLM:
 *  - text + a tool call, then a final text: both are on stdout, in order,
 *    each once;
 *  - text + a tool call, then an EMPTY final: the earlier text is on stdout
 *    exactly once (the turn-end fallback must not print it again).
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "mock_server.h"
#include "test_integ.h"
#include <limits.h>

#define DB_PATH "/tmp/cclaw_integ_cli_interim.db"
#define LOG1    "/tmp/cclaw_integ_cli_interim_run1.log"
#define LOG2    "/tmp/cclaw_integ_cli_interim_run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

#define CALL(id, text) \
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"" text "\"," \
    "\"tool_calls\":[{\"id\":\"" id "\",\"type\":\"function\",\"function\":" \
    "{\"name\":\"file_list\",\"arguments\":\"{\\\"path\\\":\\\".\\\"}\"}}]}," \
    "\"finish_reason\":\"tool_calls\"}]," \
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}"
#define SAY(text) \
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"" text "\"}," \
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}"

static IntegEnv g_env = {.db = DB_PATH};
#define db_count(sql) integ_db_int(DB_PATH, (sql), NULL)
#define db_exec(sql) integ_db_exec(DB_PATH, (sql))

static const char *RESP_FINAL = SAY("acknowledged");

/* ── helpers ─────────────────────────────────────────────────────── */

/* ── assertions ──────────────────────────────────────────────────── */

static int count_in_file(const char *path, const char *needle, long *first_at) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    int c = 0;
    if (first_at) *first_at = -1;
    for (char *p = buf; (p = strstr(p, needle)); p += strlen(needle)) {
        if (c == 0 && first_at) *first_at = (long)(p - buf);
        c++;
    }
    return c;
}

static void cleanup_files(void) {
    test_db_clean(DB_PATH);
    unlink(LOG1);
    unlink(LOG2);
}

int main(void) {
    TEST_INIT();
    alarm(40);   /* below make's 45s wrapper: fail loud, not hung */

    cleanup_files();

    char exe[PATH_MAX];
    if (!realpath("build/cclaw", exe)) FAIL("realpath build/cclaw (run from repo root)");

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    g_env.url = url;

    int rc = 1;
    if (integ_seed_provider(DB_PATH, url, 0, 0) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* ── setup run: create the session, agent and tool rows ───────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&g_env, exe, "hello", 0, LOG1, 200) < 0) {
        fprintf(stderr, "FAIL: setup run did not exit\n"); integ_dump_log(LOG1); goto done;
    }
    int64_t sid = db_count("SELECT MAX(id) FROM sessions;");
    if (sid <= 0) { fprintf(stderr, "FAIL: no session\n"); goto done; }
    if (db_exec("INSERT OR IGNORE INTO grants(agent_name,kind,value,source)"
                "  SELECT name,'tool','file_list','operator' FROM agents;") != 0) {
        fprintf(stderr, "FAIL: grant\n"); goto done;
    }

    /* ── text with a tool call, then a final text ─────────────────── */
    mock_server_reset();
    mock_server_enqueue(200, CALL("call_1", "INTERIM-ANSWER-391"));
    mock_server_enqueue(200, SAY("FINAL-WORD"));
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&g_env, exe, "question", sid, LOG1, 200) < 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n"); integ_dump_log(LOG1); goto done;
    }
    long at_i, at_f, at_tool;
    int ni = count_in_file(LOG1, "INTERIM-ANSWER-391", &at_i);
    int nf = count_in_file(LOG1, "FINAL-WORD", &at_f);
    count_in_file(LOG1, "[file_list", &at_tool);
    if (ni != 1 || nf != 1) {
        fprintf(stderr, "FAIL: interim shown %d time(s), final %d (want 1 and 1)\n", ni, nf);
        integ_dump_log(LOG1); goto done;
    }
    if (!(at_i < at_tool && at_tool < at_f)) {
        fprintf(stderr, "FAIL: order is not interim text, tool line, final text\n");
        integ_dump_log(LOG1); goto done;
    }

    /* ── text with a tool call, then an empty final ───────────────── */
    mock_server_reset();
    mock_server_enqueue(200, CALL("call_2", "ONLY-THING-SAID"));
    mock_server_enqueue(200, SAY(""));
    mock_server_enqueue(200, SAY(""));
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&g_env, exe, "another", sid, LOG2, 200) < 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n"); integ_dump_log(LOG2); goto done;
    }
    int no = count_in_file(LOG2, "ONLY-THING-SAID", NULL);
    if (no != 1) {
        fprintf(stderr, "FAIL: the only message was shown %d time(s), want 1\n", no);
        integ_dump_log(LOG2); goto done;
    }

    printf("All CLI interim-text integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
