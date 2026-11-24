/* The Anthropic Messages wire format, end to end through the real binary.
 *
 * The unit tests cover the request builder and the response ingest in
 * isolation; what they cannot see is the two meeting across a real tool loop:
 * the URL and headers the transport picks, a forked tool's result landing in
 * the entries the next request is built from, and the thinking block going
 * back where it came from. Two `-p` runs against a mock /v1/messages (which
 * rejects a request missing the version or key header, as the real API does).
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "mock_server.h"
#include "test_integ.h"
#include <limits.h>

#define DB_PATH "/tmp/cclaw_integ_anthropic.db"
#define LOG1    "/tmp/cclaw_integ_anthropic_run1.log"
#define LOG2    "/tmp/cclaw_integ_anthropic_run2.log"

#define USAGE "\"usage\":{\"input_tokens\":3,\"cache_read_input_tokens\":100," \
              "\"cache_creation_input_tokens\":20,\"output_tokens\":5}"

static const char *RESP_CALL =
    "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
    "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"SIG-1\"},"
    "{\"type\":\"tool_use\",\"id\":\"toolu_F\",\"name\":\"file_list\","
    " \"input\":{\"path\":\".\"}}],"
    "\"stop_reason\":\"tool_use\"," USAGE "}";

static const char *RESP_FINAL =
    "{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
    "{\"type\":\"text\",\"text\":\"acknowledged\"}],"
    "\"stop_reason\":\"end_turn\"," USAGE "}";

/* No URL: nothing but the providers row may select the wire format. */
static const IntegEnv ENV = {.db = DB_PATH};

#define db_int(sql, arg) integ_db_int(DB_PATH, (sql), (arg))

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static int check_tool_loop(void) {
    CHECK(mock_server_request_count() == 2, "expected exactly two requests (call, final)");
    CHECK(db_int("SELECT COUNT(*) FROM tool_calls WHERE call_id='toolu_F'"
                 " AND status='done' AND resolved_by IS NULL;", NULL) == 1,
          "toolu_F was not run by the tool");
    /* Usage normalized to cache-inclusive: 3 + 100 read + 20 written. */
    CHECK(db_int("SELECT COUNT(*) FROM entries WHERE type='assistant_message'"
                 " AND NOT (usage_in=123 AND cached_tokens=100);", NULL) == 0,
          "usage was not normalized on every assistant entry");

    /* The second request: what the first response turned into. */
    const char *req = mock_server_last_request_body();
    CHECK(req, "no request body captured");
    CHECK(db_int("SELECT json_extract(?1,'$.model')='claude-test'"
                 " AND json_extract(?1,'$.max_tokens') >= 16000"
                 " AND json_type(?1,'$.system')='array'"
                 " AND json_extract(?1,'$.messages[#-1].role')='user'", req) == 1,
          "request envelope is not Messages-shaped");
    CHECK(db_int("SELECT json_extract(?1,'$.messages[#-2].role')='assistant'"
                 " AND json_extract(?1,'$.messages[#-2].content[0].type')='thinking'"
                 " AND json_extract(?1,'$.messages[#-2].content[0].signature')='SIG-1'"
                 " AND json_extract(?1,'$.messages[#-2].content[1].id')='toolu_F'", req) == 1,
          "the thinking block did not go back verbatim ahead of its tool_use");
    CHECK(db_int("SELECT json_extract(?1,'$.messages[#-1].content[0].type')='tool_result'"
                 " AND json_extract(?1,'$.messages[#-1].content[0].tool_use_id')='toolu_F'"
                 " AND json_array_length(?1,'$.messages[#-1].content')=1", req) == 1,
          "the tool result did not answer toolu_F in the following user message");
    CHECK(db_int("SELECT COUNT(*) FROM json_each(?1,'$.tools')"
                 " WHERE json_extract(value,'$.name')='file_list'"
                 "   AND json_extract(value,'$.input_schema.type')='object'", req) == 1,
          "file_list is not declared with an input_schema");
    return 0;
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
    if (!realpath("build/cclaw", exe)) {
        fprintf(stderr, "FAIL: realpath build/cclaw (run from repo root)\n");
        return 1;
    }

    int port = mock_server_start();
    if (port < 0) { fprintf(stderr, "FAIL: mock_server_start\n"); return 1; }
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);

    int rc = 1;
    if (integ_seed_provider(DB_PATH, url, 1, 0) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* ── run 1: a plain turn — creates the session, agent and tool rows ── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&ENV, exe, "hello", 0, LOG1, 200) < 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n");
        integ_dump_log(LOG1);
        goto done;
    }
    int64_t sid = db_int("SELECT MAX(id) FROM sessions;", NULL);
    if (sid <= 0 || mock_server_request_count() != 1 ||
        db_int("SELECT COUNT(*) FROM entries WHERE type='assistant_message'"
               " AND content='acknowledged';", NULL) != 1) {
        fprintf(stderr, "FAIL: run 1 did not complete a turn over /v1/messages\n");
        integ_dump_log(LOG1);
        goto done;
    }

    /* ── run 2: thinking + a forked tool call (file_list is granted by
     * default), then the final answer ── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL);
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&ENV, exe, "list files", sid, LOG2, 200) < 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        integ_dump_log(LOG2);
        goto done;
    }
    if (check_tool_loop()) { integ_dump_log(LOG2); goto done; }

    printf("All Anthropic integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
