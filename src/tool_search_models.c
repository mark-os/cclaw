/* search_models — search a provider's model catalog. See the header for why
 * this is a separate, granted-only tool rather than a search_config topic. */
#define _POSIX_C_SOURCE 200809L
#include "tool_search_models.h"
#include "tool_args.h"
#include "buf.h"
#include "models_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEARCH_MODELS_PAGE 10

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"Substring matched against the model id."
    " Required when the catalog holds more than 50 models\"},"
    "\"provider\":{\"type\":\"string\",\"description\":\"Limit to one configured provider by name\"},"
    "\"page\":{\"type\":\"integer\",\"description\":\"1-based page, 10 results per page (default 1)\"}"
    "}}";

static char *handler(const char *arguments, void *user_data, int *is_error) {
    SearchModelsCtx *ctx = (SearchModelsCtx *)user_data;
    if (!ctx || !ctx->db)
        return tool_fail(is_error, "error: search_models unavailable");

    char *query = tool_args_str(ctx->db, arguments, "query");
    char *provider = tool_args_str(ctx->db, arguments, "provider");
    int page = tool_args_int(ctx->db, arguments, "page", 1);
    if (query && !query[0]) { free(query); query = NULL; }
    if (provider && !provider[0]) { free(provider); provider = NULL; }

    char err[256] = "", *listing = NULL;
    int rc = models_cache_query(ctx->db, provider, query, SEARCH_MODELS_PAGE,
                                page, 0, &listing, err, sizeof(err));
    free(query); free(provider);
    if (rc != 0)
        return tool_fail(is_error, "error: %s", err[0] ? err : "catalog search failed");

    Buf out = {0};
    buf_appendf(&out, "## Models listed by the provider catalog\n%s", listing);
    buf_appendf(&out,
        "\nThese are LISTED models — what the provider's catalog advertises. A"
        " listing is not a guarantee that the id works for this account, and it"
        " is not a REGISTERED model: only registered ids are routable, and"
        " adding or switching one requires a request_config approval.\n");
    free(listing);
    char *result = buf_take(&out);
    return result ? result : tool_fail(is_error, "error: out of memory");
}

/* EXEC_THREAD shim: rebuild the ctx around the thread's own db. The handler
 * may block on an HTTP call (stale cache), which is exactly why it must not
 * run on the poll thread. */
static char *search_models_thread_run(sqlite3 *db, const char *agent_name,
                                      int64_t session_id, const char *args,
                                      int *is_error) {
    (void)agent_name; (void)session_id;
    SearchModelsCtx c = {.db = db};
    return handler(args, &c, is_error);
}

int tool_search_models_register(ToolRegistry *reg, SearchModelsCtx *ctx) {
    int rc = tools_register(reg, "search_models",
        "Search the model catalogs published by the configured providers. Optional 'query' "
        "(substring of the model id — required for catalogs over 50 models), 'provider', and "
        "'page' (10 per page). May make a network request to the provider when the cached copy "
        "is older than 12h. Results are the provider's own listing, not a guarantee that a "
        "model is usable here — registering or switching a model still goes through "
        "request_config.",
        PARAMS_JSON, handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "search_models",
                         (ToolRecipe){EXEC_THREAD, SBX_NONE, search_models_thread_run});
    return rc;
}
