#ifndef CCLAW_CLI_H
#define CCLAW_CLI_H

#include "config.h"
#include <stdint.h>

/* CLI options passed from main */
typedef struct {
    int64_t session_id;      /* -1 = interactive select, 0 = force new */
    const char *config_path; /* passed to exec'd child agents */
    const char *prompt;      /* non-NULL = single-turn mode, print and exit */
} CliOpts;

/* Run CLI REPL: read user input, send to agent, print response.
 * Returns 0 on clean exit, -1 on error. */
int cli_run(const Config *cfg, const CliOpts *opts);

#endif
