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
static int tests_run = 0, tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while (0)

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
        "INSERT INTO models(id, provider_name, model, capabilities, priority)"
        " VALUES('mockgem/lite', 'mockgem', 'gem-lite', '[\"audio\"]', 0);", s_port);
    exec_sql(db, sql);
}

static int64_t seed_media_job(sqlite3 *db, int64_t sid, const char *caption,
                              const char *spool_file) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO media_jobs(session_id, source, payload) VALUES(?1, 'telegram',"
        " json_object('channel_id','42','from','Mark','text',?2,"
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

static int media_jobs_count(sqlite3 *db) {
    sqlite3_stmt *s;
    int n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM media_jobs", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return n;
}

static void test_transcribe_success(void) {
    TEST(transcribe_success);
    const char *db_path = "/tmp/cclaw_transcribe_test.db";
    unlink(db_path);

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
    if (rc != 0) { db_close(db); unlink(db_path); FAIL("expected rc==0"); }

    /* The request that went out carried the audio inline */
    const char *req = mock_server_last_request_body();
    if (!req || !strstr(req, "\"inlineData\"") || !strstr(req, "T2dnUw==")) {
        db_close(db); unlink(db_path); FAIL("request missing inlineData audio");
    }

    char *payload = inbox_payload(db, sid);
    if (!payload) { db_close(db); unlink(db_path); FAIL("no inbox row"); }
    int ok = strstr(payload, "[voice message] hello from voice") != NULL &&
             strstr(payload, "\"channel_id\":\"42\"") != NULL &&
             strstr(payload, "T2dnUw==") == NULL;   /* audio never enters context */
    free(payload);
    int jobs_left = media_jobs_count(db);

    /* Spool file must be deleted after successful transcription */
    int spool_gone = (access(spool, F_OK) != 0);

    db_close(db); unlink(db_path);
    if (!ok) FAIL("bad inbox payload");
    if (jobs_left != 0) FAIL("media_jobs row not deleted");
    if (!spool_gone) FAIL("spool file not deleted after success");
    PASS();
}

static void test_transcribe_caption_prepended(void) {
    TEST(transcribe_caption_prepended);
    const char *db_path = "/tmp/cclaw_transcribe_cap_test.db";
    unlink(db_path);

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
    if (rc != 0) { db_close(db); unlink(db_path); FAIL("expected rc==0"); }

    char *payload = inbox_payload(db, sid);
    if (!payload) { db_close(db); unlink(db_path); FAIL("no inbox row"); }
    int ok = strstr(payload, "the caption\\n[voice message] hello from voice") != NULL;
    if (!ok) printf("(payload: %s) ", payload);
    free(payload);

    /* Spool file must be deleted */
    int spool_gone = (access(spool, F_OK) != 0);

    db_close(db); unlink(db_path);
    if (!ok) FAIL("caption not prepended");
    if (!spool_gone) FAIL("spool file not deleted after caption test");
    PASS();
}

static void test_transcribe_terminal_failure(void) {
    TEST(transcribe_terminal_failure);
    const char *db_path = "/tmp/cclaw_transcribe_fail_test.db";
    unlink(db_path);

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
    if (rc != 0) { db_close(db); unlink(db_path); FAIL("expected rc==0 (resolved as failure)"); }

    char *payload = inbox_payload(db, sid);
    if (!payload) { db_close(db); unlink(db_path); FAIL("no inbox row"); }
    int ok = strstr(payload, "[voice message received but transcription failed]") != NULL;
    free(payload);
    int jobs_left = media_jobs_count(db);

    /* Spool file must be deleted even on terminal failure */
    int spool_gone = (access(spool, F_OK) != 0);

    db_close(db); unlink(db_path);
    if (!ok) FAIL("missing failure text");
    if (jobs_left != 0) FAIL("media_jobs row not deleted");
    if (!spool_gone) FAIL("spool file not deleted after terminal failure");
    PASS();
}

static void test_transcribe_no_audio_model(void) {
    TEST(transcribe_no_audio_model);
    const char *db_path = "/tmp/cclaw_transcribe_nomodel_test.db";
    unlink(db_path);

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

    db_close(db); unlink(db_path);
    if (!ok) FAIL("expected terminal failure without network traffic");
    if (!spool_gone) FAIL("spool file not deleted in no-model path");
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    s_port = mock_server_start();
    if (s_port <= 0) {
        fprintf(stderr, "FAIL: could not start mock server\n");
        return 1;
    }

    printf("test_integration_transcribe:\n");
    test_transcribe_success();
    test_transcribe_caption_prepended();
    test_transcribe_terminal_failure();
    test_transcribe_no_audio_model();

    mock_server_stop();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
