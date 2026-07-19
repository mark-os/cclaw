/* Test: capability-routed transcription plumbing —
 * model_pick_by_capability (healthy/degraded/disabled/missing-cap filtering)
 * and transcribe_build_body (gemini + openai request shapes). */
#define _POSIX_C_SOURCE 200809L
#include "llm_proc.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB "/tmp/cclaw_test_media_transcribe.db"

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while (0)

/* Spool file path template — pid-tagged to avoid collisions */
static char spool_path[256];

static void exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "sql failed: %s\n", err ? err : "?"); exit(1); }
}

/* Write known bytes to a temp spool file. "OggS" → base64 "T2dnUw==" */
static void write_spool(const char *path, const unsigned char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(data, 1, len, f);
    fclose(f);
}

static void seed_models(sqlite3 *db) {
    exec_sql(db,
        "DELETE FROM models; DELETE FROM providers;"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env) VALUES"
        " ('oai', 'https://oai.example/v1', 'openai', 'OAI_KEY'),"
        " ('gem', 'https://gem.example/v1beta', 'gemini', 'GEM_KEY');"
        "INSERT INTO models(id, provider_name, model, capabilities, priority, status, degraded_until) VALUES"
        " ('oai/chat', 'oai', 'chat-model', '[\"text\"]', 0, 'healthy', NULL),"
        " ('gem/audio', 'gem', 'gemini-lite', '[\"text\",\"image\",\"audio\"]', 10, 'healthy', NULL),"
        " ('oai/audio2', 'oai', 'audio-two', '[\"audio\"]', 20, 'healthy', NULL),"
        " ('oai/audio-degraded', 'oai', 'audio-deg', '[\"audio\"]', 1, 'degraded', unixepoch()+600),"
        " ('oai/audio-disabled', 'oai', 'audio-dis', '[\"audio\"]', 2, 'disabled', NULL),"
        " ('oai/caps-null', 'oai', 'caps-null', NULL, 3, 'healthy', NULL);");
}

static void test_picker_filters(sqlite3 *db) {
    TEST(picker_filters);
    ModelCandidate out[8];
    int n = model_pick_by_capability(db, "audio", out, 8);
    if (n != 2) { printf("FAIL: expected 2 candidates, got %d\n", n); return; }
    /* priority order: gem/audio (10) before oai/audio2 (20); the degraded,
     * disabled, missing-cap and NULL-cap models are all excluded */
    if (strcmp(out[0].id, "gem/audio") != 0) FAIL("wrong first candidate");
    if (out[0].endpoint_type != ENDPOINT_GEMINI) FAIL("gemini endpoint expected");
    if (strcmp(out[0].api_key_env, "GEM_KEY") != 0) FAIL("wrong key env");
    if (strcmp(out[1].id, "oai/audio2") != 0) FAIL("wrong second candidate");
    if (out[1].endpoint_type != ENDPOINT_OPENAI) FAIL("openai endpoint expected");
    PASS();
}

static void test_picker_no_match(sqlite3 *db) {
    TEST(picker_no_match);
    ModelCandidate out[8];
    int n = model_pick_by_capability(db, "video", out, 8);
    if (n != 0) FAIL("expected no candidates");
    PASS();
}

static void test_picker_degraded_expired(sqlite3 *db) {
    TEST(picker_degraded_expired);
    /* An expired degradation window makes the model eligible again. */
    exec_sql(db, "UPDATE models SET degraded_until=unixepoch()-10 WHERE id='oai/audio-degraded';");
    ModelCandidate out[8];
    int n = model_pick_by_capability(db, "audio", out, 8);
    if (n != 3) { printf("FAIL: expected 3 candidates, got %d\n", n); return; }
    if (strcmp(out[0].id, "oai/audio-degraded") != 0) FAIL("expired-degraded should lead (priority 1)");
    exec_sql(db, "UPDATE models SET degraded_until=unixepoch()+600 WHERE id='oai/audio-degraded';");
    PASS();
}

/* Insert a media_job with a spool file path (new contract: no data_b64) */
static int64_t insert_media_job_file(sqlite3 *db, const char *kind,
                                     const char *mime, const char *path) {
    sqlite3_stmt *s;
    const char *sql =
        "INSERT INTO media_jobs(session_id, source, payload) VALUES(7, 'telegram',"
        " json_object('chat_id','42','from','Mark','text','the caption',"
        "   'media', json_object('kind',?1,'mime',?2,'path',?3)));";
    assert(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, mime, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, path, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    return sqlite3_last_insert_rowid(db);
}

static void test_body_gemini_audio(sqlite3 *db) {
    TEST(body_gemini_audio);
    /* Write "OggS" (4 bytes) → base64 "T2dnUw==" */
    const unsigned char ogg_hdr[] = {0x4F, 0x67, 0x67, 0x53};
    write_spool(spool_path, ogg_hdr, sizeof(ogg_hdr));

    int64_t jid = insert_media_job_file(db, "audio", "audio/ogg", spool_path);
    char *body = transcribe_build_body(db, ENDPOINT_GEMINI, "gemini-lite", jid);
    if (!body) FAIL("no body");
    int ok = strstr(body, "\"contents\"") &&
             strstr(body, "\"inlineData\"") &&
             strstr(body, "\"mimeType\":\"audio/ogg\"") &&
             strstr(body, "\"data\":\"T2dnUw==\"") &&
             strstr(body, "Transcribe") &&
             !strstr(body, "gemini-lite");   /* gemini puts the model in the URL */
    if (!ok) { printf("FAIL: bad body: %s\n", body); free(body); return; }
    free(body);
    PASS();
}

static void test_body_openai_audio(sqlite3 *db) {
    TEST(body_openai_audio);
    const unsigned char ogg_hdr[] = {0x4F, 0x67, 0x67, 0x53};
    write_spool(spool_path, ogg_hdr, sizeof(ogg_hdr));

    int64_t jid = insert_media_job_file(db, "audio", "audio/ogg", spool_path);
    char *body = transcribe_build_body(db, ENDPOINT_OPENAI, "audio-two", jid);
    if (!body) FAIL("no body");
    int ok = strstr(body, "\"model\":\"audio-two\"") &&
             strstr(body, "\"input_audio\"") &&
             strstr(body, "\"format\":\"ogg\"") &&
             strstr(body, "\"data\":\"T2dnUw==\"") &&
             strstr(body, "Transcribe");
    if (!ok) { printf("FAIL: bad body: %s\n", body); free(body); return; }
    free(body);
    PASS();
}

static void test_body_gemini_image(sqlite3 *db) {
    TEST(body_gemini_image);
    /* A fake 4-byte PNG header */
    const unsigned char png_hdr[] = {0x89, 0x50, 0x4E, 0x47};
    write_spool(spool_path, png_hdr, sizeof(png_hdr));

    int64_t jid = insert_media_job_file(db, "image", "image/png", spool_path);
    char *body = transcribe_build_body(db, ENDPOINT_GEMINI, "gemini-lite", jid);
    if (!body) FAIL("no body");
    int ok = strstr(body, "\"inlineData\"") &&
             strstr(body, "\"mimeType\":\"image/png\"") &&
             strstr(body, "Describe");
    if (!ok) { printf("FAIL: bad body: %s\n", body); free(body); return; }
    free(body);
    PASS();
}

static void test_body_openai_image(sqlite3 *db) {
    TEST(body_openai_image);
    const unsigned char png_hdr[] = {0x89, 0x50, 0x4E, 0x47};
    write_spool(spool_path, png_hdr, sizeof(png_hdr));

    int64_t jid = insert_media_job_file(db, "image", "image/png", spool_path);
    char *body = transcribe_build_body(db, ENDPOINT_OPENAI, "gpt-vision", jid);
    if (!body) FAIL("no body");
    int ok = strstr(body, "\"image_url\"") &&
             strstr(body, "data:image/png;base64,") &&
             strstr(body, "Describe");
    if (!ok) { printf("FAIL: bad body: %s\n", body); free(body); return; }
    free(body);
    PASS();
}

static void test_body_openai_mp3(sqlite3 *db) {
    TEST(body_openai_mp3);
    /* audio/mpeg → format "mp3" */
    const unsigned char mp3_hdr[] = {0xFF, 0xFB, 0x90, 0x00};
    write_spool(spool_path, mp3_hdr, sizeof(mp3_hdr));

    int64_t jid = insert_media_job_file(db, "audio", "audio/mpeg", spool_path);
    char *body = transcribe_build_body(db, ENDPOINT_OPENAI, "audio-two", jid);
    if (!body) FAIL("no body");
    int ok = strstr(body, "\"format\":\"mp3\"") &&
             strstr(body, "\"input_audio\"");
    if (!ok) { printf("FAIL: bad body: %s\n", body); free(body); return; }
    free(body);
    PASS();
}

static void test_body_missing_job(sqlite3 *db) {
    TEST(body_missing_job);
    char *body = transcribe_build_body(db, ENDPOINT_GEMINI, "m", 999999);
    if (body) { free(body); FAIL("expected NULL for missing job"); }
    PASS();
}

static void test_body_unreadable_path(sqlite3 *db) {
    TEST(body_unreadable_path);
    /* Seed a job pointing to a file that doesn't exist */
    int64_t jid = insert_media_job_file(db, "audio", "audio/ogg",
                                        "/tmp/cclaw_test_NO_SUCH_FILE.ogg");
    char *body = transcribe_build_body(db, ENDPOINT_GEMINI, "gemini-lite", jid);
    if (body) { free(body); FAIL("expected NULL for unreadable path"); }
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("test_media_transcribe\n");

    snprintf(spool_path, sizeof(spool_path),
             "/tmp/cclaw_test_media_%d.ogg", (int)getpid());

    test_db_clean(TEST_DB);
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    seed_models(db);

    test_picker_filters(db);
    test_picker_no_match(db);
    test_picker_degraded_expired(db);
    test_body_gemini_audio(db);
    test_body_openai_audio(db);
    test_body_gemini_image(db);
    test_body_openai_image(db);
    test_body_openai_mp3(db);
    test_body_missing_job(db);
    test_body_unreadable_path(db);

    db_close(db);
    test_db_clean(TEST_DB);
    unlink(spool_path);
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
