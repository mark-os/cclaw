#ifndef CCLAW_QJS_LLM_H
#define CCLAW_QJS_LLM_H

/* The LLM() global for the sandboxed JS tier: a thin client for the parent's
 * per-call bridge socket (llm_bridge.h, path in $CCLAW_LLM_SOCK). All
 * routing, key resolution, body construction, accounting, and archiving
 * happen in the parent (llm_request, llm_proc.c) — no key material exists in
 * this process at all. LLM is a callable namespace (all-caps, like XML):
 * future shapes (LLM.embed, LLM.transcribe, LLM.batch) hang off the same
 * global as the core grows them. */

#include "qjs_helpers.h"

void qjs_register_llm(JSContext *ctx);

#endif
