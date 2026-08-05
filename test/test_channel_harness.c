/* channel_harness_run(): a scenario with one fixture + one inject_outbox +
 * expect_event/expect_http against a trivial echo channel, run against a
 * throwaway "real" DB (harness itself creates its own scratch DB from this
 * one's channel/extension rows — this file never touches that scratch DB,
 * only sets up what resolve_js_path needs to find). Also covers the
 * fail-closed unmatched-request path. */
#define _POSIX_C_SOURCE 200809L
#include "channel_harness.h"
#include "approval.h"
#include "resolve.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* channel.o (linked in for db_ensure_schema's caller chain via libcclaw)
 * references resolve_approval — stub keeps resolve.o out of the link, same
 * pattern as test_channel_events.c. */
void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via,
                      int64_t grant_expires_at) {
    (void)approval_id; (void)decision; (void)decided_via; (void)grant_expires_at;
}

#define TEST_DB "/tmp/test_channel_harness.sqlite"
#define EXT_DIR "/tmp/test_channel_harness_ext"

static const char *CHANNEL_JS =
    "function onInit() { return {}; }\n"
    "function onOutbox(item) {\n"
    "  channel.emit('message', 'saw:' + item.payload);\n"
    "  channel.http({method:'POST', url:'https://api.example.com/sendMessage',"
    "                body: item.payload}).then(function(r){"
    "    channel.emit('result', 'status:' + r.status);"
    "  });\n"
    "  return {};\n"
    "}\n";

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void rm_rf(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

static void setup_db_and_channel(void) {
    test_db_clean(TEST_DB);
    rm_rf(EXT_DIR);
    mkdir(EXT_DIR, 0755);
    write_file(EXT_DIR "/channel.qjs", CHANNEL_JS);

    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "INSERT INTO extensions(name, path, version, owner_agent, published, enabled)"
        " VALUES('echo', ?, '0.1.0', 'default', 0, 1);", -1, &s, NULL);
    sqlite3_bind_text(s, 1, EXT_DIR, -1, SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);
    sqlite3_prepare_v2(db,
        "INSERT INTO channels(name, extension_name, type, binary_path, status)"
        " VALUES('echo', 'echo', 'echo', ?, 'draft');", -1, &s, NULL);
    sqlite3_bind_text(s, 1, EXT_DIR "/channel.qjs", -1, SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);
    db_close(db);
}

static void write_scenario(const char *path, const char *json) {
    write_file(path, json);
}

static void test_harness_pass(void) {
    setup_db_and_channel();
    const char *scenario_path = "/tmp/test_channel_harness_scenario_pass.json";
    write_scenario(scenario_path,
        "{ \"fixtures\": [ {\"match\": {\"method\":\"POST\",\"url_substr\":\"sendMessage\"},"
        "                   \"respond\": {\"status\":200,\"body\":\"ok\"}} ],"
        "  \"steps\": [ {\"inject_outbox\": \"hello world\"},"
        "               {\"expect_event\": {\"contains\": \"saw:hello world\"}},"
        "               {\"expect_http\": {\"url_substr\": \"sendMessage\", \"body_contains\": \"hello world\"}},"
        "               {\"expect_event\": {\"contains\": \"status:200\"}} ] }");

    int rc = channel_harness_run(TEST_DB, "echo", scenario_path);
    assert(rc == 0);
    unlink(scenario_path);
    printf("  PASS test_harness_pass\n");
}

static void test_harness_unmatched_request_fails(void) {
    setup_db_and_channel();
    const char *scenario_path = "/tmp/test_channel_harness_scenario_fail.json";
    write_scenario(scenario_path,
        "{ \"fixtures\": [],"
        "  \"steps\": [ {\"inject_outbox\": \"hello\"},"
        "               {\"expect_http\": {\"url_substr\": \"sendMessage\"}} ] }");

    /* No fixture for the POST the JS makes — fail-closed: exit nonzero. */
    int rc = channel_harness_run(TEST_DB, "echo", scenario_path);
    assert(rc != 0);
    unlink(scenario_path);
    printf("  PASS test_harness_unmatched_request_fails\n");
}

static void test_harness_unknown_channel(void) {
    setup_db_and_channel();
    const char *scenario_path = "/tmp/test_channel_harness_scenario_unknown.json";
    write_scenario(scenario_path, "{ \"fixtures\": [], \"steps\": [] }");

    int rc = channel_harness_run(TEST_DB, "does-not-exist", scenario_path);
    assert(rc != 0);
    unlink(scenario_path);
    printf("  PASS test_harness_unknown_channel\n");
}

/* save_to scenario: channel.http with save_to resolves {status, path, bytes}. */
static const char *CHANNEL_JS_SAVETO =
    "function onInit() { return {}; }\n"
    "function onOutbox(item) {\n"
    "  channel.http({method:'GET', url:'https://api.example.com/file',"
    "                save_to:'f.bin'}).then(function(r){"
    "    channel.emit('result','path:'+(r.path?'yes':'no')+' bytes:'+r.bytes);"
    "  });\n"
    "  return {};\n"
    "}\n";

static void test_harness_save_to(void) {
    /* Reuse setup but overwrite the channel JS with the save_to variant. */
    setup_db_and_channel();
    write_file(EXT_DIR "/channel.qjs", CHANNEL_JS_SAVETO);

    const char *scenario_path = "/tmp/test_channel_harness_scenario_saveto.json";
    write_scenario(scenario_path,
        "{ \"fixtures\": [ {\"match\": {\"method\":\"GET\",\"url_substr\":\"file\"},"
        "                   \"respond\": {\"status\":200,\"body\":\"OGGDATA\"}} ],"
        "  \"steps\": [ {\"inject_outbox\": \"go\"},"
        "               {\"expect_http\": {\"url_substr\": \"file\"}},"
        "               {\"expect_event\": {\"contains\": \"path:yes bytes:7\"}} ] }");

    int rc = channel_harness_run(TEST_DB, "echo", scenario_path);
    assert(rc == 0);
    unlink(scenario_path);
    printf("  PASS test_harness_save_to\n");
}

int main(void) {
    TEST_INIT();
    printf("test_channel_harness:\n");
    test_harness_pass();
    test_harness_save_to();
    test_harness_unmatched_request_fails();
    test_harness_unknown_channel();
    test_db_clean(TEST_DB);
    rm_rf(EXT_DIR);
    printf("all channel harness tests passed\n");
    return 0;
}
