/* Unit test for request_config tool (new request_changes dialect) +
 * request_config_changes_apply. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "channel.h"
#include "tools.h"
#include "tool_request_config.h"
#include "agent_config.h"
#include "approval.h"
#include "config_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── fixture ────────────────────────────────────────────────────────── */

/* The registry holds &f->ctx, so a Fx must not move after fx_open. */
typedef struct {
    sqlite3 *db;
    int64_t sid;
    RequestConfigCtx ctx;
    ToolRegistry reg;
} Fx;

static void fx_open(Fx *f, const char *path) {
    f->db = test_db_open_seeded(path);
    assert(f->db);
    config_registry_sync(f->db);
    db_agent_upsert(f->db, "test", NULL, NULL);
    f->sid = session_create(f->db, "t", "test", -1, 0);
    assert(f->sid > 0);
    f->ctx = (RequestConfigCtx){ .db = f->db, .agent_name = "test",
                                 .session_id = f->sid, .agents_dir = NULL,
                                 .current_tool_call_id = "c0" };
    tools_init(&f->reg);
    assert(tool_request_config_register(&f->reg, &f->ctx) == 0);
}

static void fx_close(Fx *f) {
    tools_free(&f->reg);
    db_close(f->db);
}

/* Invoke the handler as tool call `call_id`. NULL = parked. */
static char *call(Fx *f, const char *call_id, const char *args) {
    f->ctx.current_tool_call_id = call_id;
    ToolEntry *e = tools_lookup(&f->reg, "request_config");
    assert(e != NULL);
    return e->handler(args, e->user_data, &(int){0});
}

static void exec_ok(sqlite3 *db, const char *sql) {
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

/* Make a credential name resolve, so the config-time key check passes. The
 * value is never read here — validation tests existence only. */
static void seed_key(sqlite3 *db, const char *name) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO secrets(name, value, scope)"
        " VALUES('%s','x','system')", name);
    exec_ok(db, sql);
}

/* The newest parked document for this session (caller frees). */
static char *last_args(sqlite3 *db, int64_t sid) {
    char *a = db_scalar_text(db, "SELECT args_json FROM approvals WHERE session_id=?1"
                                 " ORDER BY id DESC LIMIT 1", sid);
    assert(a);
    return a;
}

/* Approve-and-apply the newest parked document; returns the apply rc. */
static int apply_latest(sqlite3 *db, int64_t sid, char **receipt_out) {
    char *args = last_args(db, sid);
    int rc = request_config_changes_apply(db, "test", args, 0, 0, 0, receipt_out);
    free(args);
    return rc;
}

/* Assert a one-column text read (sid bound to ?1) equals want. */
static void expect_text(sqlite3 *db, const char *sql, int64_t sid, const char *want) {
    char *got = db_scalar_text(db, sql, sid);
    if (!got || strcmp(got, want) != 0)
        fprintf(stderr, "expect_text: %s\n  want '%s' got '%s'\n",
                sql, want, got ? got : "(null)");
    assert(got && strcmp(got, want) == 0);
    free(got);
}

#define DS "openrouter/deepseek/deepseek-v4.1-flash"

/* ── tests ──────────────────────────────────────────────────────────── */

/* Park a tools grant; verify approvals row and session state. */
static void test_park_tools_grant(void) {
    Fx f;
    fx_open(&f, ":memory:");
    /* awaiting_approval is only reachable from a busy state — in production
     * the dispatcher holds tool_running while the handler parks. */
    assert(session_set_state(f.db, f.sid, "tool_running") == 0);
    assert(call(&f, "call_1",
        "{\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}}}") == NULL);
    expect_text(f.db, "SELECT park_reason || ':' ||"
        " json_extract(args_json,'$.changes.grants.tools[0]')"
        " FROM approvals WHERE session_id=?1 AND tool_name='request_config'",
        f.sid, "approval_required:shell_exec");
    expect_text(f.db, "SELECT state FROM sessions WHERE id=?1", f.sid,
                "awaiting_approval");
    fx_close(&f);
}

/* Every document the handler refuses before parking. One fixture carries
 * the state the refusals need; a refusal never parks, so rows are
 * independent. */
static void test_refusals(void) {
    Fx f;
    fx_open(&f, ":memory:");
    db_agent_upsert(f.db, "Other", NULL, NULL);
    exec_ok(f.db,
        "INSERT OR IGNORE INTO config(key, default_value, description, secret)"
        " VALUES('my_secret_key','','a secret',1);"
        "INSERT INTO extensions(name,path) VALUES('tgext','/x');"
        "INSERT INTO channels(name,extension_name) VALUES('tg','tgext');"
        "INSERT INTO secrets(name,value,scope) VALUES"
        " ('ALPACA_KEY','enc:00','agent'),('SYS_KEY','enc:00','system');"
        "UPDATE models SET context_window=100000 WHERE id='" DS "';"
        "INSERT INTO providers(name, base_url, endpoint_type) VALUES"
        " ('anthx','http://a','anthropic');"
        "INSERT INTO models(id, provider_name, model) VALUES ('anthx/c','anthx','c');");
    assert(channel_pin_new_session(f.db, "tg", "-500", "Other", "o") > 0);

#define AGM(entry) "{\"changes\":{\"agent\":{\"models\":[" entry "]}}}"
#define DSM(keys) AGM("{\"id\":\"" DS "\"," keys "}")
#define SB(val) "{\"changes\":{\"secret_bindings\":{" val "}}}"
    static const struct { const char *doc, *needle, *absent; } cases[] = {
        /* document shape */
        {"{}", "changes", NULL},
        {"{\"changes\":{}}", "nothing to request", NULL},
        {"{\"changes\":{\"tokens\":{\"a\":1}}}", "unknown changes section", NULL},
        /* grants */
        {"{\"changes\":{\"grants\":{\"networks\":[\"x\"]}}}", "unknown grants key", NULL},
        {"{\"changes\":{\"grants\":{\"tools\":\"shell_exec\"}}}", "array", NULL},
        {"{\"changes\":{\"grants\":{\"tools\":[\"\"]}}}", "non-empty", NULL},
        {"{\"changes\":{\"grants\":{\"read_paths\":[\"relative/dir\"]}}}", "absolute", NULL},
        /* config */
        {"{\"changes\":{\"config\":{\"no_such_key\":\"1\"}}}", "search_config", NULL},
        {"{\"changes\":{\"config\":{\"web_port\":123}}}", "string", NULL},
        {"{\"changes\":{\"config\":{\"my_secret_key\":\"x\"}}}", "secret", NULL},
        /* provider */
        {"{\"changes\":{\"provider\":{}}}", "required", NULL},
        {"{\"changes\":{\"provider\":{\"provider\":\"myllm\"}}}", "base_url", NULL},
        {"{\"changes\":{\"provider\":{\"provider\":\"myllm\","
         "\"base_url\":\"ftp://x.com/v1\"}}}", "http", NULL},
        {"{\"changes\":{\"provider\":{\"provider\":\"openrouter\","
         "\"api_key_env\":\"sk-or-v1-secret\"}}}", "api_key_env", NULL},
        /* A model belongs in the models section — the provider doc is
         * transport. (Registering a model by re-submitting a provider doc is
         * the trap that took prod's gateway down.) */
        {"{\"changes\":{\"provider\":{\"provider\":\"openrouter\","
         "\"model\":\"some/model\"}}}", "models", NULL},
        /* models section: provider must exist first, canonical ids only,
         * typo-hostile keys, two statuses */
        {"{\"changes\":{\"models\":[{\"id\":\"m1@ghostprov\"}]}}", "ghostprov", NULL},
        {"{\"changes\":{\"models\":[{\"id\":\"just-a-name\"}]}}", "model@provider", NULL},
        {"{\"changes\":{\"models\":[{\"id\":\"x@openrouter\",\"ctx\":1}]}}", "'ctx'", NULL},
        {"{\"changes\":{\"models\":[{\"id\":\"x@openrouter\","
         "\"status\":\"retired\"}]}}", "healthy", NULL},
        /* agent section: a bare name gets a did-you-mean naming the id;
         * nothing like it registered gets no invented suggestion */
        {AGM("\"deepseek/deepseek-v4.1-flash\""), DS, NULL},
        {AGM("\"ghost\""), "unknown model 'ghost'", "Did you mean"},
        {"{\"changes\":{\"agent\":{\"sandbox_profile\":\"host\"}}}", "unknown agent key", NULL},
        {"{\"changes\":{\"agent\":{\"max_iterations\":0}}}", "positive integer", NULL},
        /* agent.models entries */
        {DSM("\"effort\":\"max\""), "effort 'max'", NULL},
        {DSM("\"reasoning\":\"high\""), "'reasoning'", NULL},
        {AGM("7"), "{id, effort}", NULL},
        {AGM("{\"effort\":\"high\"}"), "canonical model id", NULL},
        {AGM("\"" DS "\",{\"id\":\"" DS "\",\"effort\":\"low\"}"), "duplicate", NULL},
        {DSM("\"trim_threshold\":0"), "trim_threshold", NULL},
        {DSM("\"prune_target\":\"9\""), "prune_target", NULL},
        {DSM("\"trim_threshold\":5000,\"prune_target\":5000"), "below its trim_threshold", NULL},
        /* no threshold = half the window (100000) */
        {DSM("\"prune_target\":50000"), "half the model's window", NULL},
        {DSM("\"cache_lifetime\":-5"), "cache_lifetime", NULL},
        {AGM("{\"id\":\"anthx/c\",\"cache_lifetime\":600}"), "Anthropic", NULL},
        {AGM("{\"id\":\"anthx/c\",\"reasoning_display\":\"full\"}"), "reasoning_display", NULL},
        {AGM("{\"id\":\"anthx/c\",\"reasoning_display\":true}"), "reasoning_display", NULL},
        /* routes: shape, unknown channel, wildcard, foreign owner */
        {"{\"changes\":{\"routes\":[\"tg\"]}}", "'channel:chat_id'", NULL},
        {"{\"changes\":{\"routes\":[\"nochan:1\"]}}", "unknown channel", NULL},
        {"{\"changes\":{\"routes\":[\"tg:*\"]}}", "wildcard", NULL},
        {"{\"changes\":{\"routes\":[\"tg:-500\"]}}", "already owned", NULL},
        /* secret_bindings: a binding request is not where a secret is born;
         * system secrets never interpolate; host shape; array value */
        {SB("\"NOPE\":[\"api.example.com\"]"), "unknown secret", NULL},
        {SB("\"SYS_KEY\":[\"api.example.com\"]"), "unknown secret", NULL},
        {SB("\"ALPACA_KEY\":[\"*.example.com\"]"), "not a valid hostname", NULL},
        {SB("\"ALPACA_KEY\":[\".com\"]"), "not a valid hostname", NULL},
        {SB("\"ALPACA_KEY\":[\"localhost\"]"), "not a valid hostname", NULL},
        {SB("\"ALPACA_KEY\":\"api.example.com\""), "must be an array", NULL},
    };
#undef AGM
#undef DSM
#undef SB
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char id[16];
        snprintf(id, sizeof(id), "e%zu", i);
        char *r = call(&f, id, cases[i].doc);
        int ok = r && strstr(r, "error") && strstr(r, cases[i].needle)
                 && !(cases[i].absent && strstr(r, cases[i].absent));
        if (!ok)
            fprintf(stderr, "row %zu: %s\n  want '%s', got: %s\n", i,
                    cases[i].doc, cases[i].needle, r ? r : "(parked)");
        assert(ok);
        free(r);
    }
    assert(test_qi(f.db, "SELECT COUNT(*) FROM approvals") == 0);
    fx_close(&f);
}

/* The provider key check (A8) and the provider key's provenance: a missing
 * credential is refused while the agent can still fix it; a captured agent
 * secret becomes a system key on approval; a host-bound tool credential or
 * another agent's secret never becomes one. */
static void test_provider_keys(void) {
    Fx f;
    fx_open(&f, ":memory:");
    char *err = call(&f, "pv6", "{\"changes\":{\"provider\":"
        "{\"provider\":\"nokey\",\"base_url\":\"https://n.example/v1\"}}}");
    assert(err && strstr(err, "NOKEY_API_KEY") && strstr(err, "save_secret"));
    free(err);

    exec_ok(f.db,
        "INSERT INTO secrets(name, value, source, scope) VALUES('NOKEY_API_KEY','x','captured','agent');"
        "INSERT INTO grants(agent_name, kind, value, source) VALUES('test','secret','NOKEY_API_KEY','capture');");
    /* Binding the same secret in the same document contradicts that. */
    err = call(&f, "pv6a", "{\"changes\":{\"provider\":"
        "{\"provider\":\"nokey\",\"base_url\":\"https://n.example/v1\"},"
        "\"secret_bindings\":{\"NOKEY_API_KEY\":[\"n.example\"]}}}");
    assert(err && strstr(err, "takes no"));
    free(err);
    assert(call(&f, "pv6b", "{\"changes\":{\"provider\":"
        "{\"provider\":\"nokey\",\"base_url\":\"https://n.example/v1\"}}}") == NULL);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM secrets WHERE name='NOKEY_API_KEY' AND scope='agent'") == 1);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM secrets WHERE name='NOKEY_API_KEY' AND scope='system'") == 1);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM grants WHERE kind='secret' AND value='NOKEY_API_KEY'") == 0);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM providers WHERE name='nokey' AND api_key_env='NOKEY_API_KEY'") == 1);

    exec_ok(f.db,
        "INSERT INTO secrets(name, value, source, scope) VALUES('TOOL_TOKEN','x','captured','agent');"
        "INSERT INTO grants(agent_name, kind, value, source) VALUES('test','secret','TOOL_TOKEN','capture');");
    assert(db_secret_host_bind(f.db, "TOOL_TOKEN", "api.example.com") == 0);
    err = call(&f, "pv6c", "{\"changes\":{\"provider\":"
        "{\"provider\":\"evil\",\"base_url\":\"https://e.example/v1\","
        "\"api_key_env\":\"TOOL_TOKEN\"}}}");
    assert(err && strstr(err, "cannot become a provider key"));
    free(err);
    db_agent_upsert(f.db, "someone_else", NULL, NULL);
    exec_ok(f.db,
        "INSERT INTO secrets(name, value, source, scope) VALUES('OTHERS_KEY','x','captured','agent');"
        "INSERT INTO grants(agent_name, kind, value, source) VALUES('someone_else','secret','OTHERS_KEY','capture');");
    err = call(&f, "pv6d", "{\"changes\":{\"provider\":"
        "{\"provider\":\"evil\",\"base_url\":\"https://e.example/v1\","
        "\"api_key_env\":\"OTHERS_KEY\"}}}");
    assert(err && strstr(err, "cannot become a provider key"));
    free(err);

    /* Keyless is legal, but it has to be said out loud. */
    assert(call(&f, "pv7", "{\"changes\":{\"provider\":"
        "{\"provider\":\"nokey\",\"base_url\":\"https://n.example/v1\","
        "\"api_key_env\":\"\"}}}") == NULL);
    fx_close(&f);
}

/* Provider defaults fill: openrouter with only the name gets its canonical
 * base_url and key name in the parked JSON. */
static void test_provider_defaults(void) {
    Fx f;
    fx_open(&f, ":memory:");
    seed_key(f.db, "OPENROUTER_API_KEY");
    assert(call(&f, "pd1", "{\"changes\":{\"provider\":{\"provider\":\"openrouter\"}}}") == NULL);
    expect_text(f.db, "SELECT json_extract(args_json,'$.changes.provider.base_url') || ' ' ||"
        " json_extract(args_json,'$.changes.provider.api_key_env')"
        " FROM approvals WHERE session_id=?1 ORDER BY id DESC LIMIT 1",
        f.sid, "https://openrouter.ai/api/v1 OPENROUTER_API_KEY");

    /* A7 regression: a deliberately keyless provider survives a provider
     * document. The old canonicalizer re-derived <PROVIDER>_API_KEY whenever
     * api_key_env was absent, and the phantom name made routing drop every
     * one of that provider's models without a word. */
    exec_ok(f.db, "INSERT INTO providers(name, base_url, api_key_env)"
                  " VALUES('gateway','http://127.0.0.1:8080/v1','')");
    assert(call(&f, "pd2", "{\"changes\":{\"provider\":"
        "{\"provider\":\"gateway\",\"base_url\":\"http://127.0.0.1:9090/v1\"}}}") == NULL);
    expect_text(f.db, "SELECT json_extract(args_json,'$.changes.provider.api_key_env')"
        " FROM approvals WHERE session_id=?1 ORDER BY id DESC LIMIT 1", f.sid, "");

    /* …and it stays empty through the apply, too. The receipt is a re-read:
     * it names the endpoint now stored. */
    char *receipt = NULL;
    assert(apply_latest(f.db, f.sid, &receipt) == 0);
    assert(receipt && strstr(receipt, "http://127.0.0.1:9090/v1 (key: none)"));
    free(receipt);
    expect_text(f.db, "SELECT api_key_env FROM providers WHERE name='gateway'", 0, "");
    fx_close(&f);
}

/* The models section: register on an existing provider, update in place,
 * disable. */
static void test_models_section(void) {
    Fx f;
    fx_open(&f, ":memory:");
    seed_key(f.db, "OPENROUTER_API_KEY");
    assert(call(&f, "ms3", "{\"changes\":{\"models\":"
        "[{\"id\":\"newmodel@openrouter\",\"context_window\":200000,"
        "\"max_output_tokens\":8192,\"capabilities\":[\"text\",\"image\"]}]}}") == NULL);
    char *receipt = NULL;
    assert(apply_latest(f.db, f.sid, &receipt) == 0);
    assert(receipt && strstr(receipt, "context 200000"));
    free(receipt);
    assert(test_qi(f.db, "SELECT provider_name='openrouter' AND model='newmodel'"
        " AND context_window=200000 AND max_output_tokens=8192"
        " AND capabilities LIKE '%image%' AND status='healthy'"
        " FROM models WHERE id='newmodel@openrouter'") == 1);

    /* Update: only the fields present move; the rest keep their values. */
    assert(call(&f, "ms4", "{\"changes\":{\"models\":"
        "[{\"id\":\"newmodel@openrouter\",\"context_window\":64000}]}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, "SELECT context_window=64000 AND max_output_tokens=8192"
        " AND status='healthy' FROM models WHERE id='newmodel@openrouter'") == 1);

    /* Disable — a first-class verb, not a DELETE nobody could request. */
    assert(call(&f, "ms5", "{\"changes\":{\"models\":"
        "[{\"id\":\"newmodel@openrouter\",\"status\":\"disabled\"}]}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, "SELECT status='disabled' AND context_window=64000"
        " FROM models WHERE id='newmodel@openrouter'") == 1);
    fx_close(&f);
}

/* Dedup: same doc twice while pending => "already sent"; a different doc
 * parks while the first is pending; expiry is not a denial; a human denial
 * refuses the same changes doc whatever the reason says. */
static void test_dedup(void) {
    Fx f;
    fx_open(&f, ":memory:");
    const char *doc = "{\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}}}";

    assert(call(&f, "dd1", doc) == NULL);
    char *r = call(&f, "dd2", doc);
    assert(r != NULL && strstr(r, "already sent") != NULL);
    free(r);
    assert(call(&f, "dd3", "{\"changes\":{\"grants\":{\"hosts\":[\"example.com\"]}}}") == NULL);

    exec_ok(f.db,
        "UPDATE approvals SET state='denied', decided_via='auto:expired'"
        " WHERE park_reason='approval_required'"
        " AND json_extract(args_json,'$.changes.grants.tools[0]')='shell_exec'");
    assert(call(&f, "dd4", doc) == NULL);

    exec_ok(f.db,
        "UPDATE approvals SET state='denied', decided_via='channel:discord'"
        " WHERE state='pending'"
        " AND json_extract(args_json,'$.changes.grants.tools[0]')='shell_exec'");
    r = call(&f, "dd5",
        "{\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}},"
        "\"reason\":\"different wording, same ask\"}");
    assert(r != NULL && strstr(r, "already denied") != NULL);
    free(r);
    fx_close(&f);
}

/* Batch: one doc with grants+config+provider parks exactly ONE approval,
 * and applying it lands every section. */
static void test_batch_apply(void) {
    Fx f;
    fx_open(&f, ":memory:");
    exec_ok(f.db, "INSERT OR IGNORE INTO config(key, default_value, description)"
                  " VALUES('myext.knob','0','test knob')");
    seed_key(f.db, "OPENROUTER_API_KEY");
    assert(call(&f, "ba1", "{\"changes\":{"
        "\"grants\":{\"tools\":[\"shell_exec\"],\"hosts\":[\"api.example.com\"],"
        "\"read_paths\":[\"/opt/data\"],\"write_paths\":[\"/tmp/out\"]},"
        "\"config\":{\"myext.knob\":\"42\"},"
        "\"provider\":{\"provider\":\"openrouter\"}}}") == NULL);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM approvals") == 1);

    char *receipt = NULL;
    assert(apply_latest(f.db, f.sid, &receipt) == 0);
    /* The receipt is a re-read of every section. */
    assert(receipt && strstr(receipt, "tool shell_exec")
           && strstr(receipt, "myext.knob=42")
           && strstr(receipt, "(key: OPENROUTER_API_KEY)"));
    free(receipt);

    AgentCaps caps;
    agent_caps_load(f.db, "test", &caps);
    assert(caps.tool_count == 1 && strcmp(caps.tools[0], "shell_exec") == 0);
    assert(caps.host_count == 1 && strcmp(caps.hosts[0], "api.example.com") == 0);
    assert(caps.read_count == 1 && strcmp(caps.read_paths[0], "/opt/data") == 0);
    assert(caps.write_count == 1 && strcmp(caps.write_paths[0], "/tmp/out") == 0);
    agent_caps_free(&caps);
    expect_text(f.db, "SELECT value FROM config WHERE key='myext.knob'", 0, "42");
    expect_text(f.db, "SELECT base_url || ' ' || api_key_env FROM providers"
                      " WHERE name='openrouter'", 0,
                "https://openrouter.ai/api/v1 OPENROUTER_API_KEY");
    fx_close(&f);
}

/* agent + routes sections in one document: define a transport, register a
 * model on it, adopt it, take routes; a route captured between park and
 * apply rolls the whole document back. */
static void test_agent_routes_apply(void) {
    Fx f;
    fx_open(&f, ":memory:");
    db_agent_upsert(f.db, "Other", NULL, NULL);
    exec_ok(f.db,
        "INSERT INTO extensions(name,path) VALUES('tgext','/x');"
        "INSERT INTO channels(name,extension_name) VALUES('tg','tgext');"
        /* a chat the gate saw but nobody routed, starting from a filter */
        "INSERT INTO channel_chats(channel_name,chat_id,tool_filter,title)"
        " VALUES('tg','999','[\"web_fetch\"]','Lobby');");
    assert(session_set_state(f.db, f.sid, "tool_running") == 0);
    seed_key(f.db, "GEMINI_API_KEY");
    assert(call(&f, "ar1", "{\"changes\":{"
        "\"provider\":{\"provider\":\"gemini\"},"
        "\"models\":[{\"id\":\"gemini-3.5-flash-lite@gemini\","
        "\"context_window\":1000000}],"
        "\"agent\":{\"models\":[\"gemini-3.5-flash-lite@gemini\"],"
        "\"max_iterations\":40},"
        "\"routes\":[\"tg:777\",\"tg:999\"]}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);

    assert(test_qi(f.db, "SELECT provider_name='gemini' AND model='gemini-3.5-flash-lite'"
        " FROM models WHERE id='gemini-3.5-flash-lite@gemini'") == 1);
    /* routing list replaced; untouched scalar columns kept. */
    assert(test_qi(f.db, "SELECT (SELECT model_id FROM agent_models"
        "   WHERE agent_name='test' ORDER BY pos LIMIT 1)='gemini-3.5-flash-lite@gemini'"
        " AND max_iterations=40 AND shell_timeout=30 FROM agents WHERE name='test'") == 1);
    /* Routes pinned to sessions this agent owns, explicit delivery (send
     * authority only); the seen chat keeps its filter and title. */
    assert(test_qi(f.db, "SELECT se.agent_name='test' AND r.delivery_mode='explicit'"
        " AND se.channel_name='tg' AND se.chat_id='777'"
        " FROM channel_chats r JOIN sessions se ON se.id=r.session_id"
        " WHERE r.channel_name='tg' AND r.chat_id='777'") == 1);
    assert(test_qi(f.db, "SELECT s.agent_name='test' AND c.delivery_mode='explicit'"
        " AND c.tool_filter='[\"web_fetch\"]' AND c.title='Lobby'"
        " AND s.channel_name='tg' AND s.chat_id='999'"
        " FROM channel_chats c JOIN sessions s ON s.id=c.session_id"
        " WHERE c.chat_id='999';") == 1);

    /* Route captured by another agent between park and apply → the whole
     * document rolls back (savepoint), including the agent section. */
    assert(call(&f, "ar2", "{\"changes\":{"
        "\"agent\":{\"max_iterations\":99},\"routes\":[\"tg:888\"]}}") == NULL);
    assert(channel_pin_new_session(f.db, "tg", "888", "Other", "o2") > 0);
    assert(apply_latest(f.db, f.sid, NULL) == -1);
    assert(test_qi(f.db, "SELECT max_iterations FROM agents WHERE name='test'") == 40);
    fx_close(&f);
}

/* agent.models {id, effort} objects: effort lands on the routing row, a
 * bare-string replacement clears it (whole-list replace), and only the chat
 * list is replaced (v59). */
static void test_agent_models_effort(void) {
    Fx f;
    fx_open(&f, ":memory:");
    assert(session_set_state(f.db, f.sid, "tool_running") == 0);
    assert(call(&f, "ef1", "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"" DS "\",\"effort\":\"high\"}]}}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM agent_models WHERE agent_name='test'") == 1);
    assert(test_qi(f.db, "SELECT model_id='" DS "' AND reasoning_effort='high'"
        " FROM agent_models WHERE agent_name='test'") == 1);

    exec_ok(f.db,
        "INSERT INTO models(id, provider_name, model, capabilities)"
        " VALUES('ears@openrouter','openrouter','ears','[\"audio\"]');"
        "INSERT INTO agent_models(agent_name, purpose, model_id, pos)"
        " VALUES('test','audio','ears@openrouter',0);");
    assert(call(&f, "ef2", "{\"changes\":{\"agent\":{\"models\":[\"" DS "\"]}}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, "SELECT reasoning_effort IS NULL FROM agent_models"
        " WHERE agent_name='test' AND purpose='chat'") == 1);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM agent_models WHERE agent_name='test'"
        " AND purpose='audio' AND model_id='ears@openrouter'") == 1);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM agent_models WHERE agent_name='test'"
        " AND purpose='chat'") == 1);
    fx_close(&f);
}

/* agent.models trim_threshold / prune_target / cache_lifetime /
 * reasoning_display reach the routing row; a threshold over the model's
 * window is refused by the schema at apply, said by name. */
static void test_agent_models_trim(void) {
    Fx f;
    fx_open(&f, ":memory:");
    exec_ok(f.db, "UPDATE models SET context_window=100000 WHERE id='" DS "';");
    assert(session_set_state(f.db, f.sid, "tool_running") == 0);
#define CHAT_COL(expr) "SELECT " expr " FROM agent_models" \
                       " WHERE agent_name='test' AND purpose='chat'"

    assert(call(&f, "tr1", "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"" DS "\",\"trim_threshold\":60000,\"prune_target\":20000}]}}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, CHAT_COL("trim_threshold=60000 AND prune_target=20000")) == 1);

    assert(call(&f, "tr2", "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"" DS "\",\"trim_threshold\":200000}]}}}") == NULL);
    char *detail = NULL;
    assert(apply_latest(f.db, f.sid, &detail) == -1);
    assert(detail && strstr(detail, "context window"));
    free(detail);
    assert(test_qi(f.db, CHAT_COL("trim_threshold")) == 60000);

    assert(call(&f, "tr3", "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"" DS "\",\"cache_lifetime\":600}]}}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, CHAT_COL("cache_lifetime")) == 600);

    /* An Anthropic model left without a lifetime gets 3600. */
    exec_ok(f.db,
        "INSERT INTO providers(name, base_url, endpoint_type) VALUES"
        " ('anthx','http://a','anthropic');"
        "INSERT INTO models(id, provider_name, model) VALUES ('anthx/c','anthx','c');");
    assert(call(&f, "tr4", "{\"changes\":{\"agent\":{\"models\":[\"anthx/c\"]}}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, CHAT_COL("cache_lifetime")) == 3600);

    /* reasoning_display: summary by default, none applied, left out resets. */
    assert(test_qi(f.db, CHAT_COL("reasoning_display='summary'")) == 1);
    assert(call(&f, "tr5", "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"anthx/c\",\"reasoning_display\":\"none\"}]}}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, CHAT_COL("reasoning_display='none'")) == 1);
    assert(call(&f, "tr6", "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"anthx/c\",\"effort\":\"low\"}]}}}") == NULL);
    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, CHAT_COL("reasoning_display='summary'")) == 1);
#undef CHAT_COL
    fx_close(&f);
}

/* A document with a valid grant and an unregistered config key (built by
 * hand, past the handler) applies nothing. */
static void test_apply_rollback(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    assert(request_config_changes_apply(db, "test",
        "{\"changes\":{\"grants\":{\"tools\":[\"web_fetch\"]},"
        "\"config\":{\"nonexistent_key_xyz\":\"boom\"}}}", 0, 0, 0, NULL) == -1);
    AgentCaps caps;
    agent_caps_load(db, "test", &caps);
    assert(caps.tool_count == 0);
    agent_caps_free(&caps);
    db_close(db);
}

/* A path grant covering cclaw.db is refused at request time, with the reason —
 * it never parks, so no approver is ever shown a grant that would be inert.
 * Needs an on-disk DB: the rule resolves the handle's own filename.
 *
 * Positive control in the same function: an ordinary path grant still parks. */
static void test_db_path_grant_refused(void) {
    assert(system("rm -rf /tmp/cclaw_reqcfg_grantdb && mkdir -p /tmp/cclaw_reqcfg_grantdb") == 0);
    Fx f;
    fx_open(&f, "/tmp/cclaw_reqcfg_grantdb/cclaw.db");

    char *r = call(&f, "d1", "{\"changes\":{\"grants\":{\"read_paths\":[\"/usr/share\"]}}}");
    assert(r == NULL || strstr(r, "error:") == NULL);
    free(r);

    r = call(&f, "d2", "{\"changes\":"
        "{\"grants\":{\"read_paths\":[\"/tmp/cclaw_reqcfg_grantdb/cclaw.db\"]}}}");
    assert(r != NULL && strstr(r, "cclaw.db") != NULL);
    free(r);

    /* The containing directory is the realistic ask, and is refused too. */
    r = call(&f, "d3", "{\"changes\":"
        "{\"grants\":{\"write_paths\":[\"/tmp/cclaw_reqcfg_grantdb\"]}}}");
    assert(r != NULL && strstr(r, "cclaw.db") != NULL);
    free(r);

    fx_close(&f);
    assert(system("rm -rf /tmp/cclaw_reqcfg_grantdb") == 0);
}

/* Redundant requests: fully-satisfied documents return a helpful result
 * instead of parking; partially-satisfied ones park only the remainder. */
static void test_redundant_filtered(void) {
    Fx f;
    fx_open(&f, ":memory:");
    assert(agent_config_grant(f.db, "test", "host", "api.tiingo.com", 0, GRANT_ORIGIN("operator", 0)) == 0);
    assert(agent_config_grant(f.db, "test", "tool", "js_eval", 0, GRANT_ORIGIN("operator", 0)) == 0);
    assert(session_set_state(f.db, f.sid, "tool_running") == 0);

    /* Fully redundant: no approval parked, session state untouched. */
    char *result = call(&f, "r1", "{\"changes\":"
        "{\"grants\":{\"hosts\":[\"api.tiingo.com\"],\"tools\":[\"js_eval\"]}}}");
    assert(result != NULL && strstr(result, "already in effect") != NULL);
    free(result);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM approvals") == 0);
    expect_text(f.db, "SELECT state FROM sessions WHERE id=?1", f.sid, "tool_running");

    /* Partially redundant: parked document keeps only the new host. */
    assert(call(&f, "r2", "{\"changes\":"
        "{\"grants\":{\"hosts\":[\"api.tiingo.com\",\"example.com\"]}}}") == NULL);
    expect_text(f.db, "SELECT json_extract(args_json,'$.changes.grants.hosts')"
        " FROM approvals WHERE session_id=?1 AND state='pending'", f.sid,
        "[\"example.com\"]");

    /* Redundant grants alongside a live section: grants stripped, agent
     * section still parks. */
    assert(call(&f, "r3", "{\"changes\":"
        "{\"grants\":{\"tools\":[\"js_eval\"]},\"agent\":{\"max_iterations\":9}}}") == NULL);
    assert(test_qi(f.db, "SELECT json_type(args_json,'$.changes.grants') IS NULL"
        " AND json_extract(args_json,'$.changes.agent.max_iterations')=9"
        " FROM approvals WHERE state='pending' AND tool_call_id='r3'") == 1);
    fx_close(&f);
}

/* secret_bindings: park, no rows while pending, apply mints secret_hosts
 * rows, then the redundancy filter. */
static void test_secret_bindings_section(void) {
    Fx f;
    fx_open(&f, ":memory:");
    exec_ok(f.db, "INSERT INTO secrets(name,value,scope) VALUES ('ALPACA_KEY','enc:00','agent')");

    assert(call(&f, "sb1", "{\"changes\":{\"secret_bindings\":{\"ALPACA_KEY\":"
        "[\"api.alpaca.markets\",\".example.com\"]}}}") == NULL);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM approvals WHERE state='pending'"
        " AND json_type(args_json,'$.changes.secret_bindings') IS NOT NULL") == 1);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM secret_hosts") == 0);

    assert(apply_latest(f.db, f.sid, NULL) == 0);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM secret_hosts WHERE secret_name='ALPACA_KEY'"
        " AND host IN ('api.alpaca.markets','.example.com')") == 2);

    assert(session_set_state(f.db, f.sid, "tool_running") == 0);
    char *r = call(&f, "sb2", "{\"changes\":{\"secret_bindings\":"
        "{\"ALPACA_KEY\":[\"api.alpaca.markets\"]}}}");
    assert(r && strstr(r, "already in effect"));
    free(r);

    assert(call(&f, "sb3", "{\"changes\":{\"secret_bindings\":{\"ALPACA_KEY\":"
        "[\"api.alpaca.markets\",\"api.tiingo.com\"]}}}") == NULL);
    expect_text(f.db, "SELECT json_extract(args_json,'$.changes.secret_bindings.ALPACA_KEY')"
        " FROM approvals WHERE session_id=?1 AND state='pending' AND tool_call_id='sb3'",
        f.sid, "[\"api.tiingo.com\"]");
    fx_close(&f);
}

/* grants.remove — narrowing applies immediately and never parks. */
static void test_grants_remove(void) {
    Fx f;
    fx_open(&f, ":memory:");
    assert(agent_config_grant(f.db, "test", "host", "api.example.com", 0, GRANT_ORIGIN("operator", 0)) == 0);
    assert(agent_config_grant(f.db, "test", "tool", "shell_exec", 0, GRANT_ORIGIN("operator", 0)) == 0);
#define HAS_SHELL "SELECT COUNT(*) FROM grants WHERE kind='tool' AND value='shell_exec'"

    /* Happy path: applied here and now, no approval row, receipt names it. */
    char *r = call(&f, "rm1",
        "{\"changes\":{\"grants\":{\"remove\":{\"hosts\":[\"api.example.com\"]}}}}");
    assert(r != NULL && strstr(r, "api.example.com") && strstr(r, "removed"));
    free(r);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM grants WHERE agent_name='test'"
                         " AND kind='host' AND value='api.example.com'") == 0);
    assert(test_qi(f.db, "SELECT COUNT(*) FROM approvals") == 0);
    assert(test_qi(f.db, HAS_SHELL) == 1);

    /* Unknown kind refuses — and cannot reach containment settings. */
    r = call(&f, "rm2",
        "{\"changes\":{\"grants\":{\"remove\":{\"sandbox_profile\":[\"host\"]}}}}");
    assert(r != NULL && strstr(r, "sandbox_profile") && strstr(r, "error"));
    free(r);

    /* A value that is not a live grant refuses, naming it. */
    r = call(&f, "rm3",
        "{\"changes\":{\"grants\":{\"remove\":{\"hosts\":[\"nope.example\"]}}}}");
    assert(r != NULL && strstr(r, "nope.example") && strstr(r, "not one of"));
    free(r);
    assert(test_qi(f.db, HAS_SHELL) == 1);

    /* Mixed remove + widen: the removal lands now, the rest parks. */
    assert(session_set_state(f.db, f.sid, "tool_running") == 0);
    assert(call(&f, "rm4", "{\"changes\":{\"grants\":{\"hosts\":[\"new.example.com\"],"
        "\"remove\":{\"tools\":[\"shell_exec\"]}}}}") == NULL);
    assert(test_qi(f.db, HAS_SHELL) == 0);
    /* The parked document carries the widening only; the removal already
     * happened and rides the approver-facing reason. */
    assert(test_qi(f.db,
        "SELECT COUNT(*) FROM approvals WHERE state='pending'"
        "   AND json_extract(args_json,'$.changes.grants.hosts[0]')"
        "       ='new.example.com'"
        "   AND json_extract(args_json,'$.changes.grants.remove') IS NULL"
        "   AND json_extract(args_json,'$.reason') LIKE '%shell_exec%'") == 1);
#undef HAS_SHELL
    fx_close(&f);
}

/* grants.secrets (v60): an agent asks to USE a secret like any other grant.
 * Only a name an agent could be handed may be asked for — a stored
 * agent-scope secret, or an environment secret the database knows through a
 * binding — never a system secret, never a name nothing stands behind. The
 * card names the secret plainly; the applied row records the approval. */
static void test_grants_secrets(void) {
    Fx f;
    fx_open(&f, ":memory:");
    exec_ok(f.db,
        "INSERT INTO secrets(name,value,scope) VALUES"
        " ('STORED_KEY','enc:00','agent'),('SYS_KEY','enc:00','system');"
        "INSERT INTO secret_hosts(secret_name,host) VALUES('ENV_TOKEN','api.example.com');");

    char *r = call(&f, "gs1", "{\"changes\":{\"grants\":{\"secrets\":[\"NOPE\"]}}}");
    assert(r && strstr(r, "grants.secrets names 'NOPE'"));
    free(r);
    r = call(&f, "gs1", "{\"changes\":{\"grants\":{\"secrets\":[\"SYS_KEY\"]}}}");
    assert(r && strstr(r, "grants.secrets names 'SYS_KEY'"));
    free(r);
    r = call(&f, "gs1", "{\"changes\":{\"grants\":{\"secrets\":[7]}}}");
    assert(r && strstr(r, "non-empty strings"));
    free(r);

    assert(call(&f, "gs1", "{\"changes\":{\"grants\":{\"secrets\":"
                           "[\"STORED_KEY\",\"ENV_TOKEN\"]}}}") == NULL);
    int64_t aid = db_scalar_i64(f.db, "SELECT id FROM approvals WHERE session_id=?1"
        " AND state='pending' AND tool_call_id='gs1'", f.sid, -1);
    assert(aid > 0);
    Approval *a = approval_get_for_tool_call(f.db, f.sid, "gs1");
    assert(a);
    char *card = approval_format_summary(f.db, a);
    assert(card && strstr(card, "secret STORED_KEY (may use its value in tool calls)"));
    assert(strstr(card, "secret ENV_TOKEN (may use its value in tool calls)"));
    free(card);
    assert(grants_contains(f.db, "test", "secret", "STORED_KEY") == 0);
    assert(request_config_changes_apply(f.db, "test", a->args_json, 0, f.sid, aid, NULL) == 0);
    approval_free(a);
    assert(db_scalar_i64(f.db, "SELECT COUNT(*) FROM grants WHERE agent_name='test'"
        " AND kind='secret' AND source='approval' AND approval_id=?1", aid, -1) == 2);

    /* Giving one back applies immediately, like any grant. */
    assert(session_set_state(f.db, f.sid, "tool_running") == 0);
    r = call(&f, "gs2", "{\"changes\":{\"grants\":{\"remove\":"
                        "{\"secrets\":[\"ENV_TOKEN\"]}}}}");
    assert(r && strstr(r, "removed"));
    free(r);
    assert(grants_contains(f.db, "test", "secret", "ENV_TOKEN") == 0);
    assert(grants_contains(f.db, "test", "secret", "STORED_KEY") == 1);
    fx_close(&f);
}

int main(void) {
    TEST_INIT();
    printf("test_request_config:\n");
    RUN(test_park_tools_grant);
    RUN(test_refusals);
    RUN(test_db_path_grant_refused);
    RUN(test_provider_keys);
    RUN(test_provider_defaults);
    RUN(test_models_section);
    RUN(test_dedup);
    RUN(test_batch_apply);
    RUN(test_agent_routes_apply);
    RUN(test_agent_models_effort);
    RUN(test_agent_models_trim);
    RUN(test_apply_rollback);
    RUN(test_redundant_filtered);
    RUN(test_secret_bindings_section);
    RUN(test_grants_remove);
    RUN(test_grants_secrets);
    printf("All request_config tests passed.\n");
    return 0;
}
