#ifndef CCLAW_HOOK_DISPATCH_H
#define CCLAW_HOOK_DISPATCH_H

#include "extension.h"
#include <sqlite3.h>
#include <stdint.h>

/* Structured command hooks (specs/hooks.md): small JSON in, targeted JSON
 * commands out. Dispatch (build input via SQLite JSON1, run chained hooks in a
 * fresh QJS context, return accumulated command JSON) is separated from apply
 * (validate + SQL) so validation is unit-testable.
 *
 * Threading: dispatch runs on the main poll thread (the hooks QJS runtime is
 * main-thread-only, same as gate/observe). preAdvance commands cross to the
 * worker-thread payload build as DB state (hook_directives rows + entries.data
 * flags), never in memory. */

/* Dispatch preAdvance hooks. Input: {session_id, agent, entry_count, last_role,
 * last_entry_id, token_estimate, turn_number, pending_tool_calls}. Returns the
 * accumulated command JSON {inject, suppress, pin, set_data} (heap, caller
 * frees) or NULL when no hooks are registered / nothing returned commands. */
char *hook_dispatch_pre_advance(ExtensionCtx *ext_ctx, sqlite3 *db,
                                int64_t session_id);

/* Validate + apply preAdvance commands, order pin → set_data → suppress →
 * inject. Entry ids are validated against the session; suppress never hides
 * the most recent user entry or a pinned entry (pin wins); inject is capped
 * (3 entries / 2048 chars per dispatch). Returns 0 on success. */
int hook_apply_pre_advance(sqlite3 *db, int64_t session_id, const char *cmds_json);

/* Dispatch postAdvance hooks against the just-written assistant entry.
 * Input: {session_id, agent, entry_id, role, content, tool_calls, stop_reason,
 * usage_in, usage_out}. Hooks chain: a transform_content mutates input.content
 * so later hooks see it (last transform wins; annotate merges). Returns command
 * JSON {transform_content, annotate} (heap) or NULL. */
char *hook_dispatch_post_advance(ExtensionCtx *ext_ctx, sqlite3 *db,
                                 int64_t session_id, int64_t entry_id);

/* Apply postAdvance commands. transform_content rewrites the assistant entry
 * in place (type-gated — role/tool_calls untouchable by construction),
 * preserving data.original_content on first transform and keeping entries_fts
 * in sync. annotate merges into entries.data. Returns 0 on success. */
int hook_apply_post_advance(sqlite3 *db, int64_t session_id, int64_t entry_id,
                            const char *cmds_json);

/* §8 gating hook — restrict-only decision, most-restrictive wins. */
typedef enum { HOOK_GATE_ALLOW = 0, HOOK_GATE_ASK = 1, HOOK_GATE_DENY = 2 } HookGate;

/* §8 Gating hook (event "beforeToolCall"): fires once, after capability
 * resolution, before execution. Calls each hook with {name, args}. A hook may
 * only *restrict*: returning {deny:true,reason} (or {block:true}) → DENY,
 * {ask:true,reason} → ASK, anything else → ALLOW. The most restrictive decision
 * across all hooks wins — a hook can veto, never authorize. On DENY/ASK,
 * *reason_out (if non-NULL) receives a heap reason string (caller frees; may be
 * left NULL). Returns HOOK_GATE_ALLOW when no gating hooks are registered. */
HookGate hook_dispatch_gate_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                      const char *name, const char *args,
                                      char **reason_out);

/* afterToolCall hooks: fire after execution (post secret-scan, pre entry
 * write). Each hook gets {name, args, result}; returning {result} replaces the
 * result for the write AND for later hooks in the chain (each sees the
 * previous transform); {annotate} merges across hooks. Returns a heap
 * replacement result or NULL (unchanged). If annotate_out is non-NULL it
 * receives the merged annotate JSON object (heap) or NULL — caller patches it
 * onto the written entry via hook_entry_data_patch. */
char *hook_dispatch_observe_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                      const char *name, const char *args,
                                      const char *result, char **annotate_out);

/* Merge a JSON object into entries.data via json_patch. Returns 0 on success
 * (non-object patches are ignored, returning 0). */
int hook_entry_data_patch(sqlite3 *db, int64_t entry_id, const char *patch_json);

/* Delete this session's hook_directives rows — "this request only" semantics;
 * called at every llm_req exit (worker thread, its own DB connection). */
void hook_directives_clear(sqlite3 *db, int64_t session_id);

#endif
