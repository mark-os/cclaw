#define _POSIX_C_SOURCE 200809L
#include "tool_bootstrap.h"
#include "agent_define.h"
#include "approval.h"
#include "config_registry.h"  /* config_get (install_update) */
#include "db.h"
#include "tool_args.h"        /* tool_args_str (install_update) */
#include "validate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── create_agent ─────────────────────────────────────────────────── */

static const char *CREATE_AGENT_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Agent name (PascalCase: uppercase first letter, letters and digits only)\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"One-line hint: when to delegate to this agent\"},"
    "\"system_prompt\":{\"type\":\"string\",\"description\":\"Agent persona/system prompt\"},"
    "\"models\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Routing order: registered model ids, first is primary (omit to inherit your own list)\"},"
    "\"sandbox_profile\":{\"type\":\"string\",\"enum\":[\"host\",\"standard\",\"restricted\"],\"description\":\"Containment profile; must not be looser than yours\"},"
    "\"grants\":{\"type\":\"object\",\"properties\":{"
      "\"tools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"hosts\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"read_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"write_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
      "\"description\":\"Grants beyond the baseline; each must be a grant you hold\"},"
    "\"extensions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extensions to attach (published, or owned by you)\"},"
    "\"memory_blocks\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"Memory blocks: {label, description, value, char_limit?, read_only?, placement?}\"},"
    "\"max_iterations\":{\"type\":\"integer\"},"
    "\"shell_timeout\":{\"type\":\"integer\"},"
    "\"clone_from\":{\"type\":\"string\",\"description\":\"Copy an existing agent's config/grants/extensions first, then overlay\"}"
    "},\"required\":[\"name\"]}";

static char *tool_create_agent_handler(const char *arguments, void *user_data, int *is_error) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return tool_fail(is_error, "error: create_agent unavailable");

    /* Eager validation with creator caps — a definition that can't apply
     * never parks. */
    char *err = NULL;
    if (agent_definition_validate(ctx->db, arguments, ctx->agent_name, &err) != 0) {
        char *msg;
        *is_error = 1;
        if (err) {
            size_t n = strlen(err) + 8;
            msg = malloc(n);
            if (msg) snprintf(msg, n, "error: %s", err);
            free(err);
        } else {
            msg = tool_fail(is_error, "error: invalid agent definition");
        }
        return msg ? msg : tool_fail(is_error, "error: invalid agent definition");
    }

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "create_agent", APPROVAL_PARK_REQUIRED,
        arguments, "apply");
    if (aid < 0)
        return tool_fail(is_error, "error: failed to create approval");
    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

/* ── update_agent ─────────────────────────────────────────────────── */

static const char *UPDATE_AGENT_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Agent to update — yourself, or one you created\"},"
    "\"description\":{\"type\":\"string\"},"
    "\"system_prompt\":{\"type\":\"string\",\"description\":\"Replacement system prompt (full text, not a diff)\"},"
    "\"models\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Replacement routing order: registered model ids within your own list, first is primary\"},"
    "\"sandbox_profile\":{\"type\":\"string\",\"enum\":[\"host\",\"standard\",\"restricted\"],\"description\":\"Must not be looser than yours\"},"
    "\"grants\":{\"type\":\"object\",\"properties\":{"
      "\"tools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"hosts\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"read_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"write_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
      "\"description\":\"Grants to ADD (never removes); each must be a grant you hold\"},"
    "\"max_iterations\":{\"type\":\"integer\"},"
    "\"shell_timeout\":{\"type\":\"integer\"},"
    "\"reason\":{\"type\":\"string\",\"description\":\"Shown to the human approver\"}"
    "},\"required\":[\"name\"]}";

static char *tool_update_agent_handler(const char *arguments, void *user_data, int *is_error) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return tool_fail(is_error, "error: update_agent unavailable");

    char *err = NULL;
    if (agent_definition_update_validate(ctx->db, arguments,
                                         ctx->agent_name, &err) != 0) {
        char *msg;
        *is_error = 1;
        if (err) {
            size_t n = strlen(err) + 8;
            msg = malloc(n);
            if (msg) snprintf(msg, n, "error: %s", err);
            free(err);
        } else {
            msg = tool_fail(is_error, "error: invalid agent update");
        }
        return msg ? msg : tool_fail(is_error, "error: invalid agent update");
    }

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "update_agent", APPROVAL_PARK_REQUIRED,
        arguments, "apply");
    if (aid < 0)
        return tool_fail(is_error, "error: failed to create approval");
    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

int tool_update_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    int rc = tools_register(reg, "update_agent",
                          "Propose changes to an existing agent — yourself, or one you "
                          "created (requires human approval). Overlays the fields you set "
                          "(description, system_prompt, models, sandbox_profile, "
                          "max_iterations, shell_timeout) and ADDS grants; absent fields "
                          "keep their value, grants are never removed. Capped by your own "
                          "profile and grants, like create_agent.",
                          UPDATE_AGENT_PARAMS,
                          tool_update_agent_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "update_agent",
                         (ToolRecipe){.vehicle = EXEC_INLINE,
                                      .null_kind = NULL_PARK});
    return rc;
}

int tool_create_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    int rc = tools_register(reg, "create_agent",
                          "Propose creation of a named agent (requires human approval). "
                          "Takes an agent definition: description, system_prompt, models, "
                          "sandbox_profile, grants, extensions, memory_blocks, or clone_from. "
                          "The new agent is capped by your own profile and grants.",
                          CREATE_AGENT_PARAMS,
                          tool_create_agent_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "create_agent",
                         (ToolRecipe){.vehicle = EXEC_INLINE,
                                      .null_kind = NULL_PARK});
    return rc;
}

/* ── install_update ──────────────────────────────────────────────────
 * The agent can notice a release (update_check_tick's inbox note) and OFFER
 * it — the offer is this park, the button is the approval. Installing is
 * never agent authority: approval forks the binary's own `update --tag`
 * rails (handshake, snapshot, atomic swap, crash-loop verify) in resolve.c.
 * Auto-update stays operator config. */

static const char *INSTALL_UPDATE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"tag\":{\"type\":\"string\",\"description\":\"Release tag to install; omit for the newest release the daemon's check has seen\"},"
    "\"reason\":{\"type\":\"string\",\"description\":\"Shown to the approver — what is new, why now\"}"
    "}}";

static char *tool_install_update_handler(const char *arguments, void *user_data, int *is_error) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return tool_fail(is_error, "error: install_update unavailable");

    char *repo = config_get(ctx->db, "update.repo");
    if (!repo || !repo[0]) {
        free(repo);
        return tool_fail(is_error, "error: no update.repo configured — "
                                   "updates are not set up on this deployment");
    }

    char *tag = tool_args_str(ctx->db, arguments, "tag");
    if (!tag) tag = config_get(ctx->db, "update.notified_tag");
    if (!tag || !tag[0]) {
        free(repo); free(tag);
        return tool_fail(is_error, "error: no known newer release — the "
                                   "daemon's release check has not seen one; "
                                   "pass a tag explicitly if you know it");
    }
    char *installed = config_get(ctx->db, "update.installed_tag");
    if (installed && strcmp(installed, tag) == 0) {
        char *msg = tool_fail(is_error,
            "error: %s is already the installed release", tag);
        free(repo); free(tag); free(installed);
        return msg;
    }
    free(installed);

    char *reason = tool_args_str(ctx->db, arguments, "reason");
    /* Rendered via json_object so a hostile tag/reason can't break the doc. */
    sqlite3_stmt *st;
    char *doc = NULL;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT json_object('tag', ?1, 'repo', ?2, 'reason', ?3,"
            " 'current', ?4);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, tag, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, repo, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, reason ? reason : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, VERSION_COMMIT, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *j = (const char *)sqlite3_column_text(st, 0);
            if (j) doc = strdup(j);
        }
        sqlite3_finalize(st);
    }
    free(reason); free(repo);
    if (!doc) { free(tag); return tool_fail(is_error, "error: OOM"); }

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "install_update", APPROVAL_PARK_REQUIRED,
        doc, "apply");
    free(doc); free(tag);
    if (aid < 0)
        return tool_fail(is_error, "error: failed to create approval");
    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

int tool_install_update_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    int rc = tools_register(reg, "install_update",
                          "Propose installing a newer cclaw release (requires human "
                          "approval). Approval runs the full update rails: schema "
                          "handshake, DB snapshot, atomic binary swap, crash-loop "
                          "revert; the daemon restarts only if the deployment "
                          "configures update.restart_command. Say what is new in "
                          "'reason' so the approver can decide.",
                          INSTALL_UPDATE_PARAMS,
                          tool_install_update_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "install_update",
                         (ToolRecipe){.vehicle = EXEC_INLINE,
                                      .null_kind = NULL_PARK});
    return rc;
}
