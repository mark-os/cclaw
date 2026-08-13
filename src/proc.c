#include <stdio.h>

#include "proc.h"

static sqlite3    *g_db;
static Config     *g_cfg;
static int         g_daemon;
static char        g_instance_id[PROC_INSTANCE_ID_MAX];
static char        g_agent_name[64];
static int64_t     g_cli_session;
static int         g_cli_turn_active;
static int         g_auto_approve;
static AgentSetup *g_tool_setup;
static char        g_log_level_env[32] = "CCLAW_LOG_LEVEL=info";
static char        g_js_heap_env[32] = "CCLAW_JS_HEAP_MB=8";

sqlite3 *proc_db(void) { return g_db; }
void proc_set_db(sqlite3 *db) { g_db = db; }

Config *proc_cfg(void) { return g_cfg; }
void proc_set_cfg(Config *cfg) { g_cfg = cfg; }

int proc_is_daemon(void) { return g_daemon; }
void proc_set_daemon(int on) { g_daemon = on; }

const char *proc_instance_id(void) { return g_instance_id; }
void proc_set_instance_id(const char *id) {
    snprintf(g_instance_id, sizeof g_instance_id, "%s", id ? id : "");
}

const char *proc_agent_name(void) { return g_agent_name; }
void proc_set_agent_name(const char *name) {
    snprintf(g_agent_name, sizeof g_agent_name, "%s", name ? name : "");
}

int64_t proc_cli_session(void) { return g_cli_session; }
void proc_set_cli_session(int64_t session_id) { g_cli_session = session_id; }

int proc_cli_turn_active(void) { return g_cli_turn_active; }
void proc_set_cli_turn_active(int active) { g_cli_turn_active = active; }

int proc_auto_approve(void) { return g_auto_approve; }
void proc_set_auto_approve(int on) { g_auto_approve = on; }

AgentSetup *proc_tool_setup(void) { return g_tool_setup; }
void proc_set_tool_setup(AgentSetup *setup) { g_tool_setup = setup; }

char *proc_log_level_env(void) { return g_log_level_env; }

/* The --run-tool broker has no DB, so the js_eval heap cap rides to it the
 * same way the log level does: resolved from config here, passed in envp. */
char *proc_js_heap_env(void) { return g_js_heap_env; }
void proc_set_js_heap_env(int mb) {
    if (mb < 1) mb = 1;
    if (mb > 512) mb = 512;
    snprintf(g_js_heap_env, sizeof g_js_heap_env, "CCLAW_JS_HEAP_MB=%d", mb);
}
void proc_set_log_level_env(const char *level_name) {
    snprintf(g_log_level_env, sizeof g_log_level_env, "CCLAW_LOG_LEVEL=%s",
             level_name ? level_name : "info");
}
