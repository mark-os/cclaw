/* --doctor's [compaction] section (src/doctor.c): per agent, which
 * compaction it gets — the bundled script, another script, or the fallback
 * only — and its newest compaction; a hook turned off after three failures,
 * with how to turn it back on. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"
#include "doctor.h"
#include "test_util.h"

#define DB "test_doctor.db"

static sqlite3 *g_db;

static void exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "exec failed: %s\n%s\n", err, sql);
        assert(0);
    }
}

/* The section as text (heap). */
static char *section(void) {
    char *buf = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    assert(f);
    doctor_check_compaction(g_db, "/srv/cclaw.db", f);
    fclose(f);
    printf("%s", buf);
    return buf;
}

static void has(const char *out, const char *needle) {
    if (!strstr(out, needle)) fprintf(stderr, "missing [%s] in:\n%s\n", needle, out);
    assert(strstr(out, needle));
}

static void lacks(const char *out, const char *needle) {
    if (strstr(out, needle)) fprintf(stderr, "unexpected [%s] in:\n%s\n", needle, out);
    assert(!strstr(out, needle));
}

static void test_compaction(void) {
    /* Bundled: the binary's `compaction` (no owner). Other: an agent's own
     * published script. Fallback: nothing attached, or a hook that is async
     * (its reply is ignored, so it compacts nothing). */
    exec("INSERT INTO agents(name) VALUES ('Ann'), ('Bob'), ('Cid'), ('Dee');"
         "INSERT INTO extensions(name, path, owner_agent, published) VALUES"
         " ('compaction', '/s/compaction', NULL, 1), ('squash', '/s/squash', 'Bob', 1),"
         " ('watch', '/s/watch', NULL, 1);"
         "INSERT INTO hooks(extension_name, event, path) VALUES"
         " ('compaction', 'ContextThreshold', '/s/compaction/compact.qjs'),"
         " ('squash', 'ContextThreshold', '/s/squash/squash.qjs');"
         "INSERT INTO hooks(extension_name, event, path, async) VALUES"
         " ('watch', 'ContextThreshold', '/s/watch/watch.qjs', 1);"
         "INSERT INTO agent_extensions(agent_name, extension_name) VALUES"
         " ('Ann', 'compaction'), ('Bob', 'squash'), ('Cid', 'watch'), ('Dee', 'compaction');"
         "INSERT INTO sessions(id, name, agent_name) VALUES (7, 's', 'Ann');"
         /* Two compactions on Ann's session; the newer is reported. A
          * Stop hook's edit is not a compaction. */
         "INSERT INTO entries(id, session_id, type, role, content, data) VALUES"
         " (70, 7, 'system', 0, 'n', json_object('turn_edits', json_array(json_object("
         "   'by','fallback','at',1763300000,'tokens',json_object('before',90000,'after',30000))))),"
         " (71, 7, 'assistant_message', 2, 'a', json_object('turn_edits', json_array(json_object("
         "   'by','hook:compaction','at',1763300500,'tokens',json_object('before',70000,'after',20000))))),"
         " (72, 7, 'assistant_message', 2, 'b', json_object('turn_edits', json_array(json_object("
         "   'by','hook:review','at',1763309999,'tokens',json_object('before',1,'after',1)))));");
    char *out = section();
    has(out, "[compaction]");
    has(out, "ok   Ann: the bundled script");
    has(out, "70000 -> 20000 tokens (session 7)");
    lacks(out, "90000");
    has(out, "ok   Bob: another script: squash");
    has(out, "ok   Cid: the fallback only");
    has(out, "last: none yet");
    has(out, "ok   hooks: none turned off");
    free(out);

    /* Dee's bundled hook trips: Ann and Dee fall back, with a warning, and
     * the hook is listed with the way back. A failing but live hook says so. */
    exec("UPDATE hooks SET disabled = 1, fail_count = 3 WHERE extension_name = 'compaction';"
         "UPDATE hooks SET fail_count = 2 WHERE extension_name = 'squash';");
    out = section();
    has(out, "WARN Ann: the fallback only");
    has(out, "compaction is turned off");
    has(out, "WARN Dee: the fallback only");
    has(out, "its script failed the last 2 times");
    has(out, "hooks turned off after 3 failures in a row:");
    has(out, "compaction  ContextThreshold  compact.qjs");
    has(out, "turn it back on");
    has(out, "cclaw sqlite3 --write /srv/cclaw.db \"UPDATE hooks SET disabled=0, fail_count=0 WHERE extension_name='compaction'"
             " AND event='ContextThreshold' AND path='/s/compaction/compact.qjs'\"\n");
    /* The bundle shipped in the binary has no owner to promote it. */
    lacks(out, "promote");
    free(out);
    exec("UPDATE hooks SET disabled = 1 WHERE extension_name = 'squash';");
    out = section();
    has(out, "squash  ContextThreshold  squash.qjs");
    has(out, "AND path='/s/squash/squash.qjs'\"; or promote the extension again");
    free(out);

    char *buf = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    doctor_check_compaction(NULL, NULL, f);
    fclose(f);
    has(buf, "skipped: no DB handle");
    free(buf);
}

int main(void) {
    TEST_INIT();
    test_db_clean(DB);
    g_db = test_db_open(DB);
    assert(g_db);
    RUN(test_compaction);
    db_close(g_db);
    test_db_clean(DB);
    printf("All doctor tests passed.\n");
    return 0;
}
