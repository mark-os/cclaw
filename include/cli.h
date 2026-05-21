#ifndef CCLAW_CLI_H
#define CCLAW_CLI_H

#include "config.h"

/* Run CLI REPL: read user input, send to agent, print response.
 * Creates a new session in the DB. Returns 0 on clean exit, -1 on error. */
int cli_run(const Config *cfg);

#endif
