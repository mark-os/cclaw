#include <pthread.h>
#include <stdio.h>

#include "log.h"

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

/* The handle belongs to the thread that installed it (the event loop). A
 * transaction is a property of the handle, so a second thread using it can
 * end or join one the loop has open. Any other thread opens its own handle
 * with db_open(); this counts the ones that do not, and says so once. */
static pthread_t   g_db_owner;
static int         g_db_owned;
static int         g_db_foreign_uses;

sqlite3 *proc_db(void) {
    if (g_db_owned && !pthread_equal(pthread_self(), g_db_owner)) {
        if (__atomic_fetch_add(&g_db_foreign_uses, 1, __ATOMIC_RELAXED) == 0)
            LOG_ERROR_("BUG: proc_db() used off the event-loop thread —"
                       " open a separate handle with db_open()");
    }
    return g_db;
}
void proc_set_db(sqlite3 *db) {
    g_db = db;
    g_db_owner = pthread_self();
    g_db_owned = (db != NULL);
}
int proc_db_foreign_uses(void) {
    return __atomic_load_n(&g_db_foreign_uses, __ATOMIC_RELAXED);
}

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
