/* Integration test — llm_transcribe() against a mock Gemini server.
 * Verifies the media_jobs → transcription → text-only inbox flow:
 * 1. capability-picks the audio model and POSTs the inlineData request
 * 2. success: inbox row carries "[voice message] <transcript>" (+ caption),
 *    media_jobs row deleted, spool file unlinked, audio bytes never in inbox
 * 3. terminal failure: inbox row tells the agent, job still deleted, spool
 *    file unlinked */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "llm_proc.h"
#include "db.h"
#include "test_util.h"
#include "mock_server.h"

static int s_port;

/* A failed check ends the run: the mock server is in-process threads. */
#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } while (0)

static const char *GEMINI_TRANSCRIPT_RESPONSE =
    "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hello from voice\"}]},"
    "\"finishReason\":\"STOP\"}],\"usageMetadata\":{}}";

static void exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql failed: %s\n", err ? err : "?");
        exit(1);
    }
}

/* Write a small spool file with known bytes ("OggS" header → base64 "T2dnUw==") */
static char *write_spool_file(const char *tag) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/cclaw_integ_spool_%s_%d.ogg",
             tag, (int)getpid());
    FILE *f = fopen(path, "wb");
    assert(f);
    const unsigned char ogg_hdr[] = {0x4F, 0x67, 0x67, 0x53};
    fwrite(ogg_hdr, 1, sizeof(ogg_hdr), f);
    fclose(f);
    return path;
}

/* Point the models table at the mock server: one audio-capable gemini model.
 * llm_build_url appends /models/<model>:generateContent, which the mock
 * server serves via its /models/ handler. */
static void seed_audio_model(sqlite3 *db) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "DELETE FROM models; DELETE FROM providers;"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env)"
        " VALUES('mockgem', 'http://127.0.0.1:%d', 'gemini', 'MOCKGEM_KEY');"
        "INSERT INTO models(id, provider_name, model, capabilities)"
        " VALUES('mockgem/lite', 'mockgem', 'gem-lite', '[\"audio\"]');", s_port);
    exec_sql(db, sql);
}

static int64_t seed_media_job(sqlite3 *db, int64_t sid, const char *caption,
                              const char *spool_file) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO media_jobs(session_id, source, payload) VALUES(?1, 'telegram',"
        " json_object('chat_id','42','from','Mark','text',?2,"
        "   'media', json_object('kind','audio','mime','audio/ogg','path',?3)));",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_text(s, 2, caption ? caption : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, spool_file, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    return sqlite3_last_insert_rowid(db);
}

static char *inbox_payload(sqlite3 *db, int64_t sid) {
    sqlite3_stmt *s;
    char *out = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT payload FROM inbox WHERE session_id=? ORDER BY id DESC LIMIT 1",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, sid);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(s, 0);
            if (v) out = strdup(v);
        }
        sqlite3_finalize(s);
    }
    return out;
}

static void test_transcribe_success(void) {
    const char *db_path = "/tmp/cclaw_transcribe_test.db";
    test_db_clean(db_path);

    mock_server_reset();
    mock_server_enqueue(200, GEMINI_TRANSCRIPT_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    seed_audio_model(db);
    setenv("MOCKGEM_KEY", "test-key", 1);

    char *spool = write_spool_file("success");
    int64_t sid = session_create(db, "telegram", NULL, -1, 0);
    int64_t jid = seed_media_job(db, sid, "", spool);

    int rc = llm_transcribe(db, NULL, jid);
    if (rc != 0) { db_close(db); test_db_clean(db_path); FAIL("expected rc==0"); }

    /* The request that went out carried the audio inline */
    const char *req = mock_server_last_request_body();
    if (!req || !strstr(req, "\"inlineData\"") || !strstr(req, "T2dnUw==")) {
        db_close(db); test_db_clean(db_path); FAIL("request missing inlineData audio");
    }

    char *payload = inbox_payload(db, sid);
    if (!payload) { db_close(db); test_db_clean(db_path); FAIL("no inbox row"); }
    int ok = strstr(payload, "[voice message] hello from voice") != NULL &&
             strstr(payload, "\"chat_id\":\"42\"") != NULL &&
             strstr(payload, "T2dnUw==") == NULL;   /* audio never enters context */
    free(payload);
    int jobs_left = (int)test_qi(db, "SELECT COUNT(*) FROM media_jobs");

    /* Spool file must be deleted after successful transcription */
    int spool_gone = (access(spool, F_OK) != 0);

    db_close(db); test_db_clean(db_path);
    if (!ok) FAIL("bad inbox payload");
    if (jobs_left != 0) FAIL("media_jobs row not deleted");
    if (!spool_gone) FAIL("spool file not deleted after success");
}

static void test_transcribe_caption_prepended(void) {
    const char *db_path = "/tmp/cclaw_transcribe_cap_test.db";
    test_db_clean(db_path);

    mock_server_reset();
    mock_server_enqueue(200, GEMINI_TRANSCRIPT_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    seed_audio_model(db);
    setenv("MOCKGEM_KEY", "test-key", 1);

    char *spool = write_spool_file("caption");
    int64_t sid = session_create(db, "telegram", NULL, -1, 0);
    int64_t jid = seed_media_job(db, sid, "the caption", spool);

    int rc = llm_transcribe(db, NULL, jid);
    if (rc != 0) { db_close(db); test_db_clean(db_path); FAIL("expected rc==0"); }

    char *payload = inbox_payload(db, sid);
    if (!payload) { db_close(db); test_db_clean(db_path); FAIL("no inbox row"); }
    int ok = strstr(payload, "the caption\\n[voice message] hello from voice") != NULL;
    if (!ok) printf("(payload: %s) ", payload);
    free(payload);

    /* Spool file must be deleted */
    int spool_gone = (access(spool, F_OK) != 0);

    db_close(db); test_db_clean(db_path);
    if (!ok) FAIL("caption not prepended");
    if (!spool_gone) FAIL("spool file not deleted after caption test");
}

static void test_transcribe_terminal_failure(void) {
    const char *db_path = "/tmp/cclaw_transcribe_fail_test.db";
    test_db_clean(db_path);

    /* Empty queue → the mock answers 500 to every attempt; the single
     * candidate exhausts its retry and the job goes terminal. */
    mock_server_reset();

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    seed_audio_model(db);
    setenv("MOCKGEM_KEY", "test-key", 1);

    char *spool = write_spool_file("failure");
    int64_t sid = session_create(db, "telegram", NULL, -1, 0);
    int64_t jid = seed_media_job(db, sid, "", spool);

    int rc = llm_transcribe(db, NULL, jid);
    if (rc != 0) { db_close(db); test_db_clean(db_path); FAIL("expected rc==0 (resolved as failure)"); }

    char *payload = inbox_payload(db, sid);
    if (!payload) { db_close(db); test_db_clean(db_path); FAIL("no inbox row"); }
    int ok = strstr(payload, "[voice message received but transcription failed]") != NULL;
    free(payload);
    int jobs_left = (int)test_qi(db, "SELECT COUNT(*) FROM media_jobs");

    /* Spool file must be deleted even on terminal failure */
    int spool_gone = (access(spool, F_OK) != 0);

    db_close(db); test_db_clean(db_path);
    if (!ok) FAIL("missing failure text");
    if (jobs_left != 0) FAIL("media_jobs row not deleted");
    if (!spool_gone) FAIL("spool file not deleted after terminal failure");
}

static void test_transcribe_no_audio_model(void) {
    const char *db_path = "/tmp/cclaw_transcribe_nomodel_test.db";
    test_db_clean(db_path);

    mock_server_reset();

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    /* Only a text model: capability pick finds nothing, immediate terminal. */
    exec_sql(db,
        "DELETE FROM models; DELETE FROM providers;"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env)"
        " VALUES('p', 'http://127.0.0.1:1', 'openai', 'K');"
        "INSERT INTO models(id, provider_name, model, capabilities)"
        " VALUES('p/chat', 'p', 'chat', '[\"text\"]');");

    char *spool = write_spool_file("nomodel");
    int64_t sid = session_create(db, "telegram", NULL, -1, 0);
    int64_t jid = seed_media_job(db, sid, "", spool);

    int rc = llm_transcribe(db, NULL, jid);
    char *payload = inbox_payload(db, sid);
    int ok = rc == 0 && payload &&
             strstr(payload, "transcription failed") != NULL;
    free(payload);

    /* Spool file must be deleted even when no model is available */
    int spool_gone = (access(spool, F_OK) != 0);

    db_close(db); test_db_clean(db_path);
    if (!ok) FAIL("expected terminal failure without network traffic");
    if (!spool_gone) FAIL("spool file not deleted in no-model path");
}

/* agent_models.purpose='audio' (v59): the job's session names an agent, and
 * that agent's audio list is used instead of the global capability pick —
 * proved by which model's request counter moved. */
static void test_transcribe_agent_audio_list(void) {
    const char *db_path = "/tmp/cclaw_transcribe_agent_test.db";
    test_db_clean(db_path);

    mock_server_reset();
    mock_server_enqueue(200, GEMINI_TRANSCRIPT_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    seed_audio_model(db);   /* mockgem/lite: what the global pick would take */
    exec_sql(db,
        "INSERT INTO models(id, provider_name, model, capabilities, created_at)"
        " VALUES('mockgem/mine', 'mockgem', 'gem-mine', '[\"audio\"]', unixepoch()+60);"
        "INSERT INTO agents(name) VALUES('Ears');"
        "INSERT INTO agent_models(agent_name, purpose, model_id, pos)"
        " VALUES('Ears', 'audio', 'mockgem/mine', 0);");
    setenv("MOCKGEM_KEY", "test-key", 1);

    char *spool = write_spool_file("agent");
    int64_t sid = session_create(db, "telegram", "Ears", -1, 0);
    int64_t jid = seed_media_job(db, sid, "", spool);

    int rc = llm_transcribe(db, NULL, jid);
    char *payload = inbox_payload(db, sid);
    int ok = rc == 0 && payload && strstr(payload, "hello from voice") != NULL;
    free(payload);
    int64_t mine = -1, lite = -1;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT id, total_requests FROM models"
                               " WHERE id IN ('mockgem/mine','mockgem/lite')",
                           -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(s, 0);
            if (strcmp(id, "mockgem/mine") == 0) mine = sqlite3_column_int64(s, 1);
            else lite = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);
    }
    db_close(db); test_db_clean(db_path);
    if (!ok) FAIL("expected a transcript in the inbox");
    if (mine != 1 || lite != 0) {
        printf("FAIL: mine=%lld lite=%lld\n", (long long)mine, (long long)lite);
        return;
    }
}

int main(void) {
    TEST_INIT();
    s_port = mock_server_start();
    if (s_port <= 0) {
        fprintf(stderr, "FAIL: could not start mock server\n");
        return 1;
    }

    printf("test_integration_transcribe:\n");
    RUN(test_transcribe_success);
    RUN(test_transcribe_caption_prepended);
    RUN(test_transcribe_terminal_failure);
    RUN(test_transcribe_no_audio_model);
    RUN(test_transcribe_agent_audio_list);

    mock_server_stop();
    return 0;
}
