/* The data explorer's read API (src/explore.c, specs/explorer.md): meta
 * completeness, paging and filters, every key shape, identifier validation,
 * JSONB decoding, the secret walk over every table, and the HTTP surface
 * (auth on every path, read-only, headers). */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config_registry.h"
#include "db.h"
#include "explore.h"
#include "templates.h"
#include "web.h"
#include "test_util.h"

#define DB "test_explore.db"
#define MARK "ZQXMARKER7731"

static sqlite3 *g_db;

static void exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "exec failed: %s\n%s\n", err, sql);
        assert(0);
    }
}

/* Scalar text of `sql` with ?1 bound to `arg` (NULL = no binding). Heap. */
static char *q1(const char *sql, const char *arg) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n%s\n", sqlite3_errmsg(g_db), sql);
        assert(0);
    }
    if (arg) sqlite3_bind_text(st, 1, arg, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        out = strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static long long qi(const char *sql, const char *arg) {
    char *s = q1(sql, arg);
    long long v = s ? atoll(s) : -1;
    free(s);
    return v;
}

static int rows_call(const char *table, const char *filter, const char *after,
                     int limit, char **out) {
    ExploreQuery q = { .table = table, .filter = filter, .after = after, .limit = limit };
    return explore_rows(g_db, &q, out);
}

static int row_call(const char *table, const char *key, char **out) {
    ExploreQuery q = { .table = table, .key = key };
    return explore_row(g_db, &q, out);
}

/* A small but complete world: every key shape, links of every kind. */
static void seed(void) {
    exec("INSERT INTO providers(name, base_url, api_key_env) VALUES"
         " ('openrouter','https://openrouter.example/v1','OR_KEY'),"
         " ('gemini','https://gemini.example','');"
         "INSERT INTO models(id, provider_name, model, capabilities) VALUES"
         " ('m1','openrouter','vendor/one','[\"tools\"]'),"
         " ('m2','openrouter','vendor/two','[\"image\",\"audio\"]'),"
         " ('g1','gemini','gemini-x','[]');"
         "INSERT INTO models_cache(provider_name, id, synced_at) VALUES"
         " ('openrouter','a/1',1700000000),('openrouter','a/2',1700000000);"
         "INSERT INTO agents(name, created_by) VALUES ('alpha', NULL), ('beta','alpha'),"
         " ('gamma', NULL), ('delta', NULL), ('eps', NULL);"
         "INSERT INTO agent_models(agent_name, purpose, model_id, pos) VALUES"
         " ('alpha','chat','m1',0), ('alpha','chat','g1',1), ('alpha','image','m2',0),"
         " ('beta','chat','m1',0);"
         "INSERT INTO tools(name, description) VALUES ('shell_exec','run a command');"
         "INSERT INTO grants(agent_name, kind, value, source) VALUES"
         " ('alpha','tool','shell_exec','operator'), ('alpha','secret','OR_KEY','capture'),"
         " ('alpha','host','api.example.com','approval');"
         "INSERT INTO channels(name, status) VALUES ('discord','active');"
         "INSERT INTO channel_state(channel_name, key, value) VALUES ('discord','disc_bot_id','42');");
    exec("INSERT INTO sessions(id, name, agent_name) VALUES (1,'one','alpha');"
         "INSERT INTO channel_chats(channel_name, chat_id, session_id, kind) VALUES"
         " ('discord','dm-1',1,'dm'), ('discord','g-2',NULL,'group');"
         "UPDATE sessions SET channel_name='discord', chat_id='dm-1' WHERE id=1;"
         "INSERT INTO sessions(id, name, agent_name, parent_session_id) VALUES (2,'child','beta',1);");
    exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 120)"
         " INSERT INTO entries(session_id, role, content) SELECT 1, 1 + i % 2, 'entry ' || i FROM n;"
         "INSERT INTO entries(session_id, role, content) VALUES (1, 3, printf('%.5000c', 'x'));"
         "INSERT INTO llm_responses(session_id, iteration_id, status, body, request_body) VALUES"
         " (1, 1, 'ok', jsonb('{\"answer\":[1,2,3]}'), x'00ff10');"
         "INSERT INTO memory_blocks(id, created_by, label) VALUES (7, 'alpha', 'team-notes');"
         "INSERT INTO agent_memory_blocks(agent_name, block_id, read_only) VALUES"
         " ('alpha',7,0), ('beta',7,1);"
         "INSERT INTO memory_notes(block_id, text) VALUES (7, 'first'), (7, 'second');");
}

/* ── meta ──────────────────────────────────────────────────────────────── */

static void test_meta_complete(void) {
    char *m = NULL;
    assert(explore_meta(g_db, &m) == 200 && m);
    sqlite3_stmt *st;
    /* Every served table appears, with its count. */
    long long missing = qi(
        "SELECT count(*) FROM pragma_table_list t WHERE t.schema='main' AND t.type='table'"
        " AND t.name NOT LIKE 'sqlite\\_%' ESCAPE '\\' AND NOT EXISTS (SELECT 1 FROM"
        " json_each(?1, '$.tables') j WHERE j.value->>'name' = t.name)", m);
    assert(missing == 0);
    assert(qi("SELECT json_array_length(?1, '$.tables')", m) >= 30);
    assert(qi("SELECT count(*) FROM json_each(?1, '$.tables') WHERE value->>'name' LIKE"
              " 'entries\\_fts%' ESCAPE '\\' OR value->>'name' LIKE 'sqlite%'", m) == 0);
    /* Every column of every table is described, and carries a class key
     * (null = not classified). */
    long long cols_missing = qi(
        "SELECT count(*) FROM pragma_table_list t, pragma_table_info(t.name) c"
        " WHERE t.schema='main' AND t.type='table' AND t.name NOT LIKE 'sqlite\\_%' ESCAPE '\\'"
        " AND NOT EXISTS (SELECT 1 FROM json_each(?1, '$.tables') j, json_each(j.value, '$.columns') k"
        "   WHERE j.value->>'name' = t.name AND k.value->>'name' = c.name"
        "   AND json_type(k.value, '$.class') IS NOT NULL AND k.value->>'type' = c.type"
        "   AND k.value->>'pk' = c.pk AND k.value->>'notnull' = c.\"notnull\")", m);
    assert(cols_missing == 0);
    /* The change log's tables are fully classified. */
    assert(qi("SELECT count(*) FROM json_each(?1, '$.tables') j, json_each(j.value, '$.columns') k"
              " WHERE j.value->>'name' IN ('agents','grants','agent_models','providers','models',"
              " 'secrets','channel_chats') AND k.value->>'class' IS NULL", m) == 0);
    assert(qi("SELECT count(*) FROM json_each(?1, '$.tables') j, json_each(j.value, '$.columns') k"
              " WHERE j.value->>'name' = 'models' AND k.value->>'name' = 'total_requests'"
              " AND k.value->>'class' = 'measurement'", m) == 1);
    /* Every declared FK is a link, both ways. */
    assert(sqlite3_prepare_v2(g_db,
        "SELECT t.name, f.\"table\", f.\"from\" FROM pragma_table_list t,"
        " pragma_foreign_key_list(t.name) f WHERE t.schema='main' AND t.type='table'",
        -1, &st, NULL) == SQLITE_OK);
    int nfk = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        char sql[1024];
        snprintf(sql, sizeof(sql),
            "SELECT (SELECT count(*) FROM json_each(?1, '$.tables') j, json_each(j.value,"
            "  '$.links_out') l WHERE j.value->>'name' = '%s' AND l.value->>'to' = '%s'"
            "  AND EXISTS (SELECT 1 FROM json_each(l.value, '$.from_columns') WHERE value = '%s')"
            "  AND l.value->>'declared' = 1) * 10 +"
            " (SELECT count(*) FROM json_each(?1, '$.tables') j, json_each(j.value,"
            "  '$.links_in') l WHERE j.value->>'name' = '%s' AND l.value->>'from' = '%s'"
            "  AND EXISTS (SELECT 1 FROM json_each(l.value, '$.from_columns') WHERE value = '%s'))",
            sqlite3_column_text(st, 0), sqlite3_column_text(st, 1), sqlite3_column_text(st, 2),
            sqlite3_column_text(st, 1), sqlite3_column_text(st, 0), sqlite3_column_text(st, 2));
        long long v = qi(sql, m);
        if (v < 11) fprintf(stderr, "FK %s.%s -> %s missing (%lld)\n", sqlite3_column_text(st, 0),
                            sqlite3_column_text(st, 2), sqlite3_column_text(st, 1), v);
        assert(v >= 11);
        nfk++;
    }
    sqlite3_finalize(st);
    assert(nfk > 20);
    /* Every hand link names real columns on both sides, and none repeats a
     * declared one. */
    assert(qi("SELECT count(*) FROM json_each(?1, '$.tables') j, json_each(j.value, '$.links_out') l"
              " WHERE l.value->>'declared' = 0", m) >= 20);
    assert(qi("SELECT count(*) FROM json_each(?1, '$.tables') j, json_each(j.value, '$.links_out') l,"
              " json_each(l.value, '$.from_columns') fc"
              " WHERE l.value->>'declared' = 0 AND (NOT EXISTS (SELECT 1 FROM"
              " pragma_table_info(l.value->>'from') WHERE name = fc.value) OR NOT EXISTS (SELECT 1"
              " FROM pragma_table_info(l.value->>'to') WHERE name = l.value->>('$.to_columns[' ||"
              " fc.key || ']')))", m) == 0);
    assert(qi("SELECT count(*) FROM json_each(?1, '$.tables') j, json_each(j.value, '$.links_out') a,"
              " json_each(j.value, '$.links_out') b WHERE a.value->>'declared' = 0"
              " AND b.value->>'declared' = 1 AND a.value->>'to' = b.value->>'to'"
              " AND a.value->'from_columns' = b.value->'from_columns'", m) == 0);
    /* Keys: text, integer, composite, and rowid where the key is hidden. */
    assert(qi("SELECT j.value->>'key' = '[\"name\"]' FROM json_each(?1, '$.tables') j WHERE j.value->>'name' = 'agents'", m) == 1);
    assert(qi("SELECT j.value->>'key' = '[\"agent_name\",\"purpose\",\"pos\"]' FROM json_each(?1, '$.tables') j WHERE j.value->>'name' = 'agent_models'", m) == 1);
    assert(qi("SELECT j.value->>'key' = '[\"rowid\"]' FROM json_each(?1, '$.tables') j WHERE j.value->>'name' = 'shares'", m) == 1);
    assert(qi("SELECT ?1 ->> '$.attention.chats_unrouted'", m) == 1);
    /* Counts are real. */
    assert(qi("SELECT j.value->>'count' FROM json_each(?1, '$.tables') j WHERE j.value->>'name' = 'entries'", m) == 121);
    free(m);
}

/* ── rows ──────────────────────────────────────────────────────────────── */

/* Page through table with filter at limit; returns the number of rows and
 * checks no key repeats. */
static int walk_pages(const char *table, const char *filter, int limit, char **first_page) {
    char *after = NULL;
    int total = 0, pages = 0;
    exec("CREATE TEMP TABLE IF NOT EXISTS seen(k TEXT PRIMARY KEY); DELETE FROM temp.seen;");
    for (;;) {
        char *out = NULL;
        assert(rows_call(table, filter, after, limit, &out) == 200);
        int n = (int)qi("SELECT json_array_length(?1, '$.rows')", out);
        assert(n <= limit);
        sqlite3_stmt *st;
        assert(sqlite3_prepare_v2(g_db, "INSERT INTO temp.seen SELECT value->>'key' FROM"
                                  " json_each(?1, '$.rows')", -1, &st, NULL) == SQLITE_OK);
        sqlite3_bind_text(st, 1, out, -1, SQLITE_STATIC);
        assert(sqlite3_step(st) == SQLITE_DONE);   /* a repeated key fails the PK */
        sqlite3_finalize(st);
        total += n;
        pages++;
        free(after);
        after = q1("SELECT ?1 -> '$.next'", out);
        if (pages == 1 && first_page) *first_page = strdup(out);
        free(out);
        if (!after || strcmp(after, "null") == 0) break;
        assert(n == limit);
    }
    free(after);
    return total;
}

static void test_rows_paging(void) {
    char *first = NULL;
    /* Integer key: newest first, keyset cursor, every row once. */
    assert(walk_pages("entries", "{\"session_id\":1}", 50, &first) == 121);
    assert(qi("SELECT (?1 ->> '$.rows[0].key[0]') > (?1 ->> '$.rows[1].key[0]')", first) == 1);
    assert(qi("SELECT ?1 ->> '$.key[0]' = 'id'", first) == 1);
    /* The long entry is the newest: a preview with its full length. */
    assert(qi("SELECT ?1 ->> '$.rows[0].values.content.length'", first) == 5000);
    assert(qi("SELECT length(?1 ->> '$.rows[0].values.content.preview')", first) == 200);
    free(first);
    /* full: the same row with its text whole — what a page that edits a
     * value asks for, so a preview object never lands in a form. */
    ExploreQuery fq = { .table = "entries", .filter = "{\"session_id\":1}", .limit = 1, .full = 1 };
    char *whole = NULL;
    assert(explore_rows(g_db, &fq, &whole) == 200);
    assert(qi("SELECT json_type(?1, '$.rows[0].values.content') = 'text'", whole) == 1);
    assert(qi("SELECT length(?1 ->> '$.rows[0].values.content')", whole) == 5000);
    free(whole);
    assert(walk_pages("entries", "{\"role\":2}", 7, NULL) == 60);
    assert(walk_pages("entries", "{\"session_id\":\"1\",\"role\":[1,3]}", 200, NULL) == 61);
    assert(walk_pages("entries", "{\"session_id\":2}", 50, NULL) == 0);
    /* Text key ascending, two at a time. */
    assert(walk_pages("agents", NULL, 2, &first) == 5);
    assert(qi("SELECT ?1 ->> '$.rows[0].key[0]' = 'alpha' AND ?1 ->> '$.rows[1].key[0]' = 'beta'", first) == 1);
    free(first);
    /* Composite key. */
    assert(walk_pages("agent_models", NULL, 1, NULL) == 4);
    assert(walk_pages("agent_models", "{\"agent_name\":\"alpha\",\"purpose\":\"chat\"}", 1, NULL) == 2);
    /* NULL filter: the unrouted chat. */
    char *out = NULL;
    assert(rows_call("channel_chats", "{\"session_id\":null}", NULL, 0, &out) == 200);
    assert(qi("SELECT json_array_length(?1, '$.rows') = 1 AND ?1 ->> '$.rows[0].key[1]' = 'g-2'", out) == 1);
    free(out);
    /* Empty table. */
    assert(rows_call("hooks", NULL, NULL, 0, &out) == 200);
    assert(qi("SELECT ?1 ->> '$.rows' = '[]' AND ?1 -> '$.next' = 'null'", out) == 1);
    free(out);
    /* Limit is clamped; a bad cursor is refused. */
    assert(rows_call("entries", NULL, NULL, 100000, &out) == 200);
    assert(qi("SELECT ?1 ->> '$.limit'", out) == EXPLORE_MAX_LIMIT);
    free(out);
    assert(rows_call("entries", NULL, "[1,2]", 0, &out) == 400); free(out);
    assert(rows_call("entries", NULL, "nope", 0, &out) == 400); free(out);
    assert(rows_call("entries", "[1]", NULL, 0, &out) == 400); free(out);
    assert(rows_call("entries", "{\"role\":{\"a\":1}}", NULL, 0, &out) == 400); free(out);
}

/* ── row ───────────────────────────────────────────────────────────────── */

static void test_row_keys(void) {
    char *out = NULL;
    /* Text key, with incoming counts and outgoing links. */
    assert(row_call("agents", "[\"alpha\"]", &out) == 200);
    assert(qi("SELECT ?1 ->> '$.row.values.name' = 'alpha'", out) == 1);
    assert(qi("SELECT value ->> 'count' FROM json_each(?1, '$.links_in') WHERE value->>'from' ="
              " 'sessions' AND value->>'from_columns' = '[\"agent_name\"]'", out) == 1);
    assert(qi("SELECT value ->> 'count' FROM json_each(?1, '$.links_in') WHERE value->>'from' ="
              " 'agent_models'", out) == 3);
    assert(qi("SELECT value ->> 'filter' = '{\"agent_name\":\"alpha\"}' FROM json_each(?1,"
              " '$.links_in') WHERE value->>'from' = 'grants'", out) == 1);
    free(out);
    /* Integer key; the hand links count conversation rows; the parent link
     * resolves; a -1 sentinel parent points nowhere. */
    assert(row_call("sessions", "[2]", &out) == 200);
    assert(qi("SELECT value ->> 'exists' FROM json_each(?1, '$.links_out') WHERE value->>'to' ="
              " 'sessions'", out) == 1);
    free(out);
    assert(row_call("sessions", "[1]", &out) == 200);
    assert(qi("SELECT value ->> 'count' FROM json_each(?1, '$.links_in') WHERE value->>'from' ="
              " 'entries' AND value->>'from_columns' = '[\"session_id\"]'", out) == 121);
    assert(qi("SELECT value ->> 'count' FROM json_each(?1, '$.links_in') WHERE value->>'from' ="
              " 'channel_chats'", out) == 1);
    assert(qi("SELECT value ->> 'exists' FROM json_each(?1, '$.links_out') WHERE value->>'to' ="
              " 'sessions'", out) == 0);
    assert(qi("SELECT value ->> 'exists' FROM json_each(?1, '$.links_out') WHERE value->>'to' ="
              " 'channel_chats'", out) == 1);
    free(out);
    /* Composite key, integer part given as a number or as text. */
    assert(row_call("agent_models", "[\"alpha\",\"image\",0]", &out) == 200);
    assert(qi("SELECT ?1 ->> '$.row.values.model_id' = 'm2'", out) == 1);
    free(out);
    assert(row_call("agent_models", "[\"alpha\",\"chat\",\"1\"]", &out) == 200);
    assert(qi("SELECT ?1 ->> '$.row.values.model_id' = 'g1'", out) == 1);
    free(out);
    assert(row_call("agent_models", "[\"alpha\",\"chat\"]", &out) == 400); free(out);
    assert(row_call("agent_models", "[\"alpha\",\"chat\",9]", &out) == 404); free(out);
    /* Conditional hand links: grants.value is a tool or a secret by kind. */
    assert(row_call("grants", "[\"alpha\",\"tool\",\"shell_exec\"]", &out) == 200);
    assert(qi("SELECT count(*) FROM json_each(?1, '$.links_out') WHERE value->>'to' = 'tools'"
              " AND value->>'exists' = 1", out) == 1);
    assert(qi("SELECT count(*) FROM json_each(?1, '$.links_out') WHERE value->>'to' = 'secrets'", out) == 0);
    free(out);
    assert(row_call("tools", "[\"shell_exec\"]", &out) == 200);
    assert(qi("SELECT value ->> 'filter' = '{\"value\":\"shell_exec\",\"kind\":\"tool\"}'"
              " FROM json_each(?1, '$.links_in') WHERE value->>'from' = 'grants'", out) == 1);
    assert(qi("SELECT value ->> 'count' FROM json_each(?1, '$.links_in') WHERE value->>'from' = 'grants'", out) == 1);
    free(out);
    /* Provider: models and the cache both count. */
    assert(row_call("providers", "[\"openrouter\"]", &out) == 200);
    assert(qi("SELECT value ->> 'count' FROM json_each(?1, '$.links_in') WHERE value->>'from' = 'models_cache'", out) == 2);
    assert(qi("SELECT value ->> 'count' FROM json_each(?1, '$.links_in') WHERE value->>'from' = 'models'", out) == 2);
    free(out);
    /* JSONB comes back as JSON text; another blob as its size. */
    assert(row_call("llm_responses", "[1]", &out) == 200);
    assert(qi("SELECT ?1 ->> '$.row.values.body' = '{\"answer\":[1,2,3]}'", out) == 1);
    assert(qi("SELECT ?1 ->> '$.row.values.request_body.blob'", out) == 3);
    free(out);
    /* The long entry comes back whole on its own page. */
    char key[32];
    snprintf(key, sizeof(key), "[%lld]", qi("SELECT max(id) FROM entries", NULL));
    assert(row_call("entries", key, &out) == 200);
    assert(qi("SELECT length(?1 ->> '$.row.values.content')", out) == 5000);
    free(out);
}

/* ── a session's context and edits (explore_session) ───────────────────── */

/* Records are found on any entry — the trim's note, a tool result a script's
 * edit left as the leaf mid-turn, an answer — newest first, in one shape;
 * the size is the branch the next request carries; the limits are the
 * governing entry's, with what the entry sets beside the defaults. */
static void test_session_edits(void) {
    exec("INSERT INTO sessions(id, name, agent_name) VALUES (20,'long','alpha');"
         "INSERT INTO entries(id, session_id, parent_id, turn_id, type, role, content, tokens) VALUES"
         " (2001, 20,   -1, 1, 'user_message', 1, 'go', 100),"
         " (2002, 20, 2001, 1, 'tool_call', 2, '{}', 50),"
         " (2003, 20, 2002, 1, 'tool_result', 3, 'long output', 4000);"
         /* The trim's note, with its record; a script's stand-in for 2003. */
         "INSERT INTO entries(id, session_id, parent_id, turn_id, type, role, content, tokens, data) VALUES"
         " (2004, 20, 2001, 1, 'system', 0, '1 earlier turn was dropped', 20,"
         "   json_object('source','fallback','edited_by','fallback','turn_edits',json_array(json_object('by','fallback',"
         "     'at',1763300000,'left_branch',json_array(1999),'replaced',json_array(),"
         "     'notes',json_array(2004),'summaries',json_array(),"
         "     'tokens',json_object('before',90000,'after',30000),'thinking_cut',NULL,"
         "     'dropped',json_object('turns',1,'iterations',0),"
         "     'limits',json_object('window',128000,'threshold',64000,'target',32000))))),"
         " (2005, 20, 2002, 1, 'tool_result', 3, 'short', 10,"
         "   json_object('edited_by','hook:compaction','replaces',2003,"
         "     'turn_edits',json_array(json_object('by','hook:compaction','at',1763300100,"
         "       'left_branch',json_array(2003),"
         "       'replaced',json_array(json_object('original',2003,'replacement',2005)),"
         "       'notes',json_array(),'summaries',json_array(),"
         "       'tokens',json_object('before',4150,'after',160),'thinking_cut',2005,"
         "       'record',json_object('why','shorter'))))),"
         /* Not a record: turn_edits that is not an array, and data that is not JSON. */
         " (2006, 20, 2005, 1, 'assistant_message', 2, 'done', 5, '{\"turn_edits\":{\"by\":\"x\"}}'),"
         " (2007, 20, 2006, 2, 'user_message', 1, 'more', 5, 'not json');"
         "UPDATE entries SET original_parent_id = 2002 WHERE id = 2004;"
         "UPDATE sessions SET leaf_id = 2007 WHERE id = 20;");
    char *out = NULL;
    assert(explore_session(g_db, 20, &out) == 200 && out);
    assert(qi("SELECT json_array_length(?1, '$.edits')", out) == 2);
    /* Newest host first: the mid-turn record on a tool result is found. */
    assert(qi("SELECT ?1 ->> '$.edits[0].entry_id' = 2005 AND ?1 ->> '$.edits[0].source' = 'hook:compaction'"
              " AND ?1 ->> '$.edits[0].tokens.before' = 4150 AND ?1 ->> '$.edits[0].tokens.after' = 160"
              " AND ?1 ->> '$.edits[0].replaced[0].original' = 2003"
              " AND ?1 ->> '$.edits[0].thinking_cut' = 2005"
              " AND ?1 ->> '$.edits[0].record.why' = 'shorter'"
              " AND ?1 ->> '$.edits[0].turn_id' = 1", out) == 1);
    assert(qi("SELECT ?1 ->> '$.edits[1].source' = 'fallback' AND ?1 ->> '$.edits[1].dropped.turns' = 1"
              " AND ?1 ->> '$.edits[1].limits.target' = 32000"
              " AND json_type(?1, '$.edits[1].record') = 'null'"
              " AND json_array_length(?1, '$.edits[1].summaries') = 0", out) == 1);
    /* The branch 2007 -> 2006 -> 2005 -> 2002 -> 2001; no report, so no head. */
    assert(qi("SELECT ?1 ->> '$.context.tokens'", out) == 5 + 5 + 10 + 50 + 100);
    assert(qi("SELECT ?1 ->> '$.context.head' = 0 AND ?1 ->> '$.context.head_measured' = 0", out) == 1);
    /* alpha's first chat model (m1) sets nothing: half of the default window. */
    assert(qi("SELECT ?1 ->> '$.context.model' = 'm1' AND ?1 ->> '$.context.window' = 128000"
              " AND ?1 ->> '$.context.threshold' = 64000 AND ?1 ->> '$.context.target' = 32000"
              " AND json_type(?1, '$.context.threshold_set') = 'null'"
              " AND json_type(?1, '$.context.target_set') = 'null'", out) == 1);
    free(out);
    exec("UPDATE agent_models SET trim_threshold = 5000, prune_target = 2000"
         " WHERE agent_name = 'alpha' AND purpose = 'chat' AND pos = 0;");
    assert(explore_session(g_db, 20, &out) == 200);
    assert(qi("SELECT ?1 ->> '$.context.threshold' = 5000 AND ?1 ->> '$.context.threshold_set' = 5000"
              " AND ?1 ->> '$.context.target' = 2000 AND ?1 ->> '$.context.target_set' = 2000", out) == 1);
    free(out);
    exec("UPDATE agent_models SET trim_threshold = NULL, prune_target = NULL WHERE agent_name = 'alpha';");
    /* A session never edited has an empty list; no session is a 404. */
    assert(explore_session(g_db, 2, &out) == 200);
    assert(qi("SELECT json_array_length(?1, '$.edits') = 0 AND ?1 ->> '$.agent' = 'beta'", out) == 1);
    free(out);
    assert(explore_session(g_db, 999, &out) == 404);
    free(out);
}

/* ── prompt-cache statistics (explore_cache) ───────────────────────────── */

/* One field of one model's bucket in a cache response, as text (NULL for a
 * JSON null or no such bucket). Heap. */
static char *cache_cell(const char *out, const char *model, const char *key, const char *field) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT b.value ->> '$.%s' FROM json_each(?1, '$.models') m,"
             " json_each(m.value, '$.buckets') b"
             " WHERE m.value ->> '$.model' = '%s' AND b.value ->> '$.key' = '%s'",
             field, model, key);
    return q1(sql, out);
}

static void cache_expect(const char *out, const char *model, const char *key, const char *field,
                         const char *want) {
    char *got = cache_cell(out, model, key, field);
    int ok = want ? got && strcmp(got, want) == 0 : got == NULL;
    if (!ok) fprintf(stderr, "%s %s %s: got %s want %s\n", model, key, field,
                     got ? got : "null", want ? want : "null");
    assert(ok);
    free(got);
}

/* Gaps are per session (another session's request in between changes
 * nothing), only assistant messages with a usage_in are requests, and a NULL
 * cached_tokens is "not reported", never a zero and never in the share. */
static void test_cache(void) {
    exec("INSERT INTO sessions(id, name, agent_name) VALUES (10,'c1','alpha'), (11,'c2','alpha'),"
         " (12,'c3','alpha');"
         "WITH t0(t) AS (SELECT unixepoch() - 5 * 3600), v(s, ty, r, m, u, c, dt) AS (VALUES"
         "  (10, 'assistant_message', 2, 'cm-a', 1000, NULL,    0),"   /* first, not reported */
         "  (11, 'assistant_message', 2, 'cm-a',  400,  100,   20),"   /* first, hit */
         "  (10, 'assistant_message', 2, 'cm-a', 1000,  900,   30),"   /* 30 s: hit */
         "  (10, 'tool_result',       3, NULL,   NULL, NULL,   40),"   /* not a request */
         "  (10, 'assistant_message', 2, 'cm-a', NULL, NULL,  100),"   /* no usage: not a request */
         "  (10, 'assistant_message', 2, 'cm-a', 1000,    0,  150),"   /* 2 min: reported zero */
         "  (10, 'assistant_message', 2, 'cm-a', 2000, NULL,  750),"   /* 10 min: not reported */
         "  (10, 'assistant_message', 2, 'cm-a', 2000,  500, 4750),"   /* 66 min: hit */
         "  (12, 'assistant_message', 2, 'cm-old', 100,  50, -100 * 86400)"
         " ) INSERT INTO entries(session_id, type, role, model, usage_in, cached_tokens, created_at)"
         " SELECT s, ty, r, m, u, c, t + dt FROM v, t0 ORDER BY dt;");
    char *out = NULL;
    assert(explore_cache(g_db, 0, &out) == 200 && out);
    assert(qi("SELECT json_array_length(?1, '$.buckets') = 5 AND ?1 ->> '$.buckets[0].key' = 'first'"
              " AND ?1 ->> '$.buckets[4].key' = 'gt60m' AND ?1 ->> '$.days' IS NULL", out) == 1);
    assert(qi("SELECT m.value ->> '$.requests' FROM json_each(?1, '$.models') m"
              " WHERE m.value ->> '$.model' = 'cm-a'", out) == 6);
    static const struct { const char *model, *key, *field, *want; } cells[] = {
        { "cm-a", "first", "requests", "2" },       { "cm-a", "first", "not_reported", "1" },
        { "cm-a", "first", "hits", "1" },           { "cm-a", "first", "prompt_tokens", "400" },
        { "cm-a", "lt1m", "requests", "1" },        { "cm-a", "lt1m", "cached_tokens", "900" },
        { "cm-a", "1to5m", "requests", "1" },       { "cm-a", "1to5m", "reported_zero", "1" },
        { "cm-a", "1to5m", "not_reported", "0" },   { "cm-a", "1to5m", "hits", "0" },
        { "cm-a", "5to60m", "requests", "1" },      { "cm-a", "5to60m", "not_reported", "1" },
        { "cm-a", "5to60m", "reported_zero", "0" }, { "cm-a", "gt60m", "hits", "1" },
        /* Shares: over reporting requests only; none reported is null, not 0. */
        { "cm-a", "lt1m", "share", "0.9" },         { "cm-a", "first", "share", "0.25" },
        { "cm-a", "1to5m", "share", "0.0" },        { "cm-a", "5to60m", "share", NULL },
        { "cm-old", "first", "hits", "1" },
    };
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++)
        cache_expect(out, cells[i].model, cells[i].key, cells[i].field, cells[i].want);
    free(out);
    /* A window keeps recent requests, with the gaps they had in full history. */
    assert(explore_cache(g_db, 7, &out) == 200 && out);
    assert(qi("SELECT ?1 ->> '$.days' = 7 AND NOT EXISTS (SELECT 1 FROM json_each(?1, '$.models') m"
              " WHERE m.value ->> '$.model' = 'cm-old')", out) == 1);
    cache_expect(out, "cm-a", "gt60m", "hits", "1");
    free(out);
}

/* ── identifiers and values from the request never become SQL ──────────── */

static void test_injection(void) {
    char *out = NULL;
    static const char *const BAD_TABLES[] = {
        "agents\" WHERE 1; DROP TABLE agents; --", "agents; DROP TABLE agents",
        "sqlite_master", "sqlite_schema", "entries_fts", "entries_fts_data", "main.agents",
        "AGENTS", "", "pragma_table_info('agents')", "(SELECT 1)",
    };
    for (size_t i = 0; i < sizeof(BAD_TABLES) / sizeof(BAD_TABLES[0]); i++) {
        assert(rows_call(BAD_TABLES[i], NULL, NULL, 0, &out) == 404);
        /* The name is echoed escaped inside a JSON string, nothing more. */
        assert(qi("SELECT json_valid(?1) AND json_type(?1, '$.error') = 'text'", out) == 1);
        free(out);
        assert(row_call(BAD_TABLES[i], "[1]", &out) == 404); free(out);
    }
    /* A column name in a filter must be one of the table's columns. */
    assert(rows_call("agents", "{\"name\\\" = '' OR 1=1 --\":\"x\"}", NULL, 0, &out) == 400);
    assert(qi("SELECT ?1 ->> '$.error' = 'no such column'", out) == 1);
    free(out);
    assert(rows_call("agents", "{\"rowid\":1}", NULL, 0, &out) == 400); free(out);
    /* A filter value is bound: it matches nothing, it is not executed. */
    assert(rows_call("agents", "{\"name\":\"' OR 1=1 --\"}", NULL, 0, &out) == 200);
    assert(qi("SELECT json_array_length(?1, '$.rows')", out) == 0);
    free(out);
    assert(rows_call("agents", "{\"name\":[\"alpha\",\"') OR 1=1 --\"]}", NULL, 0, &out) == 200);
    assert(qi("SELECT json_array_length(?1, '$.rows')", out) == 1);
    free(out);
    /* So are keys and cursors. */
    assert(row_call("agents", "[\"alpha' OR '1'='1\"]", &out) == 404); free(out);
    assert(rows_call("agents", NULL, "[\"') OR 1=1 --\"]", 0, &out) == 200); free(out);
    assert(qi("SELECT count(*) FROM agents", NULL) == 5);
}

/* ── secrets never leave the server ────────────────────────────────────── */

static int g_walk_calls;

static void no_marker(const char *what, const char *body) {
    g_walk_calls++;
    if (body && strstr(body, MARK)) {
        fprintf(stderr, "secret marker leaked by %s:\n%.400s\n", what, strstr(body, MARK));
        assert(0);
    }
}

/* Every row of every table, through rows (all pages) and row. */
static void walk_all(void) {
    char *meta = NULL;
    assert(explore_meta(g_db, &meta) == 200);
    no_marker("meta", meta);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db, "SELECT value->>'name' FROM json_each(?1, '$.tables')",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, meta, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        char *after = NULL;
        for (;;) {
            char *out = NULL;
            assert(rows_call(t, NULL, after, 200, &out) == 200);
            no_marker(t, out);
            sqlite3_stmt *rs;
            assert(sqlite3_prepare_v2(g_db, "SELECT value->>'key' FROM json_each(?1, '$.rows')",
                                      -1, &rs, NULL) == SQLITE_OK);
            sqlite3_bind_text(rs, 1, out, -1, SQLITE_STATIC);
            while (sqlite3_step(rs) == SQLITE_ROW) {
                char *row = NULL;
                assert(row_call(t, (const char *)sqlite3_column_text(rs, 0), &row) == 200);
                no_marker(t, row);
                free(row);
            }
            sqlite3_finalize(rs);
            free(after);
            after = q1("SELECT ?1 -> '$.next'", out);
            free(out);
            if (!after || strcmp(after, "null") == 0) break;
        }
        free(after);
    }
    sqlite3_finalize(st);
    free(meta);
}

static void test_secret_walk(void) {
    /* The marker in every place decided secret: the secret store's value
     * (its ciphertext is still the secret), a secret config key, the admin
     * token, a share token. Also a secret named in places that are not
     * secret — names may show, values may not. */
    exec("INSERT INTO secrets(name, value, scope) VALUES"
         " ('OR_KEY', 'enc:" MARK "aa', 'system'), ('GH_TOKEN', 'enc:" MARK "bb', 'agent');"
         "UPDATE secrets SET value = 'enc:" MARK "cc' WHERE name = 'GH_TOKEN';"
         "INSERT INTO secret_hosts(secret_name, host) VALUES ('GH_TOKEN', 'api.github.com');"
         "INSERT OR REPLACE INTO config(key, value, secret) VALUES"
         " ('discord.bot_token', '" MARK "dd', 1);"
         "INSERT OR REPLACE INTO config(key, value, default_value) VALUES"
         " ('web_admin_token', '" MARK "ee', '" MARK "ef');"
         "INSERT INTO shares(token, agent_name, path, name, expires_at) VALUES"
         " ('" MARK "ff00ff00ff00ff00ff', 'alpha', '/tmp/x', 'x', unixepoch() + 60);");
    /* The change log saw those writes and must not have kept a value. */
    assert(qi("SELECT count(*) FROM change_log WHERE table_name = 'secrets'", NULL) >= 3);
    assert(qi("SELECT (SELECT count(*) FROM secrets WHERE value LIKE '%" MARK "%')"
              " + (SELECT count(*) FROM config WHERE value LIKE '%" MARK "%')"
              " + (SELECT count(*) FROM config WHERE default_value LIKE '%" MARK "%')"
              " + (SELECT count(*) FROM shares WHERE token LIKE '%" MARK "%')", NULL) == 6);
    g_walk_calls = 0;
    walk_all();
    assert(g_walk_calls > 200);
    /* The hidden values are marked, not dropped silently. */
    char *out = NULL;
    assert(row_call("secrets", "[\"GH_TOKEN\"]", &out) == 200);
    assert(qi("SELECT ?1 ->> '$.row.values.value.hidden'", out) == 1);
    assert(qi("SELECT ?1 ->> '$.row.values.name' = 'GH_TOKEN'", out) == 1);
    free(out);
    exec("INSERT OR REPLACE INTO config(key, value, default_value) VALUES ('web_port', '9', '8080')");
    assert(row_call("config", "[\"web_port\"]", &out) == 200);
    assert(qi("SELECT json_type(?1, '$.row.values.default_value') = 'text'", out) == 1);
    free(out);
    assert(row_call("config", "[\"web_admin_token\"]", &out) == 200);
    assert(qi("SELECT ?1 ->> '$.row.values.value.hidden'", out) == 1);
    free(out);
    /* A hidden column cannot be probed by filtering on it. */
    assert(rows_call("secrets", "{\"value\":\"x\"}", NULL, 0, &out) == 400); free(out);
    assert(rows_call("config", "{\"value\":\"x\"}", NULL, 0, &out) == 400); free(out);
    assert(rows_call("shares", "{\"token\":\"x\"}", NULL, 0, &out) == 400); free(out);
    /* A share is addressed by rowid, so its token is not its key. */
    assert(row_call("shares", "[1]", &out) == 200);
    assert(qi("SELECT ?1 ->> '$.row.values.token.hidden'", out) == 1);
    free(out);
    assert(explore_hidden("secrets", "value") == 1 && explore_hidden("config", "value") == 2 &&
           explore_hidden("secrets", "name") == 0);
}

/* ── the page never builds markup from data ────────────────────────────── */

static void test_page_source(void) {
    /* Both of the operator's pages: the explorer and the admin panel. */
    const char *const PAGES[] = { TPL_EXPLORER_HTML, TPL_DASHBOARD_HTML };
    static const char *const SINKS[] = { "innerHTML", "outerHTML", "insertAdjacentHTML",
                                         "document.write", "eval(", "new Function",
                                         "http://", "https://", "@import", "url(" };
    for (size_t pg = 0; pg < sizeof(PAGES) / sizeof(PAGES[0]); pg++) {
        const char *p = PAGES[pg];
        assert(strstr(p, "<title>"));
        for (size_t i = 0; i < sizeof(SINKS) / sizeof(SINKS[0]); i++) {
            if (strstr(p, SINKS[i])) fprintf(stderr, "page %zu contains %s\n", pg, SINKS[i]);
            assert(!strstr(p, SINKS[i]));
        }
    }
}

/* ── HTTP ──────────────────────────────────────────────────────────────── */

typedef struct { char *d; size_t n; } Body;

static size_t body_cb(void *p, size_t s, size_t m, void *ud) {
    Body *b = ud;
    size_t t = s * m;
    char *nd = realloc(b->d, b->n + t + 1);
    if (!nd) return 0;
    b->d = nd;
    memcpy(b->d + b->n, p, t);
    b->n += t;
    b->d[b->n] = '\0';
    return t;
}

static long http(CURL *c, const char *path, const char *post, Body *body, Body *hdr) {
    char url[512];
    snprintf(url, sizeof(url), "http://127.0.0.1:19891%s", path);
    free(body->d); body->d = NULL; body->n = 0;
    free(hdr->d); hdr->d = NULL; hdr->n = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
    else curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, body_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, hdr);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    assert(curl_easy_perform(c) == CURLE_OK);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

static void test_http(void) {
    Config cfg = {0};
    cfg.web_port = 19891;
    assert(web_start(&cfg, DB) == 0);
    usleep(50000);
    char *tok = config_get(g_db, "web_admin_token");
    assert(tok && tok[0]);
    CURL *c = curl_easy_init();
    assert(c);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    Body b = {0}, h = {0};
    static const char *const API[] = {
        "/admin/api/meta", "/admin/api/rows?table=agents",
        "/admin/api/row?table=agents&key=%5B%22alpha%22%5D", "/admin/api/nope",
        "/admin/api/cache", "/admin/api/session?id=20",
    };
    /* Not signed in: every path is refused, with a way in. */
    for (size_t i = 0; i < sizeof(API) / sizeof(API[0]); i++) {
        assert(http(c, API[i], NULL, &b, &h) == 403);
        assert(strstr(b.d, "not signed in") && strstr(h.d, "application/json"));
    }
    char path[256];
    snprintf(path, sizeof(path), "/admin/api/meta?token=%s", tok);
    assert(http(c, path, NULL, &b, &h) == 403);   /* API: cookie only */
    assert(http(c, "/admin/data", NULL, &b, &h) == 403);
    assert(strstr(b.d, "cclaw dashboard") && strstr(h.d, "text/html"));
    assert(http(c, "/admin/data?token=00000000000000000000000000000000", NULL, &b, &h) == 403);
    /* Signing in moves the token into the cookie and out of the URL. */
    snprintf(path, sizeof(path), "/admin/data?token=%s", tok);
    assert(http(c, path, NULL, &b, &h) == 303);
    assert(strstr(h.d, "Location: /admin/data\r\n") && strstr(h.d, "Set-Cookie: cclaw_admin="));
    assert(http(c, "/admin/data", NULL, &b, &h) == 200);
    assert(strstr(h.d, "Content-Security-Policy: default-src 'none'"));
    assert(strstr(h.d, "Cache-Control: no-store"));
    assert(strcmp(b.d, TPL_EXPLORER_HTML) == 0);
    assert(http(c, "/admin/api/meta", NULL, &b, &h) == 200);
    assert(!strstr(b.d, tok));
    assert(strstr(h.d, "X-Content-Type-Options: nosniff"));
    assert(http(c, "/admin/api/rows?table=entries&limit=3&filter=%7B%22session_id%22%3A1%7D",
                NULL, &b, &h) == 200);
    assert(qi("SELECT json_array_length(?1, '$.rows')", b.d) == 3);
    char *next = q1("SELECT ?1 -> '$.next'", b.d);
    long long last = qi("SELECT ?1 ->> '$.rows[2].key[0]'", b.d);
    char *enc = curl_easy_escape(c, next, 0);
    snprintf(path, sizeof(path), "/admin/api/rows?table=entries&limit=3&after=%s", enc);
    curl_free(enc);
    assert(http(c, path, NULL, &b, &h) == 200);
    assert(qi("SELECT ?1 ->> '$.rows[0].key[0]'", b.d) == last - 1);
    free(next);
    /* The query string reaches the in-process calls tested above. */
    assert(http(c, "/admin/api/row?table=agents&key=%5B%22alpha%22%5D", NULL, &b, &h) == 200);
    assert(http(c, "/admin/api/cache?days=7", NULL, &b, &h) == 200);
    assert(qi("SELECT ?1 ->> '$.days'", b.d) == 7);
    assert(http(c, "/admin/api/session?id=20", NULL, &b, &h) == 200);
    assert(http(c, "/admin/api/session", NULL, &b, &h) == 400);
    assert(http(c, "/admin/api/session?id=999", NULL, &b, &h) == 404);
    assert(http(c, "/admin/api/row?table=nosuch&key=%5B1%5D", NULL, &b, &h) == 404);
    assert(http(c, "/admin/api/rows", NULL, &b, &h) == 400);
    assert(http(c, "/admin/api/nope", NULL, &b, &h) == 404);
    /* Reading is GET only; actions are POST /admin/api/act, and a GET there
     * changes nothing. */
    assert(http(c, "/admin/api/meta", "x=1", &b, &h) == 405);
    assert(http(c, "/admin/data", "x=1", &b, &h) == 405);
    assert(http(c, "/admin/api/act", NULL, &b, &h) == 405);
    /* Search: a substring of a text key column, here a cached model id. */
    assert(http(c, "/admin/api/rows?table=models_cache&q=A%2F2", NULL, &b, &h) == 200);
    assert(qi("SELECT json_array_length(?1, '$.rows')", b.d) == 1);
    assert(qi("SELECT ?1 ->> '$.rows[0].key[1]' = 'a/2'", b.d) == 1);
    assert(http(c, "/admin/api/rows?table=entries&q=x", NULL, &b, &h) == 400);
    free(b.d); free(h.d); free(tok);
    curl_easy_cleanup(c);
    web_stop();
}

int main(void) {
    TEST_INIT();
    test_db_clean(DB);
    g_db = test_db_open(DB);
    assert(g_db);
    seed();
    RUN(test_meta_complete);
    RUN(test_rows_paging);
    RUN(test_row_keys);
    RUN(test_injection);
    RUN(test_session_edits);
    RUN(test_cache);
    RUN(test_page_source);
    RUN(test_secret_walk);
    RUN(test_http);
    db_close(g_db);
    test_db_clean(DB);
    printf("All explore tests passed.\n");
    return 0;
}
