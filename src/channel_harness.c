/* channel_harness — offline fixture-replay test harness for channel JS.
 *
 * `cclaw --channel <name> --harness <scenario.json>` drives the same JS a
 * channel would run in production, but never touches the network or the
 * real cclaw.db: the network seam (cclaw.send) is matched against scenario
 * fixtures instead of hitting curl, and all DB state (channel_outbox,
 * channel_events, admin.* config) lives in a scratch DB this process creates
 * and deletes. Lets an authoring agent (or a human) prove a channel's JS
 * behaves before --check/--activate ever puts it in front of real traffic.
 *
 * Scenario JSON:
 *   { "fixtures": [ {"match": {"method","url_substr"}, "respond": {"status","body"}} ],
 *     "steps":    [ {"inject_outbox": "..."},
 *                   {"expect_event": {"contains": "..."}},
 *                   {"expect_http": {"url_substr": "...", "body_contains": "..."}} ] }
 *
 * An unmatched HTTP request fails the scenario immediately (fail-closed —
 * nothing escapes to a real network by falling through unmatched). */
#define _POSIX_C_SOURCE 200809L
#include "channel_api.h"
#include "channel_harness.h"
#include "channel_runner.h"
#include "db.h"
#include "jsmn_util.h"
#include "json_escape.h"
#include "log.h"
#include "qjs_helpers.h"
#include "secret.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define HARNESS_MAX_TOKENS 4096
#define HARNESS_MAX_FIXTURES 32
#define HARNESS_MAX_STEPS 64
#define HARNESS_HEAP_SIZE (2 * 1024 * 1024)
#define HARNESS_MAX_INSTRUCTIONS 100000000

typedef struct {
    char *method;       /* NULL = match any method */
    char *url_substr;   /* NULL = match any url */
    int status;
    char *body;
} Fixture;

typedef enum { STEP_INJECT_OUTBOX, STEP_EXPECT_EVENT, STEP_EXPECT_HTTP } StepKind;

typedef struct {
    StepKind kind;
    char *a;   /* inject_outbox payload / expect_event.contains / expect_http.url_substr */
    char *b;   /* expect_http.body_contains (may be NULL) */
} Step;

extern ChannelCtx *g_ctx;  /* channel_runner.c's global — this TU binds it too */

/* ── tiny jsmn helpers (scenario JSON is untrusted input, per project rules —
 * see AGENTS.md: jsmn for untrusted/streaming JSON, SQLite JSON for data we
 * own) ── */

static char *jtok_dup(const jsmntok_t *t, const char *json) {
    if (!t || t->type != JSMN_STRING) return NULL;
    size_t len = (size_t)(t->end - t->start);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t n = json_unescape(out, len + 1, json + t->start, len);
    out[n] = '\0';
    return out;
}

static int jtok_int(const jsmntok_t *t, const char *json, int dflt) {
    if (!t || t->type != JSMN_PRIMITIVE) return dflt;
    return atoi(json + t->start);
}

/* ── scenario parsing ─────────────────────────────────────────────── */

typedef struct {
    Fixture fixtures[HARNESS_MAX_FIXTURES];
    int n_fixtures;
    Step steps[HARNESS_MAX_STEPS];
    int n_steps;
} Scenario;

static int parse_scenario(const char *json, Scenario *sc) {
    memset(sc, 0, sizeof(*sc));
    jsmntok_t *toks = malloc(sizeof(jsmntok_t) * HARNESS_MAX_TOKENS);
    if (!toks) return -1;
    jsmn_parser p;
    jsmn_init(&p);
    int nt = jsmn_parse(&p, json, strlen(json), toks, HARNESS_MAX_TOKENS);
    if (nt < 1 || toks[0].type != JSMN_OBJECT) { free(toks); return -1; }

    int fi = jtok_find(toks, nt, json, "fixtures");
    if (fi >= 0 && toks[fi].type == JSMN_ARRAY) {
        int j = fi + 1;
        for (int i = 0; i < toks[fi].size && sc->n_fixtures < HARNESS_MAX_FIXTURES; i++) {
            int obj = j;
            int mi = jtok_find(&toks[obj], nt - obj, json, "match");
            int ri = jtok_find(&toks[obj], nt - obj, json, "respond");
            Fixture *fx = &sc->fixtures[sc->n_fixtures];
            fx->status = 200;
            if (mi >= 0) {
                mi += obj;
                int meth = jtok_find(&toks[mi], nt - mi, json, "method");
                int url = jtok_find(&toks[mi], nt - mi, json, "url_substr");
                if (meth >= 0) fx->method = jtok_dup(&toks[mi + meth], json);
                if (url >= 0) fx->url_substr = jtok_dup(&toks[mi + url], json);
            }
            if (ri >= 0) {
                ri += obj;
                int st = jtok_find(&toks[ri], nt - ri, json, "status");
                int bd = jtok_find(&toks[ri], nt - ri, json, "body");
                if (st >= 0) fx->status = jtok_int(&toks[ri + st], json, 200);
                if (bd >= 0) fx->body = jtok_dup(&toks[ri + bd], json);
            }
            sc->n_fixtures++;
            j = jsmn_skip(toks, obj, nt);
        }
    }

    int si = jtok_find(toks, nt, json, "steps");
    if (si >= 0 && toks[si].type == JSMN_ARRAY) {
        int j = si + 1;
        for (int i = 0; i < toks[si].size && sc->n_steps < HARNESS_MAX_STEPS; i++) {
            int obj = j;
            Step *st = &sc->steps[sc->n_steps];
            int io = jtok_find(&toks[obj], nt - obj, json, "inject_outbox");
            int ee = jtok_find(&toks[obj], nt - obj, json, "expect_event");
            int eh = jtok_find(&toks[obj], nt - obj, json, "expect_http");
            if (io >= 0) {
                st->kind = STEP_INJECT_OUTBOX;
                st->a = jtok_dup(&toks[obj + io], json);
                sc->n_steps++;
            } else if (ee >= 0) {
                ee += obj;
                int c = jtok_find(&toks[ee], nt - ee, json, "contains");
                st->kind = STEP_EXPECT_EVENT;
                st->a = c >= 0 ? jtok_dup(&toks[ee + c], json) : NULL;
                sc->n_steps++;
            } else if (eh >= 0) {
                eh += obj;
                int u = jtok_find(&toks[eh], nt - eh, json, "url_substr");
                int b = jtok_find(&toks[eh], nt - eh, json, "body_contains");
                st->kind = STEP_EXPECT_HTTP;
                st->a = u >= 0 ? jtok_dup(&toks[eh + u], json) : NULL;
                st->b = b >= 0 ? jtok_dup(&toks[eh + b], json) : NULL;
                sc->n_steps++;
            }
            j = jsmn_skip(toks, obj, nt);
        }
    }

    free(toks);
    return 0;
}

static void scenario_free(Scenario *sc) {
    for (int i = 0; i < sc->n_fixtures; i++) {
        free(sc->fixtures[i].method); free(sc->fixtures[i].url_substr); free(sc->fixtures[i].body);
    }
    for (int i = 0; i < sc->n_steps; i++) { free(sc->steps[i].a); free(sc->steps[i].b); }
}

/* ── recorded seam traffic (for the transcript + expect_http) ────── */

#define HARNESS_MAX_LOG 256
typedef struct { char *url; char *body; int matched; int status; } LoggedReq;

static LoggedReq g_log[HARNESS_MAX_LOG];
static int g_log_count;
static int g_all_passed = 1;

static void log_req(const char *url, const char *body, int matched, int status) {
    if (g_log_count >= HARNESS_MAX_LOG) return;
    LoggedReq *l = &g_log[g_log_count++];
    l->url = url ? strdup(url) : strdup("");
    l->body = body ? strdup(body) : strdup("");
    l->matched = matched;
    l->status = status;
}

static const Fixture *match_fixture(const Scenario *sc, const char *method, const char *url) {
    for (int i = 0; i < sc->n_fixtures; i++) {
        const Fixture *fx = &sc->fixtures[i];
        if (fx->method && (!method || strcasecmp(fx->method, method) != 0)) continue;
        if (fx->url_substr && (!url || !strstr(url, fx->url_substr))) continue;
        return fx;
    }
    return NULL;
}

/* Drain the send queue cclaw.send() filled during the last JS callback,
 * matching each request against scenario fixtures instead of curl. An
 * unmatched request fails the scenario — fail-closed, nothing goes out. */
static void drain_sends(JSContext *ctx, const Scenario *sc) {
    SendReq *r;
    while ((r = send_queue_pop()) != NULL) {
        const Fixture *fx = match_fixture(sc, r->method, r->url);
        if (fx) {
            printf("  http  %-6s %s -> %d\n", r->method ? r->method : "GET", r->url, fx->status);
            log_req(r->url, r->body, 1, fx->status);
            if (r->js_ctx) {
                /* channel.http(): settle the promise the way the live runner
                 * would. save_to writes the fixture body to the real spool
                 * path so JS (and any later C stage) sees an actual file.
                 * Settling runs awaited continuations, which may push more
                 * sends — the while loop picks them up. */
                const char *body = fx->body ? fx->body : "";
                if (r->save_to && fx->status >= 200 && fx->status < 300) {
                    FILE *f = fopen(r->save_to, "wb");
                    if (f) {
                        long n = (long)fwrite(body, 1, strlen(body), f);
                        fclose(f);
                        send_req_settle(ctx, r, fx->status, NULL, r->save_to, n, NULL);
                    } else {
                        send_req_settle(ctx, r, 0, NULL, NULL, 0,
                                        "harness: cannot open save_to file");
                    }
                } else {
                    /* non-save_to, or non-2xx (error body stays readable text) */
                    send_req_settle(ctx, r, fx->status, body, NULL, 0, NULL);
                }
            }
        } else {
            printf("  http  %-6s %s -> UNMATCHED (no fixture; scenario fails)\n",
                   r->method ? r->method : "GET", r->url);
            log_req(r->url, r->body, 0, 0);
            g_all_passed = 0;
            if (r->js_ctx)
                send_req_settle(ctx, r, 0, NULL, NULL, 0, "harness: no matching fixture");
        }
        send_req_free(r);
    }
}

static int channel_event_contains(sqlite3 *db, const char *channel_name, const char *needle) {
    const char *sql = "SELECT payload FROM channel_events WHERE channel_name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
    int found = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *payload = (const char *)sqlite3_column_text(s, 0);
        if (payload && strstr(payload, needle)) { found = 1; break; }
    }
    sqlite3_finalize(s);
    return found;
}

int channel_harness_run(const char *db_path, const char *channel_name, const char *scenario_path) {
    /* Reset per-run state: a prior scenario in this process (tests call this
     * repeatedly) must not leak its verdict or request log into this one. */
    g_all_passed = 1;
    for (int i = 0; i < g_log_count; i++) { free(g_log[i].url); free(g_log[i].body); }
    g_log_count = 0;

    char *scenario_json = util_read_file(scenario_path, NULL);
    if (!scenario_json) {
        fprintf(stderr, "harness: cannot read scenario '%s'\n", scenario_path);
        return 1;
    }
    Scenario sc;
    if (parse_scenario(scenario_json, &sc) != 0) {
        free(scenario_json);
        fprintf(stderr, "harness: invalid scenario JSON\n");
        return 1;
    }
    free(scenario_json);

    /* Resolve the channel's JS from the real DB (read-only lookup) — the
     * harness runs the channel that's actually registered, just against a
     * scratch DB instead of the real one. */
    sqlite3 *real_db = db_open(db_path);
    if (!real_db) { fprintf(stderr, "harness: cannot open %s\n", db_path); scenario_free(&sc); return 1; }
    char js_path[1024];
    int resolved = resolve_js_path(real_db, channel_name, js_path, sizeof(js_path));
    db_close(real_db);
    if (resolved != 0) {
        fprintf(stderr, "harness: no channel '%s' registered in %s\n", channel_name, db_path);
        scenario_free(&sc);
        return 1;
    }
    char *js_src = util_read_file(js_path, NULL);
    if (!js_src) {
        fprintf(stderr, "harness: cannot read %s\n", js_path);
        scenario_free(&sc);
        return 1;
    }

    /* Scratch DB: fresh schema, minimal channels/extensions rows so
     * ChannelCtx + admin.* + channel_outbox/channel_events all work exactly
     * as they would against the real DB — just disposable. */
    char scratch_path[256];
    snprintf(scratch_path, sizeof(scratch_path), "/tmp/cclaw_harness_%d.db", (int)getpid());
    unlink(scratch_path);
    sqlite3 *sdb = db_open(scratch_path);
    if (!sdb || db_ensure_schema(sdb) != 0) {
        fprintf(stderr, "harness: scratch DB init failed\n");
        free(js_src); scenario_free(&sc);
        if (sdb) db_close(sdb);
        unlink(scratch_path);
        return 1;
    }
    {
        sqlite3_stmt *s;
        sqlite3_prepare_v2(sdb,
            "INSERT INTO extensions(name, path, version, owner_agent, published, enabled)"
            " VALUES(?,?,'0.0.0','harness',0,1);", -1, &s, NULL);
        sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, js_path, -1, SQLITE_STATIC);
        sqlite3_step(s); sqlite3_finalize(s);
        sqlite3_prepare_v2(sdb,
            "INSERT INTO channels(name, extension_name, type, binary_path, status)"
            " VALUES(?,?,'harness',?,'validated');", -1, &s, NULL);
        sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, channel_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, js_path, -1, SQLITE_STATIC);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    db_close(sdb);

    g_ctx = channel_ctx_open(scratch_path, channel_name);
    if (!g_ctx) {
        fprintf(stderr, "harness: scratch ChannelCtx open failed\n");
        free(js_src); scenario_free(&sc);
        unlink(scratch_path);
        return 1;
    }
    { uint8_t sk[32]; if (secret_key_load_or_create(scratch_path, sk) == 0) db_set_secret_key(sk); }

    QjsRuntime *qrt = qjs_runtime_create(HARNESS_HEAP_SIZE);
    if (!qrt) {
        fprintf(stderr, "harness: qjs runtime init failed\n");
        free(js_src); scenario_free(&sc);
        channel_ctx_free(g_ctx); unlink(scratch_path);
        return 1;
    }
    qjs_set_interrupt_limit(qrt, HARNESS_MAX_INSTRUCTIONS);
    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_CHANNEL);
    if (!ctx) {
        fprintf(stderr, "harness: qjs context init failed\n");
        free(js_src); scenario_free(&sc);
        qjs_runtime_destroy(qrt); channel_ctx_free(g_ctx); unlink(scratch_path);
        return 1;
    }
    qjs_register_channel_host_functions(ctx);

    JSValue load_val = JS_Eval(ctx, js_src, strlen(js_src), js_path, JS_EVAL_TYPE_GLOBAL);
    free(js_src);
    if (JS_IsException(load_val)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "harness: JS load error: %s\n", msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        JS_FreeContext(ctx); qjs_runtime_destroy(qrt); channel_ctx_free(g_ctx);
        unlink(scratch_path); scenario_free(&sc);
        return 1;
    }
    JS_FreeValue(ctx, load_val);

    printf("channel harness: %s (%d fixture%s, %d step%s)\n", channel_name,
           sc.n_fixtures, sc.n_fixtures == 1 ? "" : "s", sc.n_steps, sc.n_steps == 1 ? "" : "s");

    printf("  init  onInit()\n");
    JSValue init_ret = eval_js(ctx, "onInit()", "<harness-init>");
    JS_FreeValue(ctx, init_ret);
    drain_sends(ctx, &sc);

    for (int i = 0; i < sc.n_steps; i++) {
        Step *st = &sc.steps[i];
        switch (st->kind) {
        case STEP_INJECT_OUTBOX: {
            printf("  step  inject_outbox: %s\n", st->a ? st->a : "");
            sqlite3_stmt *s;
            sqlite3_prepare_v2(g_ctx->db,
                "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES(?,0,?);",
                -1, &s, NULL);
            sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 2, st->a ? st->a : "", -1, SQLITE_STATIC);
            sqlite3_step(s); sqlite3_finalize(s);

            ChannelOutboxRow *row;
            while ((row = channel_next_outbox(g_ctx)) != NULL) {
                channel_dispatch_outbox(g_ctx, row->id);
                call_on_outbox(ctx, row);
                channel_outbox_row_free(row);
            }
            drain_sends(ctx, &sc);
            break;
        }
        case STEP_EXPECT_EVENT: {
            int ok = st->a && channel_event_contains(g_ctx->db, channel_name, st->a);
            printf("  check expect_event contains \"%s\" -> %s\n",
                   st->a ? st->a : "", ok ? "PASS" : "FAIL");
            if (!ok) g_all_passed = 0;
            break;
        }
        case STEP_EXPECT_HTTP: {
            int ok = 0;
            for (int k = 0; k < g_log_count; k++) {
                if (st->a && !strstr(g_log[k].url, st->a)) continue;
                if (st->b && !strstr(g_log[k].body, st->b)) continue;
                ok = g_log[k].matched;
                if (ok) break;
            }
            printf("  check expect_http url~\"%s\"%s%s -> %s\n",
                   st->a ? st->a : "",
                   st->b ? " body~" : "", st->b ? st->b : "",
                   ok ? "PASS" : "FAIL");
            if (!ok) g_all_passed = 0;
            break;
        }
        }
    }

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
    channel_ctx_free(g_ctx); g_ctx = NULL;
    unlink(scratch_path);
    char wal[300], shm[300];
    snprintf(wal, sizeof(wal), "%s-wal", scratch_path); unlink(wal);
    snprintf(shm, sizeof(shm), "%s-shm", scratch_path); unlink(shm);

    for (int i = 0; i < g_log_count; i++) { free(g_log[i].url); free(g_log[i].body); }
    g_log_count = 0;
    scenario_free(&sc);

    printf("%s\n", g_all_passed ? "PASS: all expectations met" : "FAIL: see above");
    return g_all_passed ? 0 : 1;
}
