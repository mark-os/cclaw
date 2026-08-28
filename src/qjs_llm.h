#ifndef CCLAW_QJS_LLM_H
#define CCLAW_QJS_LLM_H

/* The llm() global for the sandboxed JS tier: one-shot completions against
 * the agent's OWN model routing list, resolved in the trusted parent and
 * shipped as candidate descriptors (llm_wire_json) in the run-tool blob.
 * The child does no routing policy of its own — it walks the list in the
 * order the parent ranked it, over the same proxied HTTP path as
 * http_request. Auth headers (key included) live in C-held values and are
 * never attached to the JS global object.
 */

#include "qjs_helpers.h"

/* Default/ceiling for one llm() HTTP attempt. Grounded or reasoning calls
 * routinely run ~1min on slow targets; the ceiling matches http_request's. */
#define QJS_LLM_TIMEOUT_DEFAULT 120

/* Candidate-descriptor JSON (borrowed; caller keeps it alive across eval).
 * NULL/empty clears — llm() then throws "no routable model". */
void qjs_host_set_llm(const char *llm_json);

/* Register the llm() global on an eval-profile context. */
void qjs_register_llm(JSContext *ctx);

#endif
