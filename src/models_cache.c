/* models_cache.c — provider availability probe. See models_cache.h. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "models_cache.h"

#include "buf.h"
#include "config_registry.h"
#include "db.h"
#include "http.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODELS_CACHE_MAX_BYTES  (4u * 1024u * 1024u)

static void set_err(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* One-row, one-column text query bound as (?1). Result strdup'd, NULL if the
 * query matched nothing. */
static char *q_text(sqlite3 *db, const char *sql, const char *p1) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    if (p1) sqlite3_bind_text(s, 1, p1, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(s);
    return out;
}

/* BEGIN IMMEDIATE when the handle is idle, a savepoint inside a caller's
 * transaction (the dashboard's add-provider writes the key, the provider and
 * its catalog as one unit). */
static int write_begin(sqlite3 *db, int *own) {
    *own = sqlite3_get_autocommit(db);
    return sqlite3_exec(db, *own ? "BEGIN IMMEDIATE" : "SAVEPOINT models_cache",
                        NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static void write_end(sqlite3 *db, int own, int ok) {
    if (ok) sqlite3_exec(db, own ? "COMMIT" : "RELEASE models_cache", NULL, NULL, NULL);
    else if (own) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    else sqlite3_exec(db, "ROLLBACK TO models_cache; RELEASE models_cache", NULL, NULL, NULL);
}

/* The two catalog shapes providers send: {"data":[{"id",...}]} (OpenAI,
 * OpenRouter, Anthropic) and Gemini's {"models":[{"name":"models/x",
 * "inputTokenLimit":N}]}. Gemini states no modality, so none is recorded.
 * The context window is context_length on OpenRouter and max_input_tokens on
 * Anthropic (seen on the real endpoint, 2026-11-23). */
#define CATALOG_ROWS_SQL \
    " SELECT json_extract(value,'$.id') AS id," \
    "        COALESCE(json_extract(value,'$.context_length')," \
    "                 json_extract(value,'$.max_input_tokens')) AS ctx," \
    "        json_extract(value,'$.pricing.prompt') AS pp," \
    "        json_extract(value,'$.pricing.completion') AS cp," \
    "        json_extract(value,'$.architecture.modality') AS modality" \
    "   FROM json_each(?2,'$.data') WHERE json_type(?2,'$.data')='array'" \
    " UNION ALL" \
    " SELECT CASE WHEN json_extract(value,'$.name') LIKE 'models/%'" \
    "             THEN substr(json_extract(value,'$.name'), 8)" \
    "             ELSE json_extract(value,'$.name') END," \
    "        json_extract(value,'$.inputTokenLimit'), NULL, NULL, NULL" \
    "   FROM json_each(?2,'$.models') WHERE json_type(?2,'$.data') IS NULL"

int models_cache_ingest(sqlite3 *db, const char *provider_name,
                        const char *body, char *err, size_t errlen) {
    if (!db || !provider_name || !body) return -1;

    /* Shape check first: a provider that answered with an HTML error page or a
     * bare array must leave the previous (still useful) cache alone. */
    sqlite3_stmt *s;
    int ok = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT json_valid(?1) AND (json_type(?1,'$.data')='array'"
            " OR (json_type(?1,'$.models')='array' AND json_array_length(?1,'$.models') > 0"
            "     AND json_type(?1,'$.models[0].name')='text'))",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, body, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) ok = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (!ok) {
        set_err(err, errlen, "provider '%s' did not return a model catalog"
                " (a JSON {\"data\":[...]} or {\"models\":[...]} list)", provider_name);
        return -1;
    }

    int own;
    if (write_begin(db, &own) != 0) {
        set_err(err, errlen, "cache busy");
        return -1;
    }
    int rc = -1;
    if (sqlite3_prepare_v2(db, "DELETE FROM models_cache WHERE provider_name=?1",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, provider_name, -1, SQLITE_STATIC);
        rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(s);
    }
    /* One statement decomposes the catalog — json_each is the parser. Rows
     * without an id are skipped rather than failing the whole refresh. */
    if (rc == 0 && sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO models_cache(provider_name, id, context_length,"
            "  prompt_price, completion_price, modality, synced_at)"
            " SELECT ?1, id, ctx, pp, cp, modality, unixepoch() FROM ("
            CATALOG_ROWS_SQL ") WHERE id IS NOT NULL",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, provider_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, body, -1, SQLITE_STATIC);
        rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
        if (rc != 0) set_err(err, errlen, "cache write failed: %s", sqlite3_errmsg(db));
        sqlite3_finalize(s);
    } else if (rc == 0) {
        set_err(err, errlen, "cache write failed: %s", sqlite3_errmsg(db));
        rc = -1;
    }
    write_end(db, own, rc == 0);
    return rc;
}

/* Provider key, resolved the way the request path does: env var wins, then the
 * system secret store. A blank api_key_env is legal (local gateways). */
char *models_cache_key(sqlite3 *db, const char *api_key_env) {
    if (!api_key_env || !api_key_env[0]) return NULL;
    const char *env = getenv(api_key_env);
    if (env && env[0]) return strdup(env);
    return db_secret_get_system(db, api_key_env);
}

int models_cache_timeout(sqlite3 *db) {
    int t = (int)config_get_int(db, "llm_probe_timeout");
    return t > MODELS_CACHE_FETCH_MIN_S ? t : MODELS_CACHE_FETCH_MIN_S;
}

int models_cache_fetch(const char *base_url, const char *endpoint_type,
                       const char *key, int timeout_s, char **body_out,
                       char *err, size_t errlen) {
    *body_out = NULL;
    if (!base_url || !base_url[0]) {
        set_err(err, errlen, "no API endpoint");
        return -1;
    }
    size_t n = strlen(base_url);
    while (n > 0 && base_url[n - 1] == '/') n--;
    /* Each wire format lists models at <base>/models, authenticates the way
     * its requests do (llm_build_auth_header), and pages: ask for one big
     * page so the catalog arrives whole. */
    int gemini = endpoint_type && strcmp(endpoint_type, "gemini") == 0;
    int anthropic = endpoint_type && strcmp(endpoint_type, "anthropic") == 0;
    char url[1100], auth[600];
    snprintf(url, sizeof(url), "%.*s/models%s", (int)n, base_url,
             gemini ? "?pageSize=1000" : anthropic ? "?limit=1000" : "");
    snprintf(auth, sizeof(auth), gemini ? "x-goog-api-key: %s"
                               : anthropic ? "x-api-key: %s"
                               : "Authorization: Bearer %s", key ? key : "");
    const char *headers[4] = { "Accept: application/json", NULL, NULL, NULL };
    int h = 1;
    if (key && key[0]) headers[h++] = auth;
    if (anthropic) headers[h++] = "anthropic-version: 2023-06-01";

    HttpRequestOpts opts = {
        .url = url, .method = "GET", .headers = headers,
        .timeout = timeout_s > 0 ? timeout_s : MODELS_CACHE_FETCH_MIN_S,
        .follow_redirects = 1,
        /* Hard cap: an oversized catalog is refused, not half-parsed. */
        .max_response_bytes = MODELS_CACHE_MAX_BYTES,
        .user_agent = "cclaw",
    };
    HttpResponse resp = {0};
    int status = http_do(&opts, &resp);
    explicit_bzero(auth, sizeof(auth));

    int rc = -1;
    if (status < 200 || status > 299) {
        if (status == 401 || status == 403)
            set_err(err, errlen, "the provider refused the key (HTTP %d) at %s", status, url);
        else if (status <= 0)
            set_err(err, errlen, "could not reach %s%s%s", url,
                    resp.err_detail[0] ? ": " : "", resp.err_detail);
        else
            set_err(err, errlen, "GET %s failed (HTTP %d)", url, status);
    } else if (resp.truncated) {
        set_err(err, errlen, "the catalog exceeds the %u MB cap — not cached",
                MODELS_CACHE_MAX_BYTES / (1024u * 1024u));
    } else if (!resp.data) {
        set_err(err, errlen, "the provider returned an empty body");
    } else {
        *body_out = resp.data;
        resp.data = NULL;
        rc = 0;
    }
    http_response_free(&resp);
    return rc;
}

int models_cache_refresh(sqlite3 *db, const char *provider_name,
                         char *err, size_t errlen) {
    if (!db || !provider_name) return -1;

    char *base_url = q_text(db, "SELECT base_url FROM providers WHERE name=?1",
                            provider_name);
    if (!base_url) {
        set_err(err, errlen, "no provider named '%s'", provider_name);
        return -1;
    }
    char *type = q_text(db, "SELECT endpoint_type FROM providers WHERE name=?1",
                        provider_name);
    char *key_env = q_text(db, "SELECT api_key_env FROM providers WHERE name=?1",
                           provider_name);
    char *key = models_cache_key(db, key_env);
    /* A named key that is not there: say so, where asking the provider would
     * come back as "refused the key" for a key nobody supplied. */
    if (key_env && key_env[0] && !key) {
        set_err(err, errlen, "no API key for provider '%s': %s is not set in the"
                " environment and not saved as a secret", provider_name, key_env);
        free(base_url); free(type); free(key_env);
        return -1;
    }
    char *body = NULL;
    int rc = models_cache_fetch(base_url, type, key, models_cache_timeout(db),
                                &body, err, errlen);
    if (key) { explicit_bzero(key, strlen(key)); free(key); }
    if (rc == 0) {
        rc = models_cache_ingest(db, provider_name, body, err, errlen);
        if (rc == 0)
            LOG_INFO_("models_cache: refreshed %s (%zu bytes)", provider_name, strlen(body));
    }
    free(body); free(base_url); free(type); free(key_env);
    return rc;
}

/* Providers in scope, oldest cache first. Callers iterate to decide refreshes. */
static int refresh_stale(sqlite3 *db, const char *provider, int force,
                         Buf *notes, char *err, size_t errlen) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT p.name FROM providers p"
            " WHERE (?1 IS NULL OR p.name=?1)"
            "   AND (?2 = 1 OR COALESCE((SELECT MAX(synced_at) FROM models_cache c"
            "                             WHERE c.provider_name=p.name), 0)"
            "        < unixepoch() - ?3)"
            " ORDER BY p.priority, p.name", -1, &s, NULL) != SQLITE_OK)
        return -1;
    if (provider) sqlite3_bind_text(s, 1, provider, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 1);
    sqlite3_bind_int(s, 2, force ? 1 : 0);
    sqlite3_bind_int(s, 3, MODELS_CACHE_TTL_S);

    char **names = NULL;
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        char **grown = realloc(names, (size_t)(count + 1) * sizeof(*names));
        if (!grown) break;
        names = grown;
        names[count++] = strdup((const char *)sqlite3_column_text(s, 0));
    }
    sqlite3_finalize(s);

    /* Refresh outside the SELECT: each probe is a blocking HTTP call and must
     * not be made with a statement open on the same connection. */
    int rc = 0;
    for (int i = 0; i < count; i++) {
        char one[256] = "";
        if (names[i] && models_cache_refresh(db, names[i], one, sizeof(one)) != 0) {
            /* A named provider's failure is the answer; when probing every
             * provider, one dead endpoint must not hide the rest. */
            if (provider) { set_err(err, errlen, "%s", one); rc = -1; }
            else buf_appendf(notes, "(note: %s)\n", one);
        }
        free(names[i]);
    }
    free(names);
    return rc;
}

int models_cache_query(sqlite3 *db, const char *provider, const char *query,
                       int limit, int page, int force, char **out,
                       char *err, size_t errlen) {
    if (!db || !out) return -1;
    *out = NULL;
    if (query && !query[0]) query = NULL;
    if (limit <= 0) limit = 10;
    if (page <= 0) page = 1;
    long long offset = (long long)(page - 1) * limit;

    Buf b = {0};
    if (refresh_stale(db, provider, force, &b, err, errlen) != 0) {
        buf_free(&b);
        return -1;
    }

    /* Size rule: an aggregator's catalog is never listed whole, at any surface.
     * The refusal names the count so the caller knows why. */
    if (!query) {
        sqlite3_stmt *s;
        long long total = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM models_cache WHERE (?1 IS NULL OR provider_name=?1)",
                -1, &s, NULL) == SQLITE_OK) {
            if (provider) sqlite3_bind_text(s, 1, provider, -1, SQLITE_STATIC);
            else sqlite3_bind_null(s, 1);
            if (sqlite3_step(s) == SQLITE_ROW) total = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
        if (total > MODELS_CACHE_LIST_MAX) {
            set_err(err, errlen, "catalog has %lld models; supply a search term",
                    total);
            buf_free(&b);
            return -1;
        }
    }

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT provider_name, id, context_length,"
            "       COALESCE(prompt_price,'?'), COALESCE(completion_price,'?'),"
            "       COALESCE(modality,''),"
            "       COUNT(*) OVER () AS total"
            "  FROM models_cache"
            " WHERE (?1 IS NULL OR provider_name=?1)"
            "   AND (?2 IS NULL OR id LIKE '%'||?2||'%')"
            " ORDER BY provider_name, id LIMIT ?3 OFFSET ?4", -1, &s, NULL) != SQLITE_OK) {
        set_err(err, errlen, "cache read failed: %s", sqlite3_errmsg(db));
        buf_free(&b);
        return -1;
    }
    if (provider) sqlite3_bind_text(s, 1, provider, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 1);
    if (query) sqlite3_bind_text(s, 2, query, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 2);
    sqlite3_bind_int(s, 3, limit);
    sqlite3_bind_int64(s, 4, offset);

    long long total = 0, shown = 0;
    char last_provider[128] = "";
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *pname = (const char *)sqlite3_column_text(s, 0);
        total = sqlite3_column_int64(s, 6);
        if (strcmp(last_provider, pname) != 0) {
            snprintf(last_provider, sizeof(last_provider), "%s", pname);
            buf_appendf(&b, "\n[%s]\n", pname);
        }
        /* A provider that does not state a window has none recorded: "?",
         * not 0. */
        char ctx[24] = "?";
        if (sqlite3_column_type(s, 2) != SQLITE_NULL)
            snprintf(ctx, sizeof(ctx), "%lld", (long long)sqlite3_column_int64(s, 2));
        buf_appendf(&b, "%s  ctx=%s  prompt=$%s completion=$%s  %s\n",
                    sqlite3_column_text(s, 1), ctx,
                    sqlite3_column_text(s, 3), sqlite3_column_text(s, 4),
                    sqlite3_column_text(s, 5));
        shown++;
    }
    sqlite3_finalize(s);

    /* Paging is plain OFFSET: a refresh between pages can shift rows. Accepted
     * (Mark) — a catalog listing is a lookup aid, not a cursor over live data. */
    if (shown == 0)
        buf_appendf(&b, offset > 0 ? "(no models listed on page %d)\n"
                                   : "(no model matches)\n", page);
    else if (total > offset + shown)
        buf_appendf(&b, "(%lld more — page=%d or narrow your query)\n",
                    total - offset - shown, page + 1);

    *out = buf_take(&b);
    if (!*out) { set_err(err, errlen, "out of memory"); return -1; }
    return 0;
}
