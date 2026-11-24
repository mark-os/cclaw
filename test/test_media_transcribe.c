/* Test: capability-routed transcription plumbing —
 * model_pick_by_capability (healthy/degraded/disabled/missing-cap filtering),
 * model_pick_for_media (an agent's purpose list first, global pick otherwise)
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

static sqlite3 *g_db;

/* Spool file path template — pid-tagged to avoid collisions */
static char spool_path[256];

static void exec_sql(const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) { fprintf(stderr, "sql failed: %s\n", err ? err : "?"); exit(1); }
}

/* Write known bytes to a temp spool file. "OggS" → base64 "T2dnUw==" */
static void write_spool(const char *path, const unsigned char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(data, 1, len, f);
    fclose(f);
}

static void seed_models(void) {
    exec_sql(
        "DELETE FROM models; DELETE FROM providers;"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env) VALUES"
        " ('oai', 'https://oai.example/v1', 'openai', 'OAI_KEY'),"
        " ('gem', 'https://gem.example/v1beta', 'gemini', 'GEM_KEY');"
        "INSERT INTO models(id, provider_name, model, capabilities, status, degraded_until) VALUES"
        " ('oai/chat', 'oai', 'chat-model', '[\"text\"]', 'healthy', NULL),"
        " ('gem/audio', 'gem', 'gemini-lite', '[\"text\",\"image\",\"audio\"]', 'healthy', NULL),"
        " ('oai/audio2', 'oai', 'audio-two', '[\"audio\"]', 'healthy', NULL),"
        " ('oai/audio-degraded', 'oai', 'audio-deg', '[\"audio\"]', 'degraded', unixepoch()+600),"
        " ('oai/audio-disabled', 'oai', 'audio-dis', '[\"audio\"]', 'disabled', NULL),"
        " ('oai/caps-null', 'oai', 'caps-null', NULL, 'healthy', NULL);");
}

static void test_picker_filters(void) {
    ModelCandidate out[8];
    int n = model_pick_by_capability(g_db, "audio", out, 8);
    assert(n == 2);
    /* catalog order (created_at, id): gem/audio before oai/audio2; the
     * degraded, disabled, missing-cap and NULL-cap models are all excluded */
    assert(strcmp(out[0].id, "gem/audio") == 0);
    assert(out[0].endpoint_type == ENDPOINT_GEMINI);
    assert(strcmp(out[0].api_key_env, "GEM_KEY") == 0);
    assert(strcmp(out[1].id, "oai/audio2") == 0);
    assert(out[1].endpoint_type == ENDPOINT_OPENAI);
}

static void test_picker_no_match(void) {
    ModelCandidate out[8];
    int n = model_pick_by_capability(g_db, "video", out, 8);
    assert(n == 0);
}

static void test_picker_degraded_expired(void) {
    /* An expired degradation window makes the model eligible again. */
    exec_sql("UPDATE models SET degraded_until=unixepoch()-10 WHERE id='oai/audio-degraded';");
    ModelCandidate out[8];
    int n = model_pick_by_capability(g_db, "audio", out, 8);
    assert(n == 3);
    int seen = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(out[i].id, "oai/audio-degraded") == 0) seen = 1;
    assert(seen);
    exec_sql("UPDATE models SET degraded_until=unixepoch()+600 WHERE id='oai/audio-degraded';");
}

/* agent_models.purpose (v59): an agent's audio/image list is the whole policy
 * for that purpose — pos order, models in their cooldown sunk below healthy
 * ones, 'disabled' dropped, and nothing from the global pick appended. */
static void test_media_pick_agent_list(void) {
    exec_sql(
        "INSERT OR IGNORE INTO agents(name) VALUES('Ears');"
        "INSERT INTO agent_models(agent_name, purpose, model_id, pos) VALUES"
        " ('Ears','audio','oai/audio-degraded',0),"
        " ('Ears','audio','oai/audio2',1),"
        " ('Ears','audio','oai/audio-disabled',2);");
    ModelCandidate out[8];
    int n = model_pick_for_media(g_db, "Ears", "audio", out, 8);
    assert(n == 2);
    assert(strcmp(out[0].id, "oai/audio2") == 0);   /* healthy leads */
    assert(strcmp(out[1].id, "oai/audio-degraded") == 0);   /* sinks, not vanishes */
    for (int i = 0; i < n; i++)
        assert(strcmp(out[i].id, "gem/audio") != 0);   /* no global append */
}

/* Rows present but every one in its cooldown: the agent's choice still
 * stands (health reorders, never empties) — the media is not rerouted to a
 * model nobody picked for this agent. */
static void test_media_pick_agent_all_unhealthy(void) {
    exec_sql(
        "INSERT OR IGNORE INTO agents(name) VALUES('Sick');"
        "INSERT INTO agent_models(agent_name, purpose, model_id, pos) VALUES"
        " ('Sick','audio','oai/audio-degraded',0);");
    ModelCandidate out[8];
    int n = model_pick_for_media(g_db, "Sick", "audio", out, 8);
    assert(n == 1 && strcmp(out[0].id, "oai/audio-degraded") == 0);
    /* Only disabled rows: the agent chose, and chose nothing usable. */
    exec_sql(
        "INSERT OR IGNORE INTO agents(name) VALUES('Off');"
        "INSERT INTO agent_models(agent_name, purpose, model_id, pos) VALUES"
        " ('Off','audio','oai/audio-disabled',0);");
    assert(model_pick_for_media(g_db, "Off", "audio", out, 8) == 0);
}

/* No rows for the purpose (chat rows don't count), or no agent at all:
 * exactly today's global capability pick. One purpose never leaks into the
 * other. */
static void test_media_pick_fallback(void) {
    exec_sql(
        "INSERT OR IGNORE INTO agents(name) VALUES('Plain');"
        "INSERT INTO agent_models(agent_name, purpose, model_id, pos) VALUES"
        " ('Plain','chat','oai/chat',0),"
        " ('Plain','image','gem/audio',0);");
    ModelCandidate a[8], b[8];
    int na = model_pick_for_media(g_db, "Plain", "audio", a, 8);
    int nb = model_pick_by_capability(g_db, "audio", b, 8);
    assert(na == nb && na == 2);
    for (int i = 0; i < na; i++)
        assert(strcmp(a[i].id, b[i].id) == 0);
    assert(model_pick_for_media(g_db, NULL, "audio", a, 8) == 2);
    assert(model_pick_for_media(g_db, "Ghost", "audio", a, 8) == 2);
    int ni = model_pick_for_media(g_db, "Plain", "image", a, 8);
    assert(ni == 1 && strcmp(a[0].id, "gem/audio") == 0);
}

/* Insert a media_job with a spool file path (new contract: no data_b64) */
static int64_t insert_media_job_file(sqlite3 *db, const char *kind,
                                     const char *mime, const char *path) {
    sqlite3_stmt *s;
    const char *sql =
        "INSERT INTO media_jobs(session_id, source, payload) VALUES(7, 'telegram',"
        " json_object('chat_id','42','from','Mark','text','the caption',"
        "   'media', json_object('kind',?1,'mime',?2,'path',?3)));";
    assert(sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, mime, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, path, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    return sqlite3_last_insert_rowid(db);
}

/* Provider wire bodies for one spooled media file. path NULL = no job row;
 * want NULL = the builder must refuse. "OggS" encodes as "T2dnUw==". */
static const struct {
    int endpoint; const char *model; const char *kind; const char *mime;
    unsigned char bytes[4]; const char *path; int want_null;
    const char *must[5]; const char *must_not;
} bodies[] = {
    {ENDPOINT_GEMINI, "gemini-lite", "audio", "audio/ogg", {0x4F, 0x67, 0x67, 0x53}, "", 0,
     {"\"contents\"", "\"inlineData\"", "\"mimeType\":\"audio/ogg\"", "\"data\":\"T2dnUw==\"",
      "Transcribe"},
     "gemini-lite"},                    /* gemini puts the model in the URL */
    {ENDPOINT_OPENAI, "audio-two", "audio", "audio/ogg", {0x4F, 0x67, 0x67, 0x53}, "", 0,
     {"\"model\":\"audio-two\"", "\"input_audio\"", "\"format\":\"ogg\"",
      "\"data\":\"T2dnUw==\"", "Transcribe"}, NULL},
    {ENDPOINT_GEMINI, "gemini-lite", "image", "image/png", {0x89, 0x50, 0x4E, 0x47}, "", 0,
     {"\"inlineData\"", "\"mimeType\":\"image/png\"", "Describe"}, NULL},
    {ENDPOINT_OPENAI, "gpt-vision", "image", "image/png", {0x89, 0x50, 0x4E, 0x47}, "", 0,
     {"\"image_url\"", "data:image/png;base64,", "Describe"}, NULL},
    {ENDPOINT_OPENAI, "audio-two", "audio", "audio/mpeg", {0xFF, 0xFB, 0x90, 0x00}, "", 0,
     {"\"format\":\"mp3\"", "\"input_audio\""}, NULL},
    {ENDPOINT_GEMINI, "m", NULL, NULL, {0}, NULL, 1, {NULL}, NULL},
    {ENDPOINT_GEMINI, "gemini-lite", "audio", "audio/ogg", {0},
     "/tmp/cclaw_test_NO_SUCH_FILE.ogg", 1, {NULL}, NULL},
};

static void test_body_table(void) {
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        int64_t jid = 999999;
        if (bodies[i].path) {
            const char *path = bodies[i].path;
            if (!*path) {
                write_spool(spool_path, bodies[i].bytes, sizeof(bodies[i].bytes));
                path = spool_path;
            }
            jid = insert_media_job_file(g_db, bodies[i].kind, bodies[i].mime, path);
        }
        char *body = transcribe_build_body(g_db, bodies[i].endpoint, bodies[i].model, jid);
        int ok = bodies[i].want_null ? body == NULL : body != NULL;
        for (int k = 0; ok && body && k < 5 && bodies[i].must[k]; k++)
            ok = strstr(body, bodies[i].must[k]) != NULL;
        if (ok && body && bodies[i].must_not) ok = !strstr(body, bodies[i].must_not);
        if (!ok) {
            printf("    row %zu: bad body: %s\n", i, body ? body : "(null)");
            assert(0);
        }
        free(body);
    }
}

int main(void) {
    TEST_INIT();
    printf("test_media_transcribe\n");

    snprintf(spool_path, sizeof(spool_path),
             "/tmp/cclaw_test_media_%d.ogg", (int)getpid());

    test_db_clean(TEST_DB);
    g_db = test_db_open(TEST_DB);
    assert(g_db);
    seed_models();

    RUN(test_picker_filters);
    RUN(test_picker_no_match);
    RUN(test_picker_degraded_expired);
    RUN(test_media_pick_agent_list);
    RUN(test_media_pick_agent_all_unhealthy);
    RUN(test_media_pick_fallback);
    RUN(test_body_table);

    db_close(g_db);
    test_db_clean(TEST_DB);
    unlink(spool_path);
    return 0;
}
