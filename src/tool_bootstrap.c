#define _POSIX_C_SOURCE 200809L
#include "tool_bootstrap.h"
#include "agent_define.h"
#include "approval.h"
#include "db.h"
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
    "\"primary_model\":{\"type\":\"string\",\"description\":\"Primary model (omit for default)\"},"
    "\"secondary_model\":{\"type\":\"string\",\"description\":\"Fallback model (omit for default)\"},"
    "\"sandbox_profile\":{\"type\":\"string\",\"enum\":[\"host\",\"trusted\",\"standard\",\"restricted\"],\"description\":\"Containment profile; must not be looser than yours\"},"
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

static char *tool_create_agent_handler(const char *arguments, void *user_data) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: create_agent unavailable");

    /* Eager validation with creator caps — a definition that can't apply
     * never parks. */
    char *err = NULL;
    if (agent_definition_validate(ctx->db, arguments, ctx->agent_name, &err) != 0) {
        char *msg;
        if (err) {
            size_t n = strlen(err) + 8;
            msg = malloc(n);
            if (msg) snprintf(msg, n, "error: %s", err);
            free(err);
        } else {
            msg = strdup("error: invalid agent definition");
        }
        return msg ? msg : strdup("error: invalid agent definition");
    }

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "create_agent", "create_agent",
        arguments, "apply");
    if (aid < 0)
        return strdup("error: failed to create approval");
    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
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
                         (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}
