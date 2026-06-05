#ifndef CCLAW_LLM_CHILD_H
#define CCLAW_LLM_CHILD_H

#include <stdint.h>

/* T299: cclaw llm --session=N
 * Subprocess that owns the LLM call: context planning, request build,
 * curl + SSE parse, DB write. Stdout inherited (token stream).
 * Exit codes:
 *   0 = final response (stop, no tool_calls)
 *  10 = assistant has tool_calls (parent should dispatch)
 *   1 = error */
#define LLM_EXIT_STOP      0
#define LLM_EXIT_ERROR     1
#define LLM_EXIT_TOOLCALL 10

/* Entry point for `cclaw llm --session=N` subcommand.
 * Reads config from CCLAW_* env vars, opens DB, runs single LLM call,
 * writes entry to DB, exits. */
int llm_child_main(int64_t session_id);

#endif
