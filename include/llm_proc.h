#ifndef CCLAW_LLM_PROC_H
#define CCLAW_LLM_PROC_H

#include <stdint.h>

/* LLM micro-process: context planning, request build, curl + SSE parse,
 * DB write. Stdout inherited in CLI mode (token stream).
 * Exit codes:
 *   0 = final response (stop, no tool_calls)
 *  10 = assistant has tool_calls (parent should dispatch)
 *   1 = error */
#define LLM_EXIT_STOP      0
#define LLM_EXIT_ERROR     1
#define LLM_EXIT_TOOLCALL 10

/* Entry point for LLM subprocess.
 * Reads config from CCLAW_* env vars, opens CCLAW_DB, runs single LLM call,
 * writes entry to DB, exits with LLM_EXIT_* code. */
int llm_proc_main(int64_t session_id);

#endif
