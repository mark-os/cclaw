/* Benchmark: measure caching, latency, throughput, and cost across models.
 * Builds sessions with growing context, measures cache hits per turn.
 * Uses cclaw's LLM + HTTP functions directly with a temp DB.
 *
 * Usage: ./build/test_e2e_bench_providers
 * Requires: OPENROUTER_API_KEY
 *
 * Default models (override with BENCH_MODELS="model1,model2,..."):
 *   ibm-granite/granite-4.1-8b
 *   deepseek/deepseek-v4-flash
 *   openai/gpt-5-nano
 *   google/gemini-2.5-flash-lite
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "llm.h"
#include "http.h"
#include "db.h"
#include <sqlite3.h>
#include "test_util.h"

static sqlite3 *g_bench_db;
#define MAX_MODELS 10
#define TURNS 5
#define BASE_URL "https://openrouter.ai/api/v1"

static const char *DEFAULT_MODELS[] = {
    "ibm-granite/granite-4.1-8b",
    "deepseek/deepseek-v4-flash",
    "openai/gpt-5-nano",
    "google/gemini-2.5-flash-lite",
};
static const int DEFAULT_MODEL_COUNT = 4;

typedef struct {
    int prompt_tokens;
    int completion_tokens;
    int cache_read;
    int cache_write;
    int64_t cost_nano;
    long latency_ms;
} TurnResult;

static long elapsed_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000 +
           (end->tv_nsec - start->tv_nsec) / 1000000;
}

/* Send a non-streaming request and parse response */
static int do_turn(const char *api_key, const char *model,
                   const char *session_id, Message *msgs, int msg_count,
                   TurnResult *out) {
    /* Build messages JSON via SQLite */
    sqlite3_exec(g_bench_db, "DROP TABLE IF EXISTS _bmsg;", NULL, NULL, NULL);
    sqlite3_exec(g_bench_db,
        "CREATE TEMP TABLE _bmsg(pos INTEGER PRIMARY KEY, role TEXT, content TEXT);",
        NULL, NULL, NULL);
    sqlite3_stmt *ins;
    sqlite3_prepare_v2(g_bench_db, "INSERT INTO _bmsg VALUES(?,?,?)", -1, &ins, NULL);
    for (int i = 0; i < msg_count; i++) {
        const char *r = msgs[i].role == ROLE_SYSTEM ? "system" :
                        msgs[i].role == ROLE_USER ? "user" : "assistant";
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, r, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, msgs[i].content ? msgs[i].content : "", -1, SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    sqlite3_stmt *bld;
    sqlite3_prepare_v2(g_bench_db,
        "SELECT json_object('model',?1,'max_tokens',2048,"
        " 'messages',(SELECT json_group_array("
        "   json_object('role',role,'content',content)) FROM _bmsg ORDER BY pos))",
        -1, &bld, NULL);
    sqlite3_bind_text(bld, 1, model, -1, SQLITE_STATIC);

    char *body = NULL;
    if (sqlite3_step(bld) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(bld, 0);
        if (t) body = strdup(t);
    }
    sqlite3_finalize(bld);
    if (!body) return -1;

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", api_key);
    char session_hdr[128];
    snprintf(session_hdr, sizeof(session_hdr), "x-session-id: %s", session_id);
    const char *headers[] = {
        "Content-Type: application/json", auth_hdr, session_hdr, NULL
    };

    char url[256];
    snprintf(url, sizeof(url), "%s/chat/completions", BASE_URL);

    HttpResponse resp = {0};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int status = http_post(url, headers, body, &resp);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(body);

    out->latency_ms = elapsed_ms(&t0, &t1);

    if (status != 200) {
        fprintf(stderr, "    HTTP %d: %.100s\n", status, resp.data ? resp.data : "");
        http_response_free(&resp);
        return -1;
    }

    /* Extract usage scalars straight from the body (incl. cache details). */
    sqlite3_stmt *us;
    if (sqlite3_prepare_v2(g_bench_db,
            "SELECT json_extract(?1,'$.usage.prompt_tokens'),"
            " json_extract(?1,'$.usage.completion_tokens'),"
            " COALESCE(json_extract(?1,'$.usage.prompt_tokens_details.cached_tokens'),"
            "          json_extract(?1,'$.usage.prompt_cache_hit_tokens')),"
            " json_extract(?1,'$.usage.prompt_tokens_details.cache_write_tokens'),"
            " COALESCE(json_extract(?1,'$.usage.cost'), json_extract(?1,'$.usage.total_cost'))",
            -1, &us, NULL) != SQLITE_OK) {
        http_response_free(&resp);
        return -1;
    }
    sqlite3_bind_text(us, 1, resp.data, -1, SQLITE_STATIC);
    if (sqlite3_step(us) == SQLITE_ROW) {
        out->prompt_tokens = sqlite3_column_int(us, 0);
        out->completion_tokens = sqlite3_column_int(us, 1);
        out->cache_read = sqlite3_column_int(us, 2);
        out->cache_write = sqlite3_column_int(us, 3);
        if (sqlite3_column_type(us, 4) != SQLITE_NULL)
            out->cost_nano = (int64_t)(sqlite3_column_double(us, 4) * 1e9 + 0.5);
    }
    sqlite3_finalize(us);

    http_response_free(&resp);
    return 0;
}

int main(void) {
    TEST_INIT();
    sqlite3_open(":memory:", &g_bench_db);
    const char *api_key = getenv("OPENROUTER_API_KEY");
    if (!api_key) {
        fprintf(stderr, "Set OPENROUTER_API_KEY\n");
        return 1;
    }

    /* Parse model list */
    const char *models[MAX_MODELS];
    int model_count = 0;
    const char *env_models = getenv("BENCH_MODELS");
    if (env_models) {
        char *buf = strdup(env_models);
        char *tok = strtok(buf, ",");
        while (tok && model_count < MAX_MODELS) {
            models[model_count++] = strdup(tok);
            tok = strtok(NULL, ",");
        }
        free(buf);
    } else {
        model_count = DEFAULT_MODEL_COUNT;
        for (int i = 0; i < model_count; i++)
            models[i] = DEFAULT_MODELS[i];
    }

    printf("Provider Benchmark (%d turns per model, non-streaming)\n", TURNS);
    printf("═══════════════════════════════════════════════════════════════════════════════════\n\n");

    /* System message — short, no padding needed. Context grows from responses. */
    const char *system_msg = "You are a concise technical assistant.";

    /* User prompts: turn 1 asks for a big response to build context, rest are short */
    const char *prompts[TURNS] = {
        "Write a comprehensive 1000-word essay on the history of Unix process management. "
        "Cover Research Unix V1 fork(), BSD job control, System V IPC, Linux clone() in 2.0, "
        "the O(1) scheduler, CFS in 2.6.23, namespaces from 2.4.19 through 3.8, "
        "cgroups v1 in 2.6.24 and v2 in 4.5, seccomp-bpf in 3.5, and how these compose "
        "into Docker containers. Include kernel version numbers for each development.",
        "Reply with only: turn-2",
        "Reply with only: turn-3",
        "Reply with only: turn-4",
        "Reply with only: turn-5",
    };

    /* Track results for verification */
    TurnResult all_results[MAX_MODELS][TURNS];
    int model_ok[MAX_MODELS]; /* 1 = all turns succeeded */
    memset(all_results, 0, sizeof(all_results));
    memset(model_ok, 0, sizeof(model_ok));

    for (int m = 0; m < model_count; m++) {
        printf("─── %s ───\n", models[m]);
        printf("  %-6s %7s %7s %7s %7s %10s %8s\n",
               "Turn", "Prompt", "Compl", "Cached", "Hit%", "Cost", "Latency");

        Message msgs[2 + TURNS * 2];
        int msg_count = 0;
        msgs[msg_count++] = (Message){.role = ROLE_SYSTEM, .content = (char *)system_msg};

        char session_id[64];
        snprintf(session_id, sizeof(session_id), "bench-%d-%d", (int)getpid(), m);

        int64_t total_cost = 0;
        long total_latency = 0;
        int total_cached = 0, total_prompt = 0;
        int failed = 0;

        for (int t = 0; t < TURNS; t++) {
            msgs[msg_count++] = (Message){.role = ROLE_USER, .content = (char *)prompts[t]};

            TurnResult r = {0};
            if (do_turn(api_key, models[m], session_id, msgs, msg_count, &r) != 0) {
                printf("  %-6d  FAILED\n", t + 1);
                msgs[msg_count++] = (Message){.role = ROLE_ASSISTANT, .content = "(error)"};
                failed = 1;
                continue;
            }

            all_results[m][t] = r;

            int hit_pct = r.prompt_tokens > 0 ? (r.cache_read * 100 / r.prompt_tokens) : 0;
            printf("  %-6d %7d %7d %7d %6d%% $%9.6f %6ldms\n",
                   t + 1, r.prompt_tokens, r.completion_tokens,
                   r.cache_read, hit_pct,
                   (double)r.cost_nano / 1e9, r.latency_ms);

            total_cost += r.cost_nano;
            total_latency += r.latency_ms;
            total_cached += r.cache_read;
            total_prompt += r.prompt_tokens;

            int len = r.completion_tokens * 4;
            if (len < 100) len = 100;
            if (len > 8000) len = 8000;
            char *asst_text = malloc((size_t)len + 1);
            memset(asst_text, 'A', (size_t)len);
            asst_text[len] = '\0';
            msgs[msg_count++] = (Message){.role = ROLE_ASSISTANT, .content = asst_text};
        }

        model_ok[m] = !failed;
        int avg_cache = total_prompt > 0 ? (total_cached * 100 / total_prompt) : 0;
        printf("  ──────────────────────────────────────────────────────────\n");
        printf("  Total: $%.6f  avg_lat=%ldms  cache_hit=%d%%\n\n",
               (double)total_cost / 1e9,
               total_latency / TURNS,
               avg_cache);
    }

    /* ── Verify caching ── */
    printf("═══════════════════════════════════════════════════════════════════════════════════\n");
    printf("Cache Verification\n\n");
    int any_cached = 0;
    int any_cost_decreased = 0;

    for (int m = 0; m < model_count; m++) {
        if (!model_ok[m]) {
            printf("  [SKIP] %s — had failed turns\n", models[m]);
            continue;
        }

        /* Check: cached_tokens > 0 on any turn 3+ (index 2+) */
        int model_cached = 0;
        for (int t = 2; t < TURNS; t++) {
            if (all_results[m][t].cache_read > 0) {
                model_cached = 1;
                break;
            }
        }

        /* Check: cost at turn 4 < cost at turn 2 (cache reduces effective cost) */
        int64_t cost_t2 = all_results[m][1].cost_nano; /* turn 2 = index 1 */
        int64_t cost_t4 = all_results[m][3].cost_nano; /* turn 4 = index 3 */
        int cost_decreased = (cost_t2 > 0 && cost_t4 < cost_t2);

        if (model_cached && cost_decreased) {
            printf("  [PASS] %s: cached_tokens>0 on turn 3+, cost t2=$%.6f > t4=$%.6f\n",
                   models[m], (double)cost_t2 / 1e9, (double)cost_t4 / 1e9);
            any_cached = 1;
            any_cost_decreased = 1;
        } else if (model_cached) {
            printf("  [WARN] %s: caching active but cost t4=$%.6f >= t2=$%.6f (context growth)\n",
                   models[m], (double)cost_t4 / 1e9, (double)cost_t2 / 1e9);
            any_cached = 1;
        } else {
            printf("  [WARN] %s: no cache hits reported (provider may not support)\n", models[m]);
        }
    }

    if (!any_cached) {
        printf("\n  [FAIL] No model reported cached_tokens > 0 — cache_control not working\n");
        return 1;
    }
    printf("\n  Cache verification passed: %s%s\n",
           any_cached ? "caching confirmed" : "",
           any_cost_decreased ? ", cost decrease observed" : "");
    sqlite3_close(g_bench_db);
    return 0;
}
