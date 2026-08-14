/* models_cache.c — provider availability probe. See models_cache.h. */
#define _POSIX_C_SOURCE 200809L
#include "models_cache.h"

#include "buf.h"
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

int models_cache_ingest(sqlite3 *db, const char *provider_name,
                        const char *body, char *err, size_t errlen) {
    if (!db || !provider_name || !body) return -1;

    /* Shape check first: a provider that answered with an HTML error page or a
     * bare array must leave the previous (still useful) cache alone. */
    sqlite3_stmt *s;
    int ok = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT json_valid(?1) AND json_type(?1,'$.data')='array'",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, body, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) ok = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (!ok) {
        set_err(err, errlen, "provider '%s' did not return a JSON {\"data\":[...]} catalog",
                provider_name);
        return -1;
    }

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
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
            " SELECT ?1, json_extract(value,'$.id'),"
            "        json_extract(value,'$.context_length'),"
            "        json_extract(value,'$.pricing.prompt'),"
            "        json_extract(value,'$.pricing.completion'),"
            "        json_extract(value,'$.architecture.modality'),"
            "        unixepoch()"
            "   FROM json_each(?2,'$.data')"
            "  WHERE json_extract(value,'$.id') IS NOT NULL",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, provider_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, body, -1, SQLITE_STATIC);
        rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
        if (rc != 0) set_err(err, errlen, "cache write failed: %s", sqlite3_errmsg(db));
        sqlite3_finalize(s);
    } else if (rc == 0) {
        rc = -1;
    }
    sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return rc;
}

/* Provider key, resolved the way the request path does: env var wins, then the
 * system secret store. A blank api_key_env is legal (local gateways). */
static char *provider_key(sqlite3 *db, const char *api_key_env) {
    if (!api_key_env || !api_key_env[0]) return NULL;
    const char *env = getenv(api_key_env);
    if (env && env[0]) return strdup(env);
    return db_secret_get_system(db, api_key_env);
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
    char *key_env = q_text(db, "SELECT api_key_env FROM providers WHERE name=?1",
                           provider_name);
    char *key = provider_key(db, key_env);

    size_t n = strlen(base_url);
    while (n > 0 && base_url[n-1] == '/') base_url[--n] = '\0';
    char url[1024], auth[512];
    snprintf(url, sizeof(url), "%s/models", base_url);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key ? key : "");

    const char *headers[3] = { "Accept: application/json", NULL, NULL };
    if (key) headers[1] = auth;

    HttpRequestOpts opts = {
        .url = url, .method = "GET", .headers = headers, .timeout = 30,
        .follow_redirects = 1,
        /* Hard cap: an oversized catalog is refused, not half-parsed. */
        .max_response_bytes = MODELS_CACHE_MAX_BYTES,
        .user_agent = "cclaw",
    };
    HttpResponse resp = {0};
    int status = http_do(&opts, &resp);

    int rc = -1;
    if (status < 200 || status > 299) {
        set_err(err, errlen, "GET %s failed (status %d)%s%s", url, status,
                resp.err_detail[0] ? ": " : "", resp.err_detail);
    } else if (resp.truncated) {
        set_err(err, errlen, "catalog from '%s' exceeds the %u MB cap — not cached",
                provider_name, MODELS_CACHE_MAX_BYTES / (1024u * 1024u));
    } else if (!resp.data) {
        set_err(err, errlen, "provider '%s' returned an empty body", provider_name);
    } else {
        rc = models_cache_ingest(db, provider_name, resp.data, err, errlen);
        if (rc == 0)
            LOG_INFO_("models_cache: refreshed %s (%zu bytes)", provider_name, resp.len);
    }

    http_response_free(&resp);
    free(base_url); free(key_env);
    if (key) { memset(key, 0, strlen(key)); free(key); }
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
            "SELECT provider_name, id, COALESCE(context_length,0),"
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
        buf_appendf(&b, "%s  ctx=%lld  prompt=$%s completion=$%s  %s\n",
                    sqlite3_column_text(s, 1),
                    (long long)sqlite3_column_int64(s, 2),
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
