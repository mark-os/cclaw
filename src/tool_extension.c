#define _POSIX_C_SOURCE 200809L
#include "tool_extension.h"
#include "approval.h"
#include "db.h"
#include "extension_manifest.h"
#include "tool_args.h"
#include "util.h"
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
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

static char *sql_text(sqlite3 *db, const char *sql, const char *a1,
                      const char *a2, const char *a3);

/* ── extension_promote ──────────────────────────────────────────────── */

/* Enumerate a validated bundle's declared contents for the approval prompt:
 * "adds N tools, M hooks, ..." plus agent names/profiles. Heap; caller frees. */
char *extension_manifest_enumerate(sqlite3 *db, const char *bundle_dir) {
    char mpath[2*PATH_MAX];
    snprintf(mpath, sizeof(mpath), "%s/extension.json", bundle_dir);
    char *manifest = util_read_file(mpath, NULL);
    if (!manifest) return strdup("(unreadable manifest)");
    char *out = sql_text(db,
        "SELECT 'adds '"
        " || json_array_length(COALESCE(json_extract(?1,'$.tools'),'[]')) || ' tools, '"
        " || json_array_length(COALESCE(json_extract(?1,'$.hooks'),'[]')) || ' hooks, '"
        " || json_array_length(COALESCE(json_extract(?1,'$.scripts'),'[]')) || ' scripts, '"
        " || json_array_length(COALESCE(json_extract(?1,'$.skills'),'[]')) || ' skills, '"
        " || json_array_length(COALESCE(json_extract(?1,'$.config'),'[]')) || ' config keys, '"
        " || json_array_length(COALESCE(json_extract(?1,'$.agents'),'[]')) || ' agents'"
        " || COALESCE((SELECT ' (' || group_concat("
        "      json_extract(value,'$.name') || ':' ||"
        "      COALESCE(json_extract(value,'$.sandbox_profile'),'standard'), ', ') || ')'"
        "    FROM json_each(json_extract(?1,'$.agents'))), '')"
        " || CASE WHEN json_extract(?1,'$.channel') IS NOT NULL"
        "         THEN ', 1 channel' ELSE '' END",
        manifest, NULL, NULL);
    free(manifest);
    return out ? out : strdup("(enumeration failed)");
}

static char *tool_extension_promote_handler(const char *arguments, void *user_data) {
    ToolExtensionCtx *ctx = (ToolExtensionCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: extension_promote unavailable");

    char *name = tool_args_str(ctx->db, arguments, "name");
    if (!valid_name(name)) {
        free(name);
        return strdup("error: 'name' is required (no path separators)");
    }

    const char *ws = (ctx->workspace && ctx->workspace[0]) ? ctx->workspace : ".";
    char bundle[PATH_MAX], namebuf[256];
    snprintf(bundle, sizeof(bundle), "%s/extensions/%s", ws, name);
    snprintf(namebuf, sizeof(namebuf), "%s", name);
    free(name);

    /* Eager validation: a bundle that can't install never parks. */
    char *err = NULL;
    if (extension_manifest_validate(bundle, &err) != 0) {
        char *m = msgf("error: promote failed: %s", err ? err : "unknown");
        free(err);
        return m;
    }
    free(err);

    /* Park an apply-approval carrying the bundle path + enumerated contents
     * so the approver sees what registers before any code does. */
    char *summary = extension_manifest_enumerate(ctx->db, bundle);
    char *args = sql_text(ctx->db,
        "SELECT json_object('name',?1,'bundle',?2,'summary',?3)",
        namebuf, bundle, summary);
    free(summary);
    if (!args) return strdup("error: failed to build approval args");

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "extension_promote", "extension_promote",
        args, "apply");
    free(args);
    if (aid < 0) return strdup("error: failed to create approval");
    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

/* ── extension_publish ──────────────────────────────────────────────── */

static char *tool_extension_publish_handler(const char *arguments, void *user_data) {
    ToolExtensionCtx *ctx = (ToolExtensionCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: extension_publish unavailable");

    char *name = tool_args_str(ctx->db, arguments, "name");
    if (!valid_name(name)) {
        free(name);
        return strdup("error: 'name' is required");
    }

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            "UPDATE extensions SET published=1 WHERE name=?1 AND owner_agent=?2",
            -1, &st, NULL) != SQLITE_OK) {
        free(name);
        return strdup("error: publish failed (db)");
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, ctx->agent_name, -1, SQLITE_STATIC);
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) { free(name); return strdup("error: publish failed (db)"); }
    if (sqlite3_changes(ctx->db) == 0) {
        char *m = msgf("error: extension '%s' not found or not owned by you", name);
        free(name);
        return m;
    }
    char *m = msgf("published extension '%s'; other agents can now attach it.", name);
    free(name);
    return m;
}

/* ── extension_attach ───────────────────────────────────────────────── */

static char *tool_extension_attach_handler(const char *arguments, void *user_data) {
    ToolExtensionCtx *ctx = (ToolExtensionCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: extension_attach unavailable");

    char *name = tool_args_str(ctx->db, arguments, "name");
    if (!valid_name(name)) {
        free(name);
        return strdup("error: 'name' is required");
    }

    /* Trust boundary: attach only a published extension, or one you own. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT 1 FROM extensions WHERE name=?1 AND (published=1 OR owner_agent=?2)",
            -1, &st, NULL) != SQLITE_OK) {
        free(name);
        return strdup("error: attach failed (db)");
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, ctx->agent_name, -1, SQLITE_STATIC);
    int visible = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (!visible) {
        char *m = msgf("error: extension '%s' is not published and not owned by you", name);
        free(name);
        return m;
    }

    if (sqlite3_prepare_v2(ctx->db,
            "INSERT INTO agent_extensions(agent_name, extension_name, enabled) "
            "VALUES(?1, ?2, 1) "
            "ON CONFLICT(agent_name, extension_name) DO UPDATE SET enabled=1",
            -1, &st, NULL) != SQLITE_OK) {
        free(name);
        return strdup("error: attach failed (db)");
    }
    sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) { free(name); return strdup("error: attach failed (db)"); }
    char *m = msgf("attached extension '%s' to agent '%s'; its tools are callable "
                "once granted (attach does not grant — use request_config).",
                name, ctx->agent_name);
    free(name);
    return m;
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
            "WHERE e.published=1 OR e.owner_agent=?1",
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

/* ── extension_fork ─────────────────────────────────────────────────── */

/* Read <bundle>/extension.json if present, else NULL. */
static char *read_manifest(const char *bundle) {
    char mpath[2*PATH_MAX];
    snprintf(mpath, sizeof(mpath), "%s/extension.json", bundle);
    size_t len = 0;
    return util_read_file(mpath, &len);
}

/* One-row/one-text-column SQL helper (JSON1 as the JSON builder). */
static char *sql_text(sqlite3 *db, const char *sql, const char *a1,
                      const char *a2, const char *a3) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    if (a1) sqlite3_bind_text(s, 1, a1, -1, SQLITE_STATIC);
    if (a2) sqlite3_bind_text(s, 2, a2, -1, SQLITE_STATIC);
    if (a3) sqlite3_bind_text(s, 3, a3, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(s);
    return out;
}

/* Copy a registered extension's bundle into the agent's workspace as a draft
 * (workspace/extensions/<as>), so system-owned extensions like telegram can
 * be developed without touching the shared store — that stays the pristine
 * copy. The manifest name is rewritten to the fork's name, so a later
 * extension_promote registers the fork as its own extension instead of
 * colliding with the original. */
static char *tool_extension_fork_handler(const char *arguments, void *user_data) {
    ToolExtensionCtx *ctx = (ToolExtensionCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: extension_fork unavailable");

    char *name = tool_args_str(ctx->db, arguments, "name");
    char *as = tool_args_str(ctx->db, arguments, "as");
    char asbuf[128];
    if (!as || !as[0]) {
        free(as);
        snprintf(asbuf, sizeof(asbuf), "%s-fork", name ? name : "");
        as = strdup(asbuf);
    }
    if (!valid_name(name) || !valid_name(as)) {
        free(name); free(as);
        return strdup("error: 'name' (and optional 'as') must be plain names, no path separators");
    }

    /* Resolve + visibility: published, or owned by this agent. */
    sqlite3_stmt *s;
    char src[PATH_MAX] = "";
    int visible = 0;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT path, published, COALESCE(owner_agent,'')"
            " FROM extensions WHERE name=?;", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *p = (const char *)sqlite3_column_text(s, 0);
            if (p) snprintf(src, sizeof(src), "%s", p);
            int published = sqlite3_column_int(s, 1);
            const char *owner = (const char *)sqlite3_column_text(s, 2);
            visible = published ||
                      (owner && ctx->agent_name && strcmp(owner, ctx->agent_name) == 0);
        }
        sqlite3_finalize(s);
    }
    if (!src[0]) {
        char *m = msgf("error: extension '%s' is not registered", name);
        free(name); free(as);
        return m;
    }
    if (!visible) {
        char *m = msgf("error: extension '%s' is not visible to you "
                       "(not published or yours)", name);
        free(name); free(as);
        return m;
    }

    const char *ws = (ctx->workspace && ctx->workspace[0]) ? ctx->workspace : ".";
    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/extensions/%s", ws, as);
    struct stat st;
    if (stat(dest, &st) == 0) {
        char *m = msgf("error: draft '%s' already exists at extensions/%s", as, as);
        free(name); free(as);
        return m;
    }
    if (util_mkdir_p(dest) != 0) {
        char *m = msgf("error: cannot create %s", dest);
        free(name); free(as);
        return m;
    }

    /* Flat copy of regular files (bundles are flat: handler .qjs + json). */
    DIR *d = opendir(src);
    if (!d) {
        char *m = msgf("error: cannot read source bundle %s", src);
        free(name); free(as);
        return m;
    }
    int copied = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char sp[2*PATH_MAX], dp[2*PATH_MAX];
        snprintf(sp, sizeof(sp), "%s/%s", src, de->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dest, de->d_name);
        if (stat(sp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (util_copy_file(sp, dp, 0644) == 0) copied++;
    }
    closedir(d);
    if (copied == 0) {
        char *m = msgf("error: source bundle %s had no files", src);
        free(name); free(as);
        return m;
    }

    /* Manifest: rewrite the name so a later promote registers the fork as
     * its own extension. Every registered extension has a real manifest
     * (extension_install requires one), so a missing one is an error. */
    char *manifest = read_manifest(dest);
    if (!manifest) {
        char *m = msgf("error: source bundle %s has no extension.json", src);
        free(name); free(as);
        return m;
    }
    char *rewritten = sql_text(ctx->db, "SELECT json_set(?1,'$.name',?2);",
                               manifest, as, NULL);
    free(manifest);
    if (rewritten) {
        char mpath[2*PATH_MAX];
        snprintf(mpath, sizeof(mpath), "%s/extension.json", dest);
        FILE *f = fopen(mpath, "w");
        if (f) { fputs(rewritten, f); fclose(f); }
        free(rewritten);
    }

    char *m = msgf("forked extension '%s' into workspace draft extensions/%s "
                "(%d files). Edit it there; test a channel fork offline with "
                "`cclaw --channel <name> --harness <scenario.json>`; then "
                "extension_promote name='%s' registers it under your ownership.",
                name, as, copied, as);
    free(name);
    free(as);
    return m;
}

/* ── registration ───────────────────────────────────────────────────── */

static const char *NAME_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Extension name\"}"
    "},\"required\":[\"name\"]}";

static const char *FORK_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Registered extension to fork (published or yours)\"},"
    "\"as\":{\"type\":\"string\",\"description\":\"Draft name for the fork (default <name>-fork)\"}"
    "},\"required\":[\"name\"]}";

static const char *EMPTY_PARAMS =
    "{\"type\":\"object\",\"properties\":{}}";

int tool_extension_register(ToolRegistry *reg, ToolExtensionCtx *ctx) {
    int rc = 0;
    rc |= tools_register(reg, "extension_promote",
        "Promote a draft extension from your workspace (workspace/extensions/<name>) "
        "into the shared store (requires human approval — the approver sees the "
        "bundle's enumerated contents). On approval the manifest's tools/hooks/"
        "skills/config/agents register as a real extension owned by you.",
        NAME_PARAMS, tool_extension_promote_handler, ctx);
    rc |= tools_register(reg, "extension_publish",
        "Publish an extension you own so other agents can attach it. Sets the "
        "single publish flag; you must be the owner.",
        NAME_PARAMS, tool_extension_publish_handler, ctx);
    rc |= tools_register(reg, "extension_attach",
        "Attach a visible extension (published, or owned by you) to yourself. "
        "Attach does not grant its tools — request them via request_config.",
        NAME_PARAMS, tool_extension_attach_handler, ctx);
    rc |= tools_register(reg, "extension_list",
        "List extensions visible to you (owned or published), with owner, publish "
        "state, and whether you have each attached. Returns a JSON array.",
        EMPTY_PARAMS, tool_extension_list_handler, ctx);
    rc |= tools_register(reg, "extension_fork",
        "Copy a registered extension's bundle (published or yours) into "
        "your workspace as a draft (extensions/<as>) for development. The manifest "
        "is renamed to the fork, so promoting it later registers a separate "
        "extension — the original stays untouched.",
        FORK_PARAMS, tool_extension_fork_handler, ctx);
    /* Inline tools: apply in-process via ctx->db. */
    tools_set_recipe(reg, "extension_promote", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    tools_set_recipe(reg, "extension_publish", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    tools_set_recipe(reg, "extension_attach",  (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    tools_set_recipe(reg, "extension_list",    (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    tools_set_recipe(reg, "extension_fork",    (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}
