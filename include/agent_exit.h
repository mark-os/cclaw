#ifndef CCLAW_AGENT_EXIT_H
#define CCLAW_AGENT_EXIT_H

/* V72: Agent process exit codes — sole IPC channel for intent.
 * Daemon dispatches on code after waitpid. */
#define AGENT_EXIT_DONE     0   /* turn complete — deliver response */
#define AGENT_EXIT_ERROR    1   /* error */
#define AGENT_EXIT_SPAWN    2   /* spawn sub-agent request */
#define AGENT_EXIT_APPROVAL 3   /* approval requested */
#define AGENT_EXIT_CONFIG   4   /* config change requested */
/* 127 = exec failed, 128+N = killed by signal N */

/* Sentinel prefixes returned by tools to signal exit intent.
 * agent_run detects these and returns the corresponding exit code. */
#define SENTINEL_SPAWN      "AGENT_EXIT_SPAWN:"
#define SENTINEL_APPROVAL   "AGENT_EXIT_APPROVAL:"
#define SENTINEL_CONFIG     "AGENT_EXIT_CONFIG:"

#endif
