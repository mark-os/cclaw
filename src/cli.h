#ifndef CCLAW_CLI_H
#define CCLAW_CLI_H

/*
 * The interactive CLI front-end: terminal output (prompt, working indicator,
 * tool-call progress), the startup session picker, and the turn trigger.
 *
 * Presentation only — it owns stdout, not policy. The poll loop and argument
 * parsing stay in main.c; this module is what they print through.
 *
 * Lib-resident: loop.c's deliver_response clears the indicator, so cli.o
 * cannot live in main.o (which the Makefile filters out of libcclaw.a).
 */

#include <stdint.h>

#include "sqlite3.h"

/* Write the input prompt and flush. */
void cli_prompt(void);

/* Erase the transient "working" cue, if one is showing. Idempotent. */
void cli_indicator_clear(void);

/* Dimmed "[tool_name {args}]" progress line; args truncated at 200 chars. */
void cli_print_tool_call(const char *name, const char *args);

/* --help / usage text. */
void print_usage(void);

/* Resolve the session to attach to: --new creates, -s pins, otherwise the
 * interactive picker (or the most recent session on non-tty stdin). */
int64_t cli_select_session(sqlite3 *db, int64_t requested_id, int new_session);

/* Queue one line of user input and start advancing the CLI session. */
void cli_start_turn(const char *input);

#endif
