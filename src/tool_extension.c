#define _POSIX_C_SOURCE 200809L
#include "tool_extension.h"
#include "extension_manifest.h"
#include "tool_parse.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A safe extension name: non-empty, no path separators, no parent refs. The
 * draft directory <workspace>/extensions/<name> is derived from it, so a bad
 * name must never escape the workspace. */
static int valid_name(const char *n) {
    if (!n || !n[0]) return 0;
    if (strchr(n, '/') || strchr(n, '\\') || strstr(n, "..")) return 0;
    return 1;
}

static char *msgf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* ── extension_promote ──────────────────────────────────────────────── */

static char *tool_extension_promote_handler(const char *arguments, void *user_data) {
    ToolExtensionCtx *ctx = (ToolExtensionCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: extension_promote unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON arguments");
    const char *name = targ_str(&ta, "name");
    if (!valid_name(name)) {
        tool_parse_free(&ta);
        return strdup("error: 'name' is required (no path separators)");
    }

    const char *ws = (ctx->workspace && ctx->workspace[0]) ? ctx->workspace : ".";
    char bundle[PATH_MAX];
    snprintf(bundle, sizeof(bundle), "%s/extensions/%s", ws, name);
    char namebuf[256];
    snprintf(namebuf, sizeof(namebuf), "%s", name);
    tool_parse_free(&ta);

    char *err = NULL;
    if (extension_install(ctx->db, bundle, ctx->agent_name, &err) != 0) {
        char *m = msgf("error: promote failed: %s", err ? err : "unknown");
        free(err);
        return m;
    }
    free(err);
    return msgf("promoted extension '%s' into the shared store; its tools are now "
                "registered and owned by '%s'. Re-promote to pick up draft edits.",
                namebuf, ctx->agent_name);
}

/* ── extension_publish ──────────────────────────────────────────────── */

static char *tool_extension_publish_handler(const char *arguments, void *user_data) {
    ToolExtensionCtx *ctx = (ToolExtensionCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: extension_publish unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON arguments");
    const char *name = targ_str(&ta, "name");
    if (!valid_name(name)) {
        tool_parse_free(&ta);
        return strdup("error: 'name' is required");
    }
    char namebuf[256];
    snprintf(namebuf, sizeof(namebuf), "%s", name);
    tool_parse_free(&ta);

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            "UPDATE extensions SET published=1 WHERE name=?1 AND owner_agent=?2",
            -1, &st, NULL) != SQLITE_OK)
        return strdup("error: publish failed (db)");
    sqlite3_bind_text(st, 1, namebuf, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, ctx->agent_name, -1, SQLITE_STATIC);
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) return strdup("error: publish failed (db)");
    if (sqlite3_changes(ctx->db) == 0)
        return msgf("error: extension '%s' not found or not owned by you", namebuf);
    return msgf("published extension '%s'; other agents can now attach it.", namebuf);
}

/* ── extension_attach ───────────────────────────────────────────────── */

static char *tool_extension_attach_handler(const char *arguments, void *user_data) {
    ToolExtensionCtx *ctx = (ToolExtensionCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: extension_attach unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON arguments");
    const char *name = targ_str(&ta, "name");
    if (!valid_name(name)) {
        tool_parse_free(&ta);
        return strdup("error: 'name' is required");
    }
    char namebuf[256];
    snprintf(namebuf, sizeof(namebuf), "%s", name);
    tool_parse_free(&ta);

    /* Trust boundary: attach only a published extension, or one you own. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT 1 FROM extensions WHERE name=?1 AND (published=1 OR owner_agent=?2)",
            -1, &st, NULL) != SQLITE_OK)
        return strdup("error: attach failed (db)");
    sqlite3_bind_text(st, 1, namebuf, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, ctx->agent_name, -1, SQLITE_STATIC);
    int visible = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (!visible)
        return msgf("error: extension '%s' is not published and not owned by you", namebuf);

    if (sqlite3_prepare_v2(ctx->db,
            "INSERT INTO agent_extensions(agent_name, extension_name, enabled) "
            "VALUES(?1, ?2, 1) "
            "ON CONFLICT(agent_name, extension_name) DO UPDATE SET enabled=1",
            -1, &st, NULL) != SQLITE_OK)
        return strdup("error: attach failed (db)");
    sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, namebuf, -1, SQLITE_STATIC);
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) return strdup("error: attach failed (db)");
    return msgf("attached extension '%s' to agent '%s'; its tools load on the next turn.",
                namebuf, ctx->agent_name);
}

/* ── extension_list ─────────────────────────────────────────────────── */

static char *tool_extension_list_handler(const char *arguments, void *user_data) {
    (void)arguments;
    ToolExtensionCtx *ctx = (ToolExtensionCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: extension_list unavailable");

    /* Extensions visible to this agent (owned or published), each annotated
     * with whether the agent currently has it attached. Built with JSON1. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT json_group_array(json_object("
            "  'name', e.name, 'owner', e.owner_agent, 'published', e.published, "
            "  'attached', EXISTS(SELECT 1 FROM agent_extensions ae "
            "      WHERE ae.extension_name=e.name AND ae.agent_name=?1 AND ae.enabled=1))) "
            "FROM extensions e "
            "WHERE e.builtin=0 AND (e.published=1 OR e.owner_agent=?1)",
            -1, &st, NULL) != SQLITE_OK)
        return strdup("error: list failed (db)");
    sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        out = strdup(v ? v : "[]");
    }
    sqlite3_finalize(st);
    return out ? out : strdup("[]");
}

/* ── registration ───────────────────────────────────────────────────── */

static const char *NAME_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Extension name\"}"
    "},\"required\":[\"name\"]}";

static const char *EMPTY_PARAMS =
    "{\"type\":\"object\",\"properties\":{}}";

int tool_extension_register(ToolRegistry *reg, ToolExtensionCtx *ctx) {
    int rc = 0;
    rc |= tools_register(reg, "extension_promote",
        "Promote a draft extension from your workspace (workspace/extensions/<name>) "
        "into the shared store. Validates the manifest, copies the bundle, and "
        "registers its tools/hooks as a real extension owned by you.",
        NAME_PARAMS, tool_extension_promote_handler, ctx);
    rc |= tools_register(reg, "extension_publish",
        "Publish an extension you own so other agents can attach it. Sets the "
        "single publish flag; you must be the owner.",
        NAME_PARAMS, tool_extension_publish_handler, ctx);
    rc |= tools_register(reg, "extension_attach",
        "Attach a visible extension (published, or owned by you) to yourself so "
        "its tools become available on your next turn.",
        NAME_PARAMS, tool_extension_attach_handler, ctx);
    rc |= tools_register(reg, "extension_list",
        "List extensions visible to you (owned or published), with owner, publish "
        "state, and whether you have each attached. Returns a JSON array.",
        EMPTY_PARAMS, tool_extension_list_handler, ctx);
    return rc;
}
