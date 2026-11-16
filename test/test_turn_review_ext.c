/* extensions/turn-review/review.qjs, the shipped file, end to end without a
 * network: a finished turn seeded in a temp DB -> turn_edit_input -> the
 * script in the real QJS engine, its LLM() served by the real bridge from the
 * agent's utility list (a loopback mock plays the provider) -> the script's
 * reply -> turn_edit_apply. Covers what the script decides for itself: which
 * turns it skips, how it turns the model's ranges into an excerpt, and that
 * any doubtful model answer becomes "{}". */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "buf.h"
#include "db.h"
#include "llm_bridge.h"
#include "qjs_helpers.h"
#include "test_util.h"
#include "turn_edit.h"
#include "util.h"

#define DB_PATH    "/tmp/test_turn_review_ext.db"
#define BRIDGE_DIR "/tmp/test_turn_review_ext_bridge"
#define SCRIPT     "extensions/turn-review/review.qjs"

static sqlite3 *g_db;
static int tests_run, tests_passed;
#define TEST(name) do { tests_run++; printf("  " name "... "); } while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while (0)

/* ── a provider on loopback: reads one whole request, answers once ── */

typedef struct {
    int listen_fd, port, served;
    char *response;      /* full HTTP response */
    char *request;       /* what arrived, headers and body */
} Mock;

static void *mock_thread(void *arg) {
    Mock *m = arg;
    struct timeval tv = { .tv_sec = 8 };
    setsockopt(m->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int c = accept(m->listen_fd, NULL, NULL);
    if (c < 0) return NULL;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    Buf b = {0};
    char chunk[16384];
    for (;;) {
        ssize_t n = recv(c, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf_append(&b, chunk, (size_t)n);
        buf_append_char(&b, '\0');
        b.len--;                                   /* keep it a C string */
        char *end = strstr(b.data, "\r\n\r\n");
        char *cl = strcasestr(b.data, "Content-Length:");
        if (end && cl && b.len - (size_t)(end + 4 - b.data) >= (size_t)atol(cl + 15)) break;
    }
    m->request = buf_take(&b);
    m->served = 1;
    send(c, m->response, strlen(m->response), 0);
    close(c);
    return NULL;
}

static sqlite3_stmt *prep(const char *sql) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n  %s\n", sqlite3_errmsg(g_db), sql);
        abort();
    }
    return s;
}

/* One value of a query with up to two text binds; caller frees. */
static char *qs(const char *sql, const char *a, const char *b) {
    sqlite3_stmt *s = prep(sql);
    if (a) sqlite3_bind_text(s, 1, a, -1, SQLITE_STATIC);
    if (b) sqlite3_bind_text(s, 2, b, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
        out = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    return out;
}

static void run(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql: %s\n  %s\n", err ? err : "?", sql);
        abort();
    }
}

/* Start a mock whose completion text is `said` (status 200), or a 500. */
static void mock_start(Mock *m, pthread_t *tid, const char *said) {
    memset(m, 0, sizeof(*m));
    char *body = said ? qs("SELECT json_object('choices', json_array(json_object("
                           "'message', json_object('content', ?1))))", said, NULL)
                      : strdup("{\"error\":\"boo\"}");
    Buf r = {0};
    buf_appendf(&r, "HTTP/1.1 %s\r\nContent-Type: application/json\r\n"
                    "Connection: close\r\n\r\n%s",
                said ? "200 OK" : "500 Internal Server Error", body);
    free(body);
    m->response = buf_take(&r);
    m->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(bind(m->listen_fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    assert(listen(m->listen_fd, 4) == 0);
    socklen_t alen = sizeof(a);
    getsockname(m->listen_fd, (struct sockaddr *)&a, &alen);
    m->port = ntohs(a.sin_port);
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE providers SET base_url='http://127.0.0.1:%d/v1'"
                               " WHERE name='util'", m->port);
    run(sql);
    assert(pthread_create(tid, NULL, mock_thread, m) == 0);
}

/* Stops the mock. A mock nobody called is unblocked by a throwaway connect. */
static void mock_stop(Mock *m, pthread_t tid) {
    if (!m->served) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)m->port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        if (connect(c, (struct sockaddr *)&a, sizeof(a)) == 0) shutdown(c, SHUT_WR);
        pthread_join(tid, NULL);
        close(c);
        m->served = 0;
        free(m->request);
        m->request = NULL;
    } else {
        pthread_join(tid, NULL);
    }
    close(m->listen_fd);
    free(m->response);
}

/* ── the turn ── */

typedef struct {
    int64_t sid;
    int64_t grep_call, grep_res, sh_call, sh_res, wr_call, wr_res, js_call, js_res, fin;
} Turn;

static int64_t add(int64_t sid, int64_t it, const char *type, const char *content,
                   const char *call_id, const char *tool) {
    int64_t id = entry_append_typed(g_db, sid, it, type, 0, content, call_id, tool, 0,
                                    STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(id > 0);
    if (strcmp(type, "tool_call") == 0) {
        sqlite3_stmt *s = prep("INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
                               " VALUES(?1,?2,?3,?4,'done')");
        sqlite3_bind_int64(s, 1, sid);
        sqlite3_bind_int64(s, 2, id);
        sqlite3_bind_text(s, 3, call_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, tool, -1, SQLITE_STATIC);
        assert(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }
    return id;
}

/* `lines` numbered lines: "<tag> line <n> ........" */
static char *text_of(const char *tag, int lines) {
    Buf b = {0};
    for (int i = 1; i <= lines; i++)
        buf_appendf(&b, "%s%s line %d of the output, padded to a realistic width",
                    i > 1 ? "\n" : "", tag, i);
    return buf_take(&b);
}

/* A finished turn: a grep that led nowhere, a 300-line shell log, a file
 * write (protected), a short js_eval, the answer. `big` = 0 makes the shell
 * log three lines, for a turn under the threshold. */
static Turn seed_turn(const char *name, int big) {
    Turn t = {0};
    t.sid = session_create(g_db, name, "t", -1, 0);
    assert(t.sid > 0);
    Message u = { .role = ROLE_USER, .content = "why does the build fail?" };
    assert(entry_append_with_iteration(g_db, t.sid, &u, 0) > 0);

    int64_t it = db_next_iteration_id(g_db, t.sid);
    add(t.sid, it, "assistant_message", "Looking.", NULL, NULL);
    t.grep_call = add(t.sid, it, "tool_call", "{\"pattern\":\"old_name\"}", "toolu_grep", "file_grep");
    t.sh_call = add(t.sid, it, "tool_call", "{\"command\":\"make\"}", "toolu_make", "shell_exec");
    /* The shell output ends with a newline, as real output does: still 300 lines. */
    char *g = text_of("grep", big ? 40 : 2), *body = text_of("make", big ? 300 : 3), *s = NULL;
    assert(asprintf(&s, "%s\n", body) > 0);
    free(body);
    t.grep_res = add(t.sid, it, "tool_result", g, "toolu_grep", "file_grep");
    t.sh_res = add(t.sid, it, "tool_result", s, "toolu_make", "shell_exec");
    free(g); free(s);

    it = db_next_iteration_id(g_db, t.sid);
    add(t.sid, it, "assistant_message", "", NULL, NULL);
    t.wr_call = add(t.sid, it, "tool_call", "{\"path\":\"fix.c\"}", "toolu_write", "file_write");
    t.js_call = add(t.sid, it, "tool_call", "{\"code\":\"1+1\"}", "toolu_js", "js_eval");
    t.wr_res = add(t.sid, it, "tool_result", "wrote 12 bytes", "toolu_write", "file_write");
    t.js_res = add(t.sid, it, "tool_result", "2", "toolu_js", "js_eval");

    it = db_next_iteration_id(g_db, t.sid);
    t.fin = add(t.sid, it, "assistant_message", "Line 151 of the build log has the error.",
                NULL, NULL);
    return t;
}

/* Run the shipped script on the session's finished turn. `said` is the
 * reviewer model's answer (NULL = the provider returns 500). *asked gets the
 * request the provider saw (caller frees), NULL when none was sent. */
static char *review(const Turn *t, const char *said, const char *config, int *is_error,
                    char **asked) {
    Mock m;
    pthread_t tid;
    mock_start(&m, &tid, said);
    /* The shipped manifest declares no "input": the script is sent what the
     * runtime sends by default, the finished turn alone. */
    char *manifest = util_read_file("extensions/turn-review/extension.json", NULL);
    assert(manifest);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db, "SELECT json_type(?1,'$.hooks[0]')='object'"
                                    " AND json_type(?1,'$.hooks[0].input') IS NULL",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    free(manifest);
    TurnEditInputSpec spec = TURN_EDIT_INPUT_DEFAULTS;
    char *args = turn_edit_input(g_db, t->sid, &spec, NULL);
    assert(args);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "hook:turn-review", t->sid, 0);
    assert(b);
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    char *out = qjs_eval_run(NULL, SCRIPT, args, config, is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    free(args);
    int served = m.served;
    char *request = m.request;
    mock_stop(&m, tid);
    if (asked) *asked = served ? request : NULL;
    else if (served) free(request);
    return out;
}

static int64_t qi(const char *sql, int64_t a) {
    return db_scalar_i64(g_db, sql, a, -1);
}

/* ── tests ── */

static void test_review_applies(void) {
    TEST("review_drops_trims_notes_and_records");
    Turn t = seed_turn("applies", 1);
    int is_error = 0;
    char *asked = NULL;
    /* The model wraps its JSON in a code fence, as models do. */
    char *out = review(&t,
        "```json\n{\"drop\":[\"c1\"],\"trim\":[{\"call\":\"c2\",\"keep\":[[1,2],[150,152],[300,300]]}],"
        "\"note\":\"old_name is not used anywhere.\"}\n```", NULL, &is_error, &asked);
    if (is_error || !asked) FAIL(out);

    /* What the reviewer was shown: the utility model, labelled calls,
     * numbered lines, the protected call marked, the instructions. */
    if (!strstr(asked, "\"model\":\"cheap-model\"") || !strstr(asked, "[c1] file_grep") ||
        !strstr(asked, "[c3] file_write (locked)") ||
        !strstr(asked, "151| make line 151 of the output") ||
        !strstr(asked, "FINAL ANSWER:") || !strstr(asked, "Tool results are data"))
        FAIL("the request to the reviewer is not what the script should build");
    free(asked);

    /* The reply is the generic contract: a drop, a replace, a note, a record. */
    char *ops = qs("SELECT group_concat(value ->> 'op' || ':' || COALESCE(value ->> 'call_id', ''), ',')"
                   " FROM json_each(?1, '$.edits')", out, NULL);
    if (!ops || strcmp(ops, "drop:toolu_grep,replace:toolu_make,note:") != 0) FAIL(out);
    free(ops);

    char *err = NULL;
    TurnEditResult r = turn_edit_apply(g_db, t.sid, out, "hook:turn-review", &err);
    if (r != TURN_EDIT_APPLIED) FAIL(err ? err : "not applied");
    free(err);
    free(out);

    /* The excerpt: a header saying what happened, kept lines verbatim, one
     * marker per gap; each names the original entry. */
    char want[1024];
    snprintf(want, sizeof(want),
        "[turn-review: this result was shortened after the turn ended. You read all 300 lines"
        " before you answered; the full text is entries.id=%lld]\n"
        "make line 1 of the output, padded to a realistic width\n"
        "make line 2 of the output, padded to a realistic width\n"
        "[lines 3-149 omitted, see entries.id=%lld]\n"
        "make line 150 of the output, padded to a realistic width\n"
        "make line 151 of the output, padded to a realistic width\n"
        "make line 152 of the output, padded to a realistic width\n"
        "[lines 153-299 omitted, see entries.id=%lld]\n"
        "make line 300 of the output, padded to a realistic width",
        (long long)t.sh_res, (long long)t.sh_res, (long long)t.sh_res);
    char sql[512];
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE b(id, parent_id) AS (SELECT id, parent_id FROM entries WHERE id=%lld"
        " UNION ALL SELECT e.id, e.parent_id FROM entries e JOIN b ON e.id=b.parent_id)"
        " SELECT content FROM entries WHERE id IN (SELECT id FROM b) AND type='tool_result'"
        " AND tool_call_id='toolu_make'", (long long)t.fin);
    char *got = qs(sql, NULL, NULL);
    if (!got || strcmp(got, want) != 0) FAIL(got ? got : "no shell result on the branch");
    free(got);

    /* The original is still in the table; the grep pair is off the branch. */
    assert(qi("SELECT length(content) > 10000 FROM entries WHERE id=?", t.sh_res) == 1);
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE b(id, parent_id) AS (SELECT id, parent_id FROM entries WHERE id=%lld"
        " UNION ALL SELECT e.id, e.parent_id FROM entries e JOIN b ON e.id=b.parent_id)"
        " SELECT group_concat(tool_name, ',') FROM (SELECT e.tool_name FROM entries e JOIN b"
        " ON b.id=e.id WHERE e.type='tool_call' ORDER BY e.id)", (long long)t.fin);
    got = qs(sql, NULL, NULL);
    if (!got || strcmp(got, "shell_exec,file_write,js_eval") != 0) FAIL(got ? got : "?");
    free(got);

    /* The note: the model's sentence, then where the dropped result is. */
    snprintf(want, sizeof(want), "old_name is not used anywhere. After this turn ended, these"
        " calls were removed from it as no longer needed: file_grep (entries.id=%lld). Their"
        " results are still in the database.",
        (long long)t.grep_res);
    got = qs("SELECT content FROM entries WHERE session_id=?1 AND type='system'"
             " AND data ->> 'source' = 'hook:turn-review'",
             (snprintf(sql, sizeof(sql), "%lld", (long long)t.sid), sql), NULL);
    if (!got || strcmp(got, want) != 0) FAIL(got ? got : "no note");
    free(got);

    /* The record is on the final assistant entry, the script's part inside. */
    got = qs("SELECT (data ->> '$.turn_edits[0].by') || '|' ||"
             " (data ->> '$.turn_edits[0].record.reviewer') || '|' ||"
             " (data ->> '$.turn_edits[0].record.dropped[0].tool') || '|' ||"
             " (data ->> '$.turn_edits[0].record.trimmed[0].lines') || '|' ||"
             " (data -> '$.turn_edits[0].record.trimmed[0].kept') || '|' ||"
             " ((data ->> '$.turn_edits[0].tokens.after') < (data ->> '$.turn_edits[0].tokens.before') / 4)"
             " FROM entries WHERE id=?1",
             (snprintf(sql, sizeof(sql), "%lld", (long long)t.fin), sql), NULL);
    if (!got || strcmp(got, "hook:turn-review|m-util|file_grep|300|[[1,2],[150,152],[300,300]]|1") != 0)
        FAIL(got ? got : "no record");
    free(got);
    PASS();
}

/* A turn under the threshold, or over the ceiling, is left alone and the
 * model is never asked. */
static void test_skips_without_asking(void) {
    TEST("small_or_huge_turn_is_skipped_without_a_model_call");
    Turn small = seed_turn("small", 0), big = seed_turn("big", 1);
    struct { const Turn *t; const char *config; } cases[] = {
        { &small, NULL },                                   /* under 4000 tool tokens */
        { &big, "{\"max_turn_tokens\":\"1000\"}" },         /* over the ceiling */
        { &big, "{\"min_tool_tokens\":\"900000\"}" },       /* threshold raised by config */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        int is_error = 0;
        char *asked = NULL;
        char *out = review(cases[i].t, "{\"drop\":[\"c1\"]}", cases[i].config, &is_error, &asked);
        if (is_error || strcmp(out, "{}") != 0 || asked) { printf("(case %zu) ", i); FAIL(out); }
        free(out);
    }
    /* The same big turn with default settings is reviewed. */
    int is_error = 0;
    char *out = review(&big, "{\"drop\":[\"c1\"],\"trim\":[],\"note\":\"\"}", NULL, &is_error, NULL);
    if (is_error || !strstr(out, "\"op\":\"drop\"")) FAIL(out);
    free(out);
    PASS();
}

/* Anything doubtful in the model's answer: no edits at all. */
static void test_bad_answers_change_nothing(void) {
    TEST("bad_model_answers_return_no_edits");
    Turn t = seed_turn("bad", 1);
    char long_note[700];
    memset(long_note, 'x', sizeof(long_note) - 1);
    long_note[sizeof(long_note) - 1] = '\0';
    char note_case[900];
    snprintf(note_case, sizeof(note_case), "{\"drop\":[\"c1\"],\"note\":\"%s\"}", long_note);
    const char *bad[] = {
        "I think the grep can go.",                                    /* not JSON */
        "{\"drop\":[\"c1\"",                                           /* cut off */
        "[\"c1\"]",                                                    /* not an object */
        "{\"drop\":\"c1\"}",                                           /* drop not a list */
        "{\"drop\":[\"c9\"]}",                                         /* no such call */
        "{\"drop\":[\"toString\"]}",                                   /* not a label */
        "{\"drop\":[\"c3\"]}",                                         /* protected */
        "{\"drop\":[\"c1\",\"c1\"]}",                                  /* twice */
        "{\"drop\":[\"c2\"],\"trim\":[{\"call\":\"c2\",\"keep\":[[1,2]]}]}",   /* both */
        "{\"drop\":[\"c1\"],\"trim\":[{\"call\":\"c2\",\"keep\":[[1,301]]}]}", /* past the end */
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[[0,5]]}]}",             /* line 0 */
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[[5,2]]}]}",             /* backwards */
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[[1,10],[10,20]]}]}",    /* overlap */
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[[20,30],[1,5]]}]}",     /* not ascending */
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[[1.5,3]]}]}",           /* not whole */
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[[\"1\",\"3\"]]}]}",     /* strings */
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[]}]}",                  /* keeps nothing */
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[[1,2,3]]}]}",           /* not a pair */
        "{\"trim\":[\"c2\"]}",                                         /* not an object */
        "{\"drop\":[\"c1\"],\"note\":7}",                              /* note not text */
        note_case,                                                     /* note too long */
        /* Well-formed, but there is nothing to do. */
        "{\"drop\":[],\"trim\":[],\"note\":\"\"}",
        "{\"drop\":[],\"trim\":[],\"note\":\"a note with no edit is not written\"}",
        "{\"trim\":[{\"call\":\"c2\",\"keep\":[[1,300]]}]}",           /* keeps everything */
        "{\"trim\":[{\"call\":\"c4\",\"keep\":[[1,1]]}]}",             /* one-line result */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        int is_error = 0;
        char *asked = NULL;
        char *out = review(&t, bad[i], NULL, &is_error, &asked);
        if (is_error || strcmp(out, "{}") != 0 || !asked) {
            printf("(%.60s) ", bad[i]);
            FAIL(out);
        }
        free(out);
        free(asked);
    }
    /* And "{}" is, to the runtime, a turn left alone. */
    char *err = NULL;
    assert(turn_edit_apply(g_db, t.sid, "{}", "hook:turn-review", &err) == TURN_EDIT_NOTHING);
    free(err);
    PASS();
}

/* The model cannot be reached: the script throws, so the run counts as a
 * failed hook (the runtime's business) and nothing is returned to apply. */
static void test_model_failure_is_an_error(void) {
    TEST("model_failure_is_a_failed_run");
    Turn t = seed_turn("down", 1);
    int is_error = 0;
    char *out = review(&t, NULL, NULL, &is_error, NULL);
    if (!is_error || !strstr(out, "every candidate failed")) FAIL(out);
    free(out);
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_turn_review_ext:\n");
    unlink(DB_PATH); unlink(DB_PATH "-wal"); unlink(DB_PATH "-shm");
    mkdir(BRIDGE_DIR, 0755);
    g_db = test_db_open(DB_PATH);
    assert(g_db);
    setenv("TEST_LLM_KEY", "k-test", 1);
    /* The chat model is unreachable on purpose: the script must go to the
     * utility list. Tool facts as tools_sync_to_db leaves them. */
    run("INSERT INTO agents(name) VALUES('t');"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env) VALUES"
        " ('chat','http://127.0.0.1:1/v1','openai','TEST_LLM_KEY'),"
        " ('util','http://127.0.0.1:1/v1','openai','TEST_LLM_KEY');"
        "INSERT INTO models(id, provider_name, model) VALUES"
        " ('m-chat','chat','dear-model'), ('m-util','util','cheap-model');"
        "INSERT INTO agent_models(agent_name, purpose, model_id, pos) VALUES"
        " ('t','chat','m-chat',0), ('t','utility','m-util',0);"
        "INSERT OR REPLACE INTO tools(name, droppable) VALUES"
        " ('shell_exec',1),('js_eval',1),('file_grep',1),('file_write',0);");

    test_review_applies();
    test_skips_without_asking();
    test_bad_answers_change_nothing();
    test_model_failure_is_an_error();

    sqlite3_close(g_db);
    unlink(DB_PATH); unlink(DB_PATH "-wal"); unlink(DB_PATH "-shm");
    rmdir(BRIDGE_DIR);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
