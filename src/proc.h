#ifndef CCLAW_PROC_H
#define CCLAW_PROC_H

/*
 * Process-lifetime state: the handles and identity this process is built
 * around. Written once during startup (main / run_daemon / run_cli), read
 * everywhere for the rest of the process's life.
 *
 * Accessors, not extern variables — the storage stays module-private so
 * archive-resident modules can reach the same state main.c does without
 * libcclaw.a growing a dependency on main.o.
 *
 * Unrelated to db.h's process_register / process_is_live liveness table,
 * which tracks *other* processes through the DB. Different naming family;
 * keep it that way.
 */

#include <stdint.h>

#include "sqlite3.h"
#include "types.h"
#include "agent_setup.h"

/* Registry instance id buffer: 32 hex chars + NUL, matching db.c. */
#define PROC_INSTANCE_ID_MAX 40

sqlite3 *proc_db(void);
void     proc_set_db(sqlite3 *db);

Config  *proc_cfg(void);
void     proc_set_cfg(Config *cfg);

/* 1 = daemon, 0 = CLI. */
int  proc_is_daemon(void);
void proc_set_daemon(int on);

/* "" until process_register() has succeeded. */
const char *proc_instance_id(void);
void        proc_set_instance_id(const char *id);

/* Agent this process was started for. The pointer is stable for the process
 * lifetime — AgentSetup contexts hold it so a rename is seen live. */
const char *proc_agent_name(void);
void        proc_set_agent_name(const char *name);

int64_t proc_cli_session(void);
void    proc_set_cli_session(int64_t session_id);

/* 1 while the CLI is waiting for a turn to finish. */
int  proc_cli_turn_active(void);
void proc_set_cli_turn_active(int active);

/* --auto-approve: answer parks without prompting (CLI only). */
int  proc_auto_approve(void);
void proc_set_auto_approve(int on);

/* Tool registry + per-tool contexts in use; NULL outside run_daemon/run_cli. */
AgentSetup *proc_tool_setup(void);
void        proc_set_tool_setup(AgentSetup *setup);

/* The --run-tool child gets a minimal env: just the log level, so its syslog
 * output (proxy denials, sandbox failures) honors the configured verbosity.
 * Prebuilt at startup — the fork child is async-signal-safe. Non-const
 * because execve's envp is char *const[]. */
char *proc_log_level_env(void);
char *proc_js_heap_env(void);
void proc_set_js_heap_env(int mb);
void  proc_set_log_level_env(const char *level_name);

#endif
