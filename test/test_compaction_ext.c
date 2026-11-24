/* extensions/compaction/compact.qjs, the shipped file, end to end without a
 * network: a session seeded in a temp DB -> turn_edit_input with the
 * manifest's own input and the numbers settle.c sends -> the script in the
 * real QJS engine, its LLM() served by the real bridge from the agent's chat
 * list (a loopback mock plays the provider) -> the script's reply ->
 * turn_edit_apply -> the session measured again. Covers what the script
 * decides: which turns or steps it takes, that the actions taken are listed
 * by it and carried across a fold, that an earlier summary is folded rather
 * than stacked, and that a model failure or a hopeless session changes
 * nothing. Also that the binary embeds this very file. */
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
#include <time.h>
#include <unistd.h>
#include "buf.h"
#include "cache_expiring.h"
#include "db.h"
#include "extension_manifest.h"
#include "llm_bridge.h"
#include "qjs_helpers.h"
#include "templates.h"
#include "test_util.h"
#include "turn_edit.h"
#include "util.h"

#define DB_PATH    "/tmp/test_compaction_ext.db"
#define BRIDGE_DIR "/tmp/test_compaction_ext_bridge"
#define BUNDLE     "extensions/compaction"
#define SCRIPT     BUNDLE "/compact.qjs"
#define EFFECTS    "Actions already taken in the part of the conversation this replaces"

static sqlite3 *g_db;

/* ── a provider on loopback: reads one whole request, answers once ── */

typedef struct {
    int listen_fd, port, served;
    char *response;
    char *request;
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
        b.len--;
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

static int64_t qn2(const char *sql, const char *a, const char *b) {
    sqlite3_stmt *s = prep(sql);
    if (a) sqlite3_bind_text(s, 1, a, -1, SQLITE_STATIC);
    if (b) sqlite3_bind_text(s, 2, b, -1, SQLITE_STATIC);
    int64_t v = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    return v;
}
#define qn(sql, a) qn2(sql, a, NULL)

static void run(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql: %s\n  %s\n", err ? err : "?", sql);
        abort();
    }
}

/* A mock whose completion text is `said` (200), or a 500 when NULL. */
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
                               " WHERE name='chat'", m->port);
    run(sql);
    assert(pthread_create(tid, NULL, mock_thread, m) == 0);
}

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

/* ── sessions ── */

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

static void user(int64_t sid, const char *text) {
    Message u = { .role = ROLE_USER, .content = (char *)text };
    assert(entry_append_with_iteration(g_db, sid, &u, 0) > 0);
}

/* About 1000 tokens of shell output, tagged so it can be found in a prompt. */
static char *output(const char *tag) {
    Buf b = {0};
    for (int i = 0; i < 60; i++)
        buf_appendf(&b, "%s line %02d of the build output, padded to a width\n", tag, i);
    return buf_take(&b);
}

/* One step: a word from the agent, a shell call with a long result, and —
 * when `write` — a file write (protected: made for its effect). */
static void step(int64_t sid, const char *tag, int write) {
    int64_t it = db_next_iteration_id(g_db, sid);
    char said[64], cid[64], wid[64], wargs[96], wres[64];
    snprintf(said, sizeof(said), "Checking %s.", tag);
    snprintf(cid, sizeof(cid), "sh_%s", tag);
    snprintf(wid, sizeof(wid), "wr_%s", tag);
    snprintf(wargs, sizeof(wargs), "{\"path\":\"%s.c\",\"content\":\"fixed\"}", tag);
    snprintf(wres, sizeof(wres), "wrote 5 bytes to %s.c", tag);
    add(sid, it, "assistant_message", said, NULL, NULL);
    add(sid, it, "tool_call", "{\"command\":\"make\"}", cid, "shell_exec");
    if (write) add(sid, it, "tool_call", wargs, wid, "file_write");
    char *out = output(tag);
    add(sid, it, "tool_result", out, cid, "shell_exec");
    free(out);
    if (write) add(sid, it, "tool_result", wres, wid, "file_write");
}

/* A finished turn: a question, one step with a write, the answer. */
static void turn(int64_t sid, const char *tag) {
    char q[64], a[64];
    snprintf(q, sizeof(q), "please fix %s", tag);
    snprintf(a, sizeof(a), "Fixed %s.", tag);
    user(sid, q);
    step(sid, tag, 1);
    add(sid, db_next_iteration_id(g_db, sid), "assistant_message", a, NULL, NULL);
}

/* ── running the script ── */

static char *g_manifest_input;   /* the shipped hook's "input", as stored */

/* The input settle.c would build for this session right now, with the trim
 * numbers set on the agent's routing entry first. NULL when not over. */
static char *hook_args(int64_t sid, int threshold, int target, int *tokens) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE agent_models SET trim_threshold=%d, prune_target=%d"
                               " WHERE agent_name='t' AND purpose='chat'", threshold, target);
    run(sql);
    ContextLimits lim;
    int total = turn_edit_measure(g_db, sid, "t", &lim);
    if (tokens) *tokens = total;
    if (total <= lim.threshold) return NULL;
    char ctx[192];
    snprintf(ctx, sizeof(ctx), "{\"tokens\":%d,\"window\":%d,\"trim_threshold\":%d,"
             "\"prune_target\":%d}", total, lim.window, lim.threshold, lim.target);
    TurnEditInputSpec spec = {
        .history = TURN_EDIT_HISTORY_ALL,
        .history_results = (int)qn("SELECT json_extract(?1,'$.history_results')", g_manifest_input),
        .max_bytes = (int)qn("SELECT json_extract(?1,'$.max_bytes')", g_manifest_input),
    };
    TurnEditInputFor what = { .event = "ContextThreshold", .need_answer = 0, .context = ctx };
    char *a = turn_edit_input(g_db, sid, &spec, &what, NULL);
    assert(a);
    return a;
}

/* The input settle.c would build for CacheExpiring on this idle session:
 * the trim numbers set first, the session's depth set, its context from
 * cache_expiring_context. NULL when it is not above its prune target. */
static char *cache_args(int64_t sid, int threshold, int target, int depth, int *tokens) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE agent_models SET trim_threshold=%d, prune_target=%d,"
                               " cache_lifetime=3600 WHERE agent_name='t' AND purpose='chat'",
             threshold, target);
    run(sql);
    snprintf(sql, sizeof(sql), "UPDATE sessions SET depth=%d WHERE id=%lld", depth,
             (long long)sid);
    run(sql);
    ContextLimits lim;
    int total = turn_edit_measure(g_db, sid, "t", &lim);
    if (tokens) *tokens = total;
    if (total <= lim.target) return NULL;
    char *ctx = cache_expiring_context(g_db, sid, time(NULL), total, &lim);
    assert(ctx);
    TurnEditInputSpec spec = {
        .history = TURN_EDIT_HISTORY_ALL,
        .history_results = (int)qn("SELECT json_extract(?1,'$.history_results')", g_manifest_input),
        .max_bytes = (int)qn("SELECT json_extract(?1,'$.max_bytes')", g_manifest_input),
    };
    TurnEditInputFor what = { .event = "CacheExpiring", .need_answer = 0, .context = ctx };
    char *a = turn_edit_input(g_db, sid, &spec, &what, NULL);
    free(ctx);
    assert(a);
    return a;
}

/* Run the shipped script on `args`. `said` is the summariser's answer (NULL:
 * the provider fails). *asked gets the request the provider saw, NULL when
 * none was sent. */
static char *compact(int64_t sid, const char *args, const char *said, int *is_error,
                     char **asked) {
    Mock m;
    pthread_t tid;
    mock_start(&m, &tid, said);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "hook:compaction", sid, 0);
    assert(b);
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    *is_error = 0;
    char *out = qjs_eval_run(NULL, SCRIPT, args, NULL, is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    int served = m.served;
    char *request = m.request;
    mock_stop(&m, tid);
    if (asked) *asked = served ? request : NULL;
    else if (served) free(request);
    return out;
}

static int64_t summaries_on_branch(int64_t sid) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE b(id) AS (SELECT leaf_id FROM sessions WHERE id=%lld"
        " UNION ALL SELECT e.parent_id FROM entries e JOIN b ON e.id=b.id)"
        " SELECT COUNT(*) FROM entries e JOIN b ON b.id=e.id WHERE e.type='compaction'",
        (long long)sid);
    return qn(sql, NULL);
}

static char *summary_on_branch(int64_t sid) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE b(id) AS (SELECT leaf_id FROM sessions WHERE id=%lld"
        " UNION ALL SELECT e.parent_id FROM entries e JOIN b ON e.id=b.id)"
        " SELECT e.content FROM entries e JOIN b ON b.id=e.id WHERE e.type='compaction'",
        (long long)sid);
    return qs(sql, NULL, NULL);
}

/* How many entries on the session's branch have this content. */
static int64_t on_branch(int64_t sid, const char *content) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "WITH RECURSIVE b(id) AS (SELECT leaf_id FROM sessions WHERE id=%lld"
        " UNION ALL SELECT e.parent_id FROM entries e JOIN b ON e.id=b.id)"
        " SELECT COUNT(*) FROM entries e JOIN b ON b.id=e.id WHERE e.content=?1",
        (long long)sid);
    return qn(sql, content);
}

/* What it takes for the history alone to be enough: the gap between the
 * session and its target is most of the earlier turns, not all of them. */
static int history_tokens(const char *args) {
    return (int)qn("SELECT SUM(value -> '$.tokens' ->> 'turn') FROM json_each(?1, '$.history')"
                   " WHERE value -> 'turn_id' IS NOT NULL", args);
}

/* ── tests ── */

static void test_embedded(void) {
    char *file = util_read_file(SCRIPT, NULL), *man = util_read_file(BUNDLE "/extension.json", NULL);
    assert(file && strcmp(file, TPL_EXT_COMPACTION_COMPACT_QJS) == 0);
    assert(man && strcmp(man, TPL_EXT_COMPACTION_EXTENSION_JSON) == 0);
    char *err = NULL;
    assert(extension_manifest_validate(BUNDLE, &err) == 0);
    /* A ContextThreshold and a CacheExpiring hook, both the same script,
     * both waited for, both over all history (sized by the script, not left
     * at the default). */
    assert(!(qn("SELECT json_array_length(?1,'$.hooks')=2"
           " AND json_extract(?1,'$.hooks[0].event')='ContextThreshold'"
           " AND json_extract(?1,'$.hooks[1].event')='CacheExpiring'"
           " AND json_extract(?1,'$.hooks[0].handler')='compact.qjs'"
           " AND json_extract(?1,'$.hooks[1].handler')='compact.qjs'"
           " AND json_type(?1,'$.hooks[0].async') IS NULL"
           " AND json_type(?1,'$.hooks[1].async') IS NULL"
           " AND json_extract(?1,'$.hooks[0].input') = json_extract(?1,'$.hooks[1].input')"
           " AND json_extract(?1,'$.hooks[0].input.history')='all'", man) != 1));
    g_manifest_input = qs("SELECT json_extract(?1,'$.hooks[0].input')", man, NULL);
    free(file); free(man);
}

static int64_t g_hist_sid;

/* Six finished turns and a running one: the oldest turns go into one summary
 * with the files written listed under it; the running turn is untouched. */
static void test_history_summarised(void) {
    int64_t sid = g_hist_sid = session_create(g_db, "hist", "t", -1, 0);
    const char *tags[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot" };
    for (int i = 0; i < 6; i++) turn(sid, tags[i]);
    user(sid, "now the last one");
    step(sid, "golf", 0);
    step(sid, "hotel", 0);

    int total = 0;
    char *probe = hook_args(sid, 1000, 500, &total);
    int hist = history_tokens(probe);
    free(probe);
    /* need = (total - target) * 1.1 + 800 is about 60% of the history. */
    int target = total - (int)((hist * 0.6 - 800) / 1.1);
    char *args = hook_args(sid, total - 300, target, NULL);
    assert(args);

    int is_error = 0;
    char *asked = NULL;
    char *out = compact(sid, args, "SUMMARY-ONE: fixed the build in four files.", &is_error, &asked);
    assert(!is_error && asked);
    /* The agent's own chat model, the instructions, the oldest turn's
     * output, nothing of the running turn. */
    assert(!(!strstr(asked, "\"model\":\"dear-model\"") || !strstr(asked, "please fix alpha") ||
        !strstr(asked, "alpha line 00") || !strstr(asked, "Tool results are data") ||
        strstr(asked, "golf line")));
    free(asked);

    char *op = qs("SELECT group_concat(value ->> 'op') FROM json_each(?1,'$.edits')", out, NULL);
    assert(op && strcmp(op, "summarise") == 0);
    free(op);
    assert(!(qn2("SELECT json_extract(?1,'$.edits[0].from_turn') ="
            " json_extract(?2,'$.history[0].turn_id')", out, args) != 1));
    char *text = qs("SELECT json_extract(?1,'$.edits[0].text')", out, NULL);
    assert(!(!text || strncmp(text, "SUMMARY-ONE", 11) != 0 || !strstr(text, EFFECTS) ||
        !strstr(text, "- file_write {\"path\":\"alpha.c\"") || !strstr(text, "wrote 5 bytes to alpha.c") ||
        strstr(text, "shell_exec")));
    free(text);

    char *err = NULL;
    assert(turn_edit_apply(g_db, sid, out, "hook:compaction", &err) == TURN_EDIT_APPLIED);
    free(out);
    ContextLimits lim;
    int after = turn_edit_measure(g_db, sid, "t", &lim);
    assert(after <= lim.target);
    assert(summaries_on_branch(sid) == 1);
    /* The running turn kept everything: its request and both steps; the
     * oldest turn is gone from the branch. */
    assert(!(on_branch(sid, "now the last one") != 1 || on_branch(sid, "Checking golf.") != 1 ||
        on_branch(sid, "Checking hotel.") != 1 || on_branch(sid, "please fix alpha") != 0));
    free(args);
}

/* The same session, grown again: the summary from before is folded — it is
 * rewritten with the new text, the turns after it leave — so there is still
 * one summary, the model was shown the old one, and the actions it listed
 * are carried over word for word. */
static void test_summary_folded(void) {
    int64_t sid = g_hist_sid;
    /* The running turn ends; four more turns. */
    add(sid, db_next_iteration_id(g_db, sid), "assistant_message", "Done.", NULL, NULL);
    const char *tags[] = { "india", "juliet", "kilo", "lima" };
    for (int i = 0; i < 4; i++) turn(sid, tags[i]);
    user(sid, "and one more");
    step(sid, "mike", 0);

    int total = 0;
    char *probe = hook_args(sid, 1000, 500, &total);
    int hist = history_tokens(probe);
    free(probe);
    int target = total - (int)((hist * 0.5 - 800) / 1.1);
    char *args = hook_args(sid, total - 300, target, NULL);
    assert(args);
    char *old = summary_on_branch(sid);
    int64_t old_id = qn("SELECT json_extract(value,'$.summary.entry_id') FROM json_each(?1,'$.history')"
                        " WHERE value -> 'summary' IS NOT NULL", args);
    assert(old && old_id > 0);

    int is_error = 0;
    char *asked = NULL;
    char *out = compact(sid, args, "SUMMARY-TWO: fixed eight files in all.", &is_error, &asked);
    assert(!is_error && asked);
    assert(!(!strstr(asked, "EARLIER SUMMARY") || !strstr(asked, "SUMMARY-ONE")));
    free(asked);

    char *ops = qs("SELECT group_concat(value ->> 'op') FROM json_each(?1,'$.edits')", out, NULL);
    assert(ops && strcmp(ops, "rewrite,drop_turns") == 0);
    free(ops);
    assert(!(qn("SELECT json_extract(?1,'$.edits[0].entry_id')", out) != old_id));

    char *err = NULL;
    assert(turn_edit_apply(g_db, sid, out, "hook:compaction", &err) == TURN_EDIT_APPLIED);
    free(out);
    assert(summaries_on_branch(sid) == 1);
    char *now = summary_on_branch(sid);
    /* New prose, the old actions carried, the new ones added; no trace of
     * the old prose (the model folded it), and the header once. */
    assert(!(!now || strncmp(now, "SUMMARY-TWO", 11) != 0 || strstr(now, "SUMMARY-ONE") ||
        !strstr(now, "alpha.c") || !strstr(now, "echo.c") ||
        strstr(strstr(now, EFFECTS) + 1, EFFECTS)));
    free(now); free(old); free(args);
    ContextLimits lim;
    assert(turn_edit_measure(g_db, sid, "t", &lim) <= lim.target);
}

/* One long turn and nothing before it: the oldest steps go, the user's
 * request and the newest step stay; done twice, the second summary takes in
 * the first. */
static void test_running_turn(void) {
    int64_t sid = session_create(g_db, "long", "t", -1, 0);
    user(sid, "rebuild everything");
    const char *tags[] = { "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8" };
    for (int i = 0; i < 8; i++) step(sid, tags[i], i == 1);

    int total = 0;
    char *args = hook_args(sid, 1000, 500, &total);
    free(args);
    /* Half the steps' worth has to go. */
    args = hook_args(sid, total - 300, total - 4 * 1050, NULL);
    assert(args);
    int64_t newest = qn("SELECT json_extract(?1,'$.iterations[#-1].iteration_id')", args);
    int is_error = 0;
    char *asked = NULL;
    char *out = compact(sid, args, "STEPS-ONE: ran make in s1 to s4.", &is_error, &asked);
    assert(!is_error && asked);
    assert(!(!strstr(asked, "THE REQUEST BEING WORKED ON") || !strstr(asked, "rebuild everything") ||
        !strstr(asked, "s1 line 00") || strstr(asked, "s8 line")));
    free(asked);
    assert(!(qn2("SELECT json_extract(?1,'$.edits[0].op')='summarise'"
            " AND json_extract(?1,'$.edits[0].from_iteration')="
            "     json_extract(?2,'$.iterations[0].iteration_id')"
            " AND json_array_length(?1,'$.edits')=1", out, args) != 1));
    assert(!(qn("SELECT json_extract(?1,'$.edits[0].to_iteration')", out) == newest));
    char *text = qs("SELECT json_extract(?1,'$.edits[0].text')", out, NULL);
    assert(!(!text || !strstr(text, "- file_write {\"path\":\"s2.c\"")));
    free(text);
    char *err = NULL;
    assert(turn_edit_apply(g_db, sid, out, "hook:compaction", &err) == TURN_EDIT_APPLIED);
    free(out); free(args);
    ContextLimits lim;
    assert(turn_edit_measure(g_db, sid, "t", &lim) <= lim.target);

    /* More steps; again. The first summary stands right before the oldest
     * step left, so it leaves with the run and is folded. */
    const char *more[] = { "s9", "s10", "s11", "s12" };
    for (int i = 0; i < 4; i++) step(sid, more[i], 0);
    char *probe = hook_args(sid, 1000, 500, &total);
    free(probe);
    args = hook_args(sid, total - 300, total - 4 * 1050, NULL);
    assert(args);
    out = compact(sid, args, "STEPS-TWO: ran make in s1 to s9.", &is_error, &asked);
    assert(!(is_error || !asked || !strstr(asked, "STEPS-ONE")));
    free(asked);
    assert(turn_edit_apply(g_db, sid, out, "hook:compaction", &err) == TURN_EDIT_APPLIED);
    free(out); free(args);
    assert(summaries_on_branch(sid) == 1);
    char *now = summary_on_branch(sid);
    assert(!(!now || strncmp(now, "STEPS-TWO", 9) != 0 || !strstr(now, "s2.c")));
    free(now);
    /* The request that opened the turn and the newest step are still there;
     * the oldest steps are not. */
    assert(!(on_branch(sid, "rebuild everything") != 1 || on_branch(sid, "Checking s12.") != 1 ||
        on_branch(sid, "Checking s1.") != 0));
}

/* The model cannot be reached: the script throws — a failed run, the
 * runtime's business — and returns nothing to apply. */
static void test_model_failure(void) {
    int64_t sid = session_create(g_db, "down", "t", -1, 0);
    for (int i = 0; i < 4; i++) turn(sid, "x");
    user(sid, "go");
    step(sid, "y", 0);
    int total = 0;
    char *args = hook_args(sid, 1000, 500, &total);
    int hist = history_tokens(args);
    free(args);
    args = hook_args(sid, total - 300, total - (int)((hist * 0.6 - 800) / 1.1), NULL);
    int is_error = 0;
    char *out = compact(sid, args, NULL, &is_error, NULL);
    assert(!(!is_error || strstr(out, "\"edits\"")));
    free(out); free(args);
    assert(summaries_on_branch(sid) == 0);
}

/* CacheExpiring on an idle top-level session above its prune target: down
 * to half of what is there, and never left above the prune target — the
 * whole of the last turn's steps may go, its answer stays. `num`/`den` set
 * the prune target as a share of the session: just under it (9/10) the goal
 * is half the session; under half of it (9/20) the goal is the target itself. */
static void cache_expiring_case(const char *name, const char *agent_sid, int num, int den) {
    printf("    %s\n", name);
    int64_t sid = session_create(g_db, agent_sid, "t", -1, 0);
    /* Enough earlier turns that they alone cover what has to go: one
     * summary, one model call (the mock answers once). */
    const char *tags[] = { "c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9" };
    for (int i = 0; i < 9; i++) turn(sid, tags[i]);
    user(sid, "the last one");
    step(sid, "d1", 1);
    step(sid, "d2", 0);
    add(sid, db_next_iteration_id(g_db, sid), "assistant_message", "All fixed.", NULL, NULL);

    int total = 0;
    char *probe = cache_args(sid, 150000, 500, 0, &total);
    free(probe);
    /* Above the target, and under the threshold: only CacheExpiring could
     * fire here. */
    int target = total * num / den;
    int goal = target < total / 2 ? target : total / 2;
    char *args = cache_args(sid, total + 1000, target, 0, NULL);
    assert(args);
    assert(!(qn("SELECT json_extract(?1,'$.event')='CacheExpiring'"
           " AND json_extract(?1,'$.context.depth')=0"
           " AND json_extract(?1,'$.context.cache_lifetime')=3600"
           " AND json_extract(?1,'$.final.text')='All fixed.'", args) != 1));
    int is_error = 0;
    char *asked = NULL;
    char *out = compact(sid, args, "IDLE-SUMMARY: fixed c1 to c9.", &is_error, &asked);
    assert(!is_error && asked);
    free(asked);
    char *err = NULL;
    assert(turn_edit_apply(g_db, sid, out, "hook:compaction", &err) == TURN_EDIT_APPLIED);
    free(out); free(args);
    ContextLimits lim;
    int after = turn_edit_measure(g_db, sid, "t", &lim);
    /* Half its size or its prune target, whichever is lower. */
    assert(after <= goal);
    /* The answer and the request it answered stay; the oldest turn left. */
    assert(!(on_branch(sid, "All fixed.") != 1 || on_branch(sid, "please fix c1") != 0));
    /* At or under the target now: the event has nothing to fire on. */
    args = cache_args(sid, total + 1000, target, 0, NULL);
    assert(!args);
}

static void test_cache_expiring_halves(void) {
    cache_expiring_case("cache_expiring_summarises_to_half_the_session", "idle", 9, 10);
    cache_expiring_case("cache_expiring_never_leaves_it_above_the_prune_target", "idle-low", 9, 20);
}

/* Sessions with nothing to do: "{}" without asking the model. */
static void test_no_model_call(void) {
    static const struct { const char *name; } cases[] = {
        /* ContextThreshold: the newest step alone is over the threshold, so
         * nothing it may take gets under it; the runtime's trim decides. */
        { "hopeless" },
        /* CacheExpiring leaves a sub-agent's session alone. */
        { "sub" },
        /* CacheExpiring: nothing it may take removes a quarter of the
         * session (the answer alone is most of it). */
        { "idle-hopeless" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("    %s\n", cases[i].name);
        int64_t sid = session_create(g_db, cases[i].name, "t", -1, 0);
        char *args = NULL;
        if (i == 0) {
            user(sid, "go");
            step(sid, "a", 0);
            step(sid, "b", 0);
            args = hook_args(sid, 400, 200, NULL);
        } else if (i == 1) {
            for (int k = 0; k < 4; k++) turn(sid, "z");
            args = cache_args(sid, 150000, 500, 1, NULL);
        } else {
            user(sid, "write it all out");
            step(sid, "w", 0);
            char *big = output("answer");
            add(sid, db_next_iteration_id(g_db, sid), "assistant_message", big, NULL, NULL);
            free(big);
            args = cache_args(sid, 1000, 500, 0, NULL);
        }
        assert(args);
        int is_error = 0;
        char *asked = NULL;
        char *out = compact(sid, args, "should not be asked", &is_error, &asked);
        assert(!is_error && strcmp(out, "{}") == 0 && !asked);
        free(out); free(args);
    }
}

/* The most the manifest asks for — a session far over what fits, with a
 * window large enough to send it all to the summariser — still runs on the
 * default heap (js_heap_mb 8): the input arrives at its max_bytes, history
 * cut, and the script builds its prompt from it. */
static void test_full_input_fits_the_heap(void) {
    run("UPDATE models SET context_window=1000000 WHERE id='m-chat'");
    int64_t sid = session_create(g_db, "huge", "t", -1, 0);
    Buf big = {0};
    for (int i = 0; i < 1500; i++)
        buf_appendf(&big, "huge line %04d of a very long listing, padded out to a width\n", i);
    char *text = buf_take(&big);
    for (int k = 0; k < 25; k++) {
        char q[32], cid[32];
        snprintf(q, sizeof(q), "turn %d", k);
        snprintf(cid, sizeof(cid), "big_%d", k);
        user(sid, q);
        int64_t it = db_next_iteration_id(g_db, sid);
        add(sid, it, "assistant_message", "Listing.", NULL, NULL);
        add(sid, it, "tool_call", "{\"command\":\"ls -R\"}", cid, "shell_exec");
        add(sid, it, "tool_result", text, cid, "shell_exec");
        add(sid, db_next_iteration_id(g_db, sid), "assistant_message", "Listed.", NULL, NULL);
    }
    free(text);
    user(sid, "and now");
    step(sid, "last", 0);
    int total = 0;
    char *args = hook_args(sid, 1000, 500, &total);
    free(args);
    args = hook_args(sid, total - 300, total / 2, NULL);
    assert(args);
    assert(!(strlen(args) < 1500000 || qn("SELECT json_extract(?1,'$.history_omitted') > 0", args) != 1));
    int is_error = 0;
    char *asked = NULL;
    char *out = compact(sid, args, "HUGE: listed it all.", &is_error, &asked);
    assert(!(is_error || !asked || !strstr(out, "\"summarise\"")));
    free(asked); free(out); free(args);
    run("UPDATE models SET context_window=200000 WHERE id='m-chat'");
}

int main(void) {
    TEST_INIT();
    printf("test_compaction_ext:\n");
    test_db_clean(DB_PATH);
    mkdir(BRIDGE_DIR, 0755);
    g_db = test_db_open(DB_PATH);
    assert(g_db);
    setenv("TEST_LLM_KEY", "k-test", 1);
    /* The utility model is unreachable on purpose: the script must use the
     * agent's own chat list. Tool facts as tools_sync_to_db leaves them. */
    run("INSERT INTO agents(name) VALUES('t');"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env) VALUES"
        " ('chat','http://127.0.0.1:1/v1','openai','TEST_LLM_KEY'),"
        " ('util','http://127.0.0.1:1/v1','openai','TEST_LLM_KEY');"
        "INSERT INTO models(id, provider_name, model, context_window) VALUES"
        " ('m-chat','chat','dear-model',200000), ('m-util','util','cheap-model',200000);"
        "INSERT INTO agent_models(agent_name, purpose, model_id, pos) VALUES"
        " ('t','chat','m-chat',0), ('t','utility','m-util',0);"
        "INSERT OR REPLACE INTO tools(name, droppable) VALUES"
        " ('shell_exec',1),('file_write',0);");

    RUN(test_embedded);
    RUN(test_history_summarised);
    RUN(test_summary_folded);
    RUN(test_running_turn);
    RUN(test_model_failure);
    RUN(test_no_model_call);
    RUN(test_cache_expiring_halves);
    RUN(test_full_input_fits_the_heap);

    sqlite3_close(g_db);
    test_db_clean(DB_PATH);
    rmdir(BRIDGE_DIR);
    return 0;
}
