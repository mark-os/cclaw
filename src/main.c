#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cclaw.h"
#include "config.h"
#include "config_registry.h"
#include "dashboard.h"
#include "log.h"
#include "sandbox.h"
#include "tool_js.h"
#include "qjs_helpers.h"
#include "agent_config.h"
#include "agent_define.h"
#include "skills.h"
#include "agent_setup.h"
#include "hook_dispatch.h"
#include "approval.h"
#include "buf.h"
#include "llm_proc.h"
#include "llm_worker.h"
#include "tool_thread.h"
#include "tools.h"
#include "tool_args.h"
#include "tool_request_config.h"
#include "context.h"
#include "db.h"
#include "secret_store.h"
#include "cli_verbs.h"
#include "templates.h"
#include "db_response.h"
#include "shutdown.h"
#include "crash.h"
#include "channel_harness.h"
#include "extension_manifest.h"
#include "doctor.h"
#include "install.h"
#include "wake.h"
#include "advance.h"
#include "channel.h"
#include "channel_api.h"
#include "channel_runner.h"
#include "secret.h"
#include "validate.h"
#include "secret_scan.h"
#include "tool_policy.h"
#include "secret_interp.h"
#include "secret_capture.h"
#include "external_content.h"
#include "unicode_normalize.h"
#include "run_tool.h"
#include "tool_file.h"
#include "resolve.h"
#include "web.h"
#include "cron.h"
#include "child.h"
#include "cli.h"
#include "dispatch.h"
#include "loop.h"
#include "proc.h"
#include "sweep.h"
#include "util.h"

/* vendor/sqlite3/shell.c, compiled with -Dmain=sqlite3_shell_main */
int sqlite3_shell_main(int argc, char **argv);

_Static_assert(sizeof(WakeMsg) <= PIPE_BUF,
    "WakeMsg must fit in PIPE_BUF so wake-pipe writes stay atomic");

/* ── Globals ────────────────────────────────────────────────────── */

static int g_llm_threads = 4;     /* worker thread pool size */
static int g_cli_done;          /* 1 = exit after turn completes (for -p mode) */

/* ── compute_timeout_ms: dynamic poll() timeout ─────────────── */

#define POLL_DB_INTERVAL 30  /* seconds between DB polls (heartbeat, recovery, approvals) */
#define POLL_MAX_SLEEP   30  /* upper bound on poll() sleep */

static time_t g_next_db_poll;  /* next time DB periodic work is due */

static int compute_timeout_ms(void) {
    time_t now = time(NULL);
    time_t nearest = now + POLL_MAX_SLEEP;

    /* Tier 1: in-memory deadlines (precise) */
    for (int i = 0; i < child_count(); i++) {
        time_t d = child_at(i)->deadline;
        if (d > 0 && d < nearest)
            nearest = d;
    }
    if (proc_is_daemon()) {
        time_t cd = channel_next_deadline();
        if (cd > 0 && cd < nearest)
            nearest = cd;
    }

    /* Tier 2: periodic DB poll */
    if (g_next_db_poll < nearest)
        nearest = g_next_db_poll;

    int ms = (int)((nearest - now) * 1000);
    return ms < 0 ? 0 : ms;
}

/* ── event_step: shared event handling for both daemon and CLI loops ── */

/* postAdvance hooks: fire on the worker-completion wake, before run_advance
 * consumes the llm_running state. The state guard filters compaction
 * completions riding the same pipe; error entries are skipped (nothing for a
 * redact/annotate hook to act on, and error filtering drops them from context anyway). */
static void maybe_dispatch_post_advance(int64_t session_id) {
    if (!proc_tool_setup() ||
        proc_tool_setup()->ext_ctx.hooks[HOOK_POST_ADVANCE].count == 0)
        return;

    int64_t entry_id = -1;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(proc_db(),
            "SELECT e.id FROM entries e"
            " WHERE e.id = (SELECT MAX(id) FROM entries"
            "                WHERE session_id=?1 AND type='assistant_message')"
            "   AND e.stop_reason != 4"  /* STOP_REASON_ERROR */
            "   AND (SELECT state FROM sessions WHERE id=?1) = 'llm_running';",
            -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, session_id);
    if (sqlite3_step(s) == SQLITE_ROW)
        entry_id = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    if (entry_id < 0) return;

    char *cmds = hook_dispatch_post_advance(&proc_tool_setup()->ext_ctx, proc_db(),
                                            session_id, entry_id);
    if (cmds) {
        hook_apply_post_advance(proc_db(), session_id, entry_id, cmds);
        free(cmds);
    }
}

static void event_step_worker(int worker_fd) {
    (void)worker_fd;
    int64_t completed_sid;
    while (llm_worker_read(&completed_sid) == 0) {
        if (completed_sid == -1) continue;
        maybe_dispatch_post_advance(completed_sid);
        run_advance(completed_sid);
    }
    /* The job row this completion deleted was a concurrency-gate resource —
     * re-advance anything deferred on the cap. After the completions above,
     * so in-flight turns keep their momentum before queued opens compete. */
    stalled_drain();
}

/* Drain tool-thread completion notifications: a finished EXEC_THREAD tool wrote
 * its result + completed its call with its own db, then pushed session_id here.
 * Advance on the poll thread exactly like worker/child completions. */
static void event_step_tool_thread(void) {
    int64_t completed_sid;
    while (tool_thread_read(&completed_sid) == 0) {
        if (completed_sid == -1) continue;
        run_advance(completed_sid);
    }
    stalled_drain();   /* a running tool_call resolved — a gate slot freed */
}

static void event_step_chld(void) {
    char buf[64];
    while (read(child_sigchld_fd(), buf, sizeof(buf)) > 0) {}
    reap_children();
}

/* ── main ───────────────────────────────────────────────────────── */

/* PATH_MAX-sized buffers make gcc's -Wformat-truncation flag snprintfs in the
 * functions below as possibly truncating, even though the inputs never
 * approach that length; clang doesn't have this warning at all. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif


/* Bootstrap the default agent on a fresh DB. Shared by CLI and daemon —
 * a headless install (daemon-only, e.g. a channel-driven deploy) must not
 * depend on someone having run the CLI once to get a routable agent. */
static void ensure_default_agent(const char *base_dir) {
    int ac = 0; char **al = db_agent_list(proc_db(), &ac);
    if (!al || ac == 0) {
        char ws[PATH_MAX]; snprintf(ws, sizeof(ws), "%s/agents/Assistant/workspace/.keep", base_dir);
        util_ensure_parent_dir(ws);
        /* Create default agent */
        const char *agent_sql =
            "INSERT OR IGNORE INTO agents(name, system_prompt, sandbox_profile)"
            " VALUES('Assistant', ?, 'standard');"
            ;
        sqlite3_stmt *bs;
        if (sqlite3_prepare_v2(proc_db(), agent_sql, -1, &bs, NULL) == SQLITE_OK) {
            sqlite3_bind_text(bs, 1, TPL_DEFAULT_SYSTEM_PROMPT_MD, -1, SQLITE_STATIC);
            sqlite3_step(bs); sqlite3_finalize(bs);
        }
        /* One-row routing list: the seeded bootstrap model. Explicit — an
         * agent with no list is unroutable by design (model-routing.md R2). */
        sqlite3_exec(proc_db(),
            "INSERT OR IGNORE INTO agent_models(agent_name, model_id, pos)"
            " SELECT 'Assistant', 'openrouter/deepseek/deepseek-v4-flash', 0"
            " WHERE EXISTS (SELECT 1 FROM models"
            "               WHERE id='openrouter/deepseek/deepseek-v4-flash');",
            NULL, NULL, NULL);
        /* Seed default tools as grants */
        agent_grant_defaults(proc_db(), "Assistant");
        /* Disabled 'heartbeat' bare-wake job — visible/enable-able in cron_list. */
        cron_seed_heartbeat(proc_db(), "Assistant");
        /* default_agent needs no write — 'Assistant' is the registry default */
        /* Seed default memory blocks. Explicitly 'system' placement: identity
         * (who am I / who is the user) belongs in the system prompt, where it
         * changes rarely and stays in the cached prefix — unlike new blocks,
         * which default to the per-turn 'context' placement. */
        memory_block_create(proc_db(), "Assistant", "AGENT",
            "Your identity, capabilities, and operational notes. Update as you learn about yourself.",
            5000, "system");
        memory_block_create(proc_db(), "Assistant", "USER",
            "Information about the user: preferences, context, working style. Update as you learn.",
            5000, "system");
        /* AGENT gets one functional starter entry (it drives the self-naming
         * flow). USER starts empty — its description already states the block's
         * purpose, so a placeholder entry would just be noise the model has to
         * carry until it edits it away. */
        memory_entry_add(proc_db(), "Assistant", "AGENT",
            "You are CClaw. You do not have a name yet — ask the user what they would like to call you, then save it here with memory_edit.");
    }
    if (al) { for (int i = 0; i < ac; i++) free(al[i]); free(al); }
}

static int run_daemon(char *db_path) {
    proc_set_daemon(1);
    g_next_db_poll = time(NULL);  /* run DB checks immediately on first iter */
    workspace_init(proc_cfg());

    /* Tool dispatch setup. Without this proc_tool_setup() is NULL and every forkable
     * tool resolves to "unknown tool" in daemon mode. The daemon serves many
     * agents through this one shared setup; caps are re-bound to the advancing
     * session's agent before each dispatch (run_advance). The init agent name
     * just seeds the caps/contexts; agents_dir backs rename support. */
    char daemon_agent[64];
    { char *def = config_get(proc_db(), "default_agent");
      snprintf(daemon_agent, sizeof(daemon_agent), "%s", def ? def : "Assistant");
      free(def); }
    proc_set_agent_name(daemon_agent);
    char daemon_agents_dir[PATH_MAX];
    { char base[PATH_MAX]; snprintf(base, sizeof(base), "%s", db_path);
      char *sl = strrchr(base, '/'); if (sl) *sl = '\0'; else snprintf(base, sizeof(base), ".");
      snprintf(daemon_agents_dir, sizeof(daemon_agents_dir), "%s/agents", base);
      ensure_default_agent(base); }
    AgentSetup daemon_setup;
    agent_setup_init(&daemon_setup, proc_db(), 0, proc_cfg(), proc_agent_name());
    daemon_setup.req_cfg_ctx.agents_dir = daemon_agents_dir;
    daemon_setup.req_cfg_ctx.agent_name = proc_agent_name();
    proc_set_tool_setup(&daemon_setup);
    /* Scheduled scripts need the child table, which only the daemon has. */
    cron_set_script_runner(cron_script_run);

    printf("cclaw %s — daemon mode\n", CCLAW_VERSION);
    /* Resolved config, once, at default verbosity: a daemon that talks to the
     * wrong provider or writes to the wrong workspace should say so in the log
     * rather than in a support thread. The key is named by *source* only —
     * never any part of its value. */
    LOG_INFO_("daemon: config: provider=%s model=%s endpoint=%s wire=%s"
              " workspace=%s db=%s api_key=%s",
              proc_cfg()->provider.name ? proc_cfg()->provider.name : "(builtin)",
              proc_cfg()->provider.model ? proc_cfg()->provider.model : "(unset)",
              proc_cfg()->provider.base_url ? proc_cfg()->provider.base_url : "(unset)",
              proc_cfg()->provider.endpoint_type == ENDPOINT_GEMINI ? "gemini" : "openai",
              proc_cfg()->workspace ? proc_cfg()->workspace : "(unset)",
              proc_cfg()->db_path ? proc_cfg()->db_path : "(unset)",
              proc_cfg()->provider.api_key_source ? proc_cfg()->provider.api_key_source
                                             : "(none)");
    web_start(proc_cfg(), proc_db(), db_path);
    /* No heartbeat thread: the pulse is a seeded bare-wake cron row fired by
     * cron_run_due off the db_periodic tick — one scheduler. */

    /* Start LLM worker threads */
    if (llm_worker_start(db_path, g_llm_threads) != 0) {
        LOG_ERROR_("daemon: failed to start LLM worker");
        config_free(proc_cfg()); db_close(proc_db()); free(db_path); return 1;
    }
    int daemon_worker_fd = llm_worker_fd();
    util_set_nonblock(daemon_worker_fd);

    /* Re-run media jobs (voice transcriptions) a crashed run left behind —
     * same crash-recovery idea as llm_jobs, except these rows are re-runnable
     * (input is in the row), so resubmit instead of delete. */
    llm_worker_resubmit_media(proc_db());

    /* Fire-and-forget tool threads (EXEC_THREAD vehicle) */
    if (tool_thread_start(db_path, 0) != 0) {
        LOG_ERROR_("daemon: failed to start tool threads");
        llm_worker_stop();
        config_free(proc_cfg()); db_close(proc_db()); free(db_path); return 1;
    }
    int daemon_tool_fd = tool_thread_fd();

    /* Init wake pipe + FIFO */
    wake_init();
    int fifo_fd = wake_fifo_open(db_path);

    /* SIGCHLD self-pipe */
    if (child_sigchld_init() != 0) return 1;

    /* Launch channel processes */
    channel_launch_all(proc_db());

    /* Sweep events parked from a previous life. Consume is otherwise only
     * triggered by a FIFO wake, so anything left in channel_events at crash
     * or restart would wait for the *next* inbound message to be replayed. */
    channel_consume_events(proc_db());

    /* poll() setup — sized for fixed fds + all children with result pipes */
    int max_pfds = 6 + CHILD_MAX;  /* chld, wake, fifo, worker, tool-thread, result pipes */
    struct pollfd *pfds = malloc(max_pfds * sizeof(struct pollfd));
    if (!pfds) { perror("malloc"); return 1; }

    /* Register in the liveness table so this daemon's in-flight sessions are
     * owner-stamped and never reclaimed by a peer's recovery. */
    char inst[PROC_INSTANCE_ID_MAX] = {0};
    if (process_register(proc_db(), "daemon", getpid(), inst, sizeof inst) != 0)
        LOG_ERROR_("daemon: process registration failed");
    proc_set_instance_id(inst);
    db_set_instance_id(proc_instance_id());

    /* Daemon event loop */
    while (!shutdown_requested()) {
        /* Rebuild pollfd set each iteration to include active result pipes */
        int nfds = 0;
        pfds[nfds].fd = child_sigchld_fd(); pfds[nfds].events = POLLIN; nfds++;
        pfds[nfds].fd = wake_fd(); pfds[nfds].events = POLLIN; nfds++;
        int fifo_idx = -1;
        if (fifo_fd >= 0) { fifo_idx = nfds; pfds[nfds].fd = fifo_fd; pfds[nfds].events = POLLIN; nfds++; }
        int d_worker_idx = nfds;
        pfds[nfds].fd = daemon_worker_fd; pfds[nfds].events = POLLIN; nfds++;
        int d_tool_idx = nfds;
        pfds[nfds].fd = daemon_tool_fd; pfds[nfds].events = POLLIN; nfds++;

        int result_pipe_base = nfds;
        nfds = add_result_pipe_fds(pfds, nfds, max_pfds);

        int rc = poll(pfds, (nfds_t)nfds, compute_timeout_ms());
        if (rc < 0) { if (errno == EINTR) continue; break; }

        drain_ready_result_pipes(pfds, result_pipe_base, nfds);

        if (pfds[0].revents & POLLIN)
            event_step_chld();
        if (pfds[1].revents & POLLIN) {
            WakeMsg msg;
            while (read(wake_fd(), &msg, sizeof(msg)) == (ssize_t)sizeof(msg)) {
                if (child_has_session(msg.session_id)) continue;
                run_advance(msg.session_id);
            }
        }
        if (fifo_idx >= 0 && (pfds[fifo_idx].revents & POLLIN)) {
            char drain[64];
            while (read(fifo_fd, drain, sizeof(drain)) > 0) {}
            channel_consume_events(proc_db());
        }
        if (pfds[d_worker_idx].revents & POLLIN)
            event_step_worker(daemon_worker_fd);
        if (pfds[d_tool_idx].revents & POLLIN)
            event_step_tool_thread();

        child_sweep_deadlines();
        channel_tick(proc_db());

        /* DB periodic work — gated on interval */
        time_t now = time(NULL);
        if (now >= g_next_db_poll) {
            db_periodic();
            g_next_db_poll = now + POLL_DB_INTERVAL;
        }
    }

    free(pfds);

    /* Shutdown */
    llm_worker_stop();
    tool_thread_stop();  /* drain in-flight tool threads before freeing state */
    channel_shutdown_all();
    web_stop();
    child_sigchld_teardown();
    wake_close(); wake_fifo_close(fifo_fd, db_path);
    process_unregister(proc_db(), proc_instance_id());
    cron_set_script_runner(NULL);
    proc_set_tool_setup(NULL);
    agent_setup_destroy(&daemon_setup);
    config_free(proc_cfg()); db_close(proc_db()); free(db_path);
    return 0;
}

static int run_cli(char *db_path, const char *prompt,
                   int64_t session_id, int new_session, int host_mode, int auto_approve) {
    /* ── CLI mode ────────────────────────────────────────────────── */
    proc_set_daemon(0);
    proc_set_auto_approve(auto_approve);
    g_next_db_poll = time(NULL);

    if (!proc_cfg()->provider.api_key || !proc_cfg()->provider.api_key[0]) {
        fprintf(stderr, "error: no API key (set OPENROUTER_API_KEY)\n");
        config_free(proc_cfg()); db_close(proc_db()); free(db_path); return 1;
    }

    /* Derive base_dir for workspace */
    char *base_dir = strdup(db_path);
    { char *sl = strrchr(base_dir, '/'); if (sl) *sl = '\0'; else { free(base_dir); base_dir = strdup("."); } }

    ensure_default_agent(base_dir);

    /* Agent selection */
    char *agent_sel = NULL;
    if (prompt || !isatty(STDIN_FILENO)) {
        char *def = config_get(proc_db(), "default_agent");
        agent_sel = def ? def : strdup("Assistant");
    } else {
        int ac = 0; char **al = db_agent_list(proc_db(), &ac);
        if (ac == 1) { agent_sel = strdup(al[0]); }
        else if (ac > 1) {
            printf("agents:\n");
            for (int i = 0; i < ac; i++) printf("  %d) %s\n", i+1, al[i]);
            printf("select: "); fflush(stdout);
            char buf[64]; if (fgets(buf, sizeof(buf), stdin)) {
                int ch = atoi(buf);
                if (ch >= 1 && ch <= ac) agent_sel = strdup(al[ch-1]);
            }
        }
        if (al) { for (int i = 0; i < ac; i++) free(al[i]); free(al); }
    }
    if (!agent_sel) { fprintf(stderr, "no agent selected\n"); free(base_dir); config_free(proc_cfg()); db_close(proc_db()); free(db_path); return 1; }
    proc_set_agent_name(agent_sel);
    setenv("CCLAW_AGENT_NAME", proc_agent_name(), 1);
    free(agent_sel);

    /* Inject agent config env vars (for forked children). -y skips kernel
     * isolation only — the agent's tool allowlist still applies. */
    {
        AgentConfig *ac = agent_config_load_db(proc_db(), proc_agent_name());
        if (ac) {
            if (ac->tool_count > 0) {
                size_t len = 0; for (size_t i = 0; i < ac->tool_count; i++) len += strlen(ac->tools[i]) + 1;
                char *csv = malloc(len); if (csv) { csv[0] = '\0';
                    for (size_t i = 0; i < ac->tool_count; i++) { if (i) strcat(csv, ","); strcat(csv, ac->tools[i]); }
                    setenv("CCLAW_TOOLS", csv, 1); free(csv); }
            }
            agent_config_free(ac);
        }
    }
    if (host_mode) setenv("CCLAW_SANDBOX_PROFILE", "host", 1);
    setenv("CCLAW_MODE", "cli", 1);
    workspace_init(proc_cfg());
    /* Make the workspace the process cwd so relative paths, shell children, and
     * fs.* all operate in the agent's workspace by default. (CLI only — the
     * daemon serves multiple agents and its tool children chdir per-agent.) */
    if (proc_cfg()->workspace && chdir(proc_cfg()->workspace) != 0)
        fprintf(stderr, "warning: chdir to workspace %s failed: %s\n",
                proc_cfg()->workspace, strerror(errno));
    { char cwd[PATH_MAX]; if (getcwd(cwd, sizeof(cwd))) setenv("CCLAW_PATH", cwd, 1); }

    /* Set up tool schemas env for LLM proc children */
    AgentSetup setup;
    agent_setup_init(&setup, proc_db(), 0, proc_cfg(), proc_agent_name());
    proc_set_tool_setup(&setup);
    /* Set agents_dir for rename support; point agent_name at proc_agent_name() for live update */
    char agents_dir[PATH_MAX];
    snprintf(agents_dir, sizeof(agents_dir), "%s/agents", base_dir);
    setup.req_cfg_ctx.agents_dir = agents_dir;
    setup.req_cfg_ctx.agent_name = proc_agent_name();

    /* Session selection */
    session_id = cli_select_session(proc_db(), session_id, new_session);
    if (session_id < 0) { agent_setup_destroy(&setup); free(base_dir); config_free(proc_cfg()); db_close(proc_db()); free(db_path); return 1; }
    proc_set_cli_session(session_id);
    setup.req_cfg_ctx.session_id = session_id;

    /* Register in the liveness table before touching session state, so our
     * transitions stamp owner_instance and recovery won't reclaim them. */
    char inst[PROC_INSTANCE_ID_MAX] = {0};
    if (process_register(proc_db(), "cli", getpid(), inst, sizeof inst) != 0)
        LOG_WARN_("process registration failed kind=cli");
    proc_set_instance_id(inst);
    db_set_instance_id(proc_instance_id());

    /* Refuse to drive a session another live process is mid-turn on — two
     * writers would corrupt its branch. Idle (owner NULL) or dead-owned
     * sessions are takeable; only a live *other* owner blocks us. */
    {
        char st[32] = {0}, owner[40] = {0};
        sqlite3_stmt *gs;
        if (sqlite3_prepare_v2(proc_db(),
                "SELECT state, COALESCE(owner_instance,'') FROM sessions WHERE id=?;",
                -1, &gs, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(gs, 1, session_id);
            if (sqlite3_step(gs) == SQLITE_ROW) {
                snprintf(st, sizeof st, "%s", (const char *)sqlite3_column_text(gs, 0));
                snprintf(owner, sizeof owner, "%s", (const char *)sqlite3_column_text(gs, 1));
            }
            sqlite3_finalize(gs);
        }
        int transient = strcmp(st, "idle") != 0 && st[0];
        if (transient && owner[0] && strcmp(owner, proc_instance_id()) != 0 &&
            process_is_live(proc_db(), owner, PROCESS_TTL_SEC)) {
            char omode[16] = "cclaw"; int opid = 0;
            sqlite3_stmt *ps;
            if (sqlite3_prepare_v2(proc_db(),
                    "SELECT mode, pid FROM processes WHERE instance_id=?;",
                    -1, &ps, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ps, 1, owner, -1, SQLITE_STATIC);
                if (sqlite3_step(ps) == SQLITE_ROW) {
                    const char *m = (const char *)sqlite3_column_text(ps, 0);
                    if (m) snprintf(omode, sizeof omode, "%s", m);
                    opid = sqlite3_column_int(ps, 1);
                }
                sqlite3_finalize(ps);
            }
            fprintf(stderr,
                    "error: session %lld is being driven by %s pid %d\n",
                    (long long)session_id, omode, opid);
            process_unregister(proc_db(), proc_instance_id());
            agent_setup_destroy(&setup); free(base_dir);
            config_free(proc_cfg()); db_close(proc_db()); free(db_path);
            return 1;
        }
    }

    /* ── SIGCHLD self-pipe ───────────────────────────────────────── */
    if (child_sigchld_init() != 0) return 1;

    /* ── poll() setup ──────────────────────────────────────────────── */
    /* Allocate for fixed fds + all children with result pipes */
    int cli_max_pfds = 5 + CHILD_MAX;  /* worker, stdin, chld, wake, tool-thread + pipes */
    struct pollfd *cli_pfds = malloc(cli_max_pfds * sizeof(struct pollfd));
    if (!cli_pfds) { perror("malloc"); return 1; }

    /* Start LLM worker */
    int worker_fd = -1;
    int worker_idx = -1;  /* slot index, recomputed each loop iteration */
    if (llm_worker_start(db_path, g_llm_threads) == 0) {
        worker_fd = llm_worker_fd();
        util_set_nonblock(worker_fd);
    } else {
        LOG_ERROR_("llm worker start failed");
        agent_setup_destroy(&setup); free(base_dir); config_free(proc_cfg()); db_close(proc_db()); free(db_path); return 1;
    }

    /* Fire-and-forget tool threads (EXEC_THREAD vehicle); cli_mode=1 prints the
     * dim "→ result" progress line like the inline path. */
    int cli_tool_fd = -1;
    if (tool_thread_start(db_path, 1) == 0) cli_tool_fd = tool_thread_fd();

    /* Wake pipe: sub-agent launch and completion wake sessions through this fd,
     * exactly as in the daemon loop. Without it wake_session() is a no-op, so a
     * spawned child session would never advance and a parent waiting on it would
     * hang. This is what lets the CLI run the multi-agent code, not just the
     * daemon — the only remaining difference is that the CLI has no channels. */
    int wake_pipe_fd = -1;
    if (wake_init() == 0) wake_pipe_fd = wake_fd();

    /* Register stdin for interactive mode */
    int use_stdin = 0;
    int stdin_idx = -1;   /* slot index, recomputed each loop iteration */
    if (!prompt && isatty(STDIN_FILENO)) {
        util_set_nonblock(STDIN_FILENO);
        use_stdin = 1;
        printf("cclaw cli (type 'exit' or Ctrl-D to quit)\n> ");
        fflush(stdout);
    }

    /* Single-turn mode: -p <prompt> */
    if (prompt) {
        g_cli_done = 1;
        cli_start_turn(prompt);
    }

    /* ── Event loop ──────────────────────────────────────────────── */
    int rc = 0;

    while (!shutdown_requested()) {
        /* Rebuild pollfd set each iteration to include active result pipes.
         * Slot indexes are recomputed because slot order depends on which
         * fixed fds are present. */
        int cli_nfds = 0;
        worker_idx = -1;
        stdin_idx = -1;
        if (worker_fd >= 0) {
            worker_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = worker_fd; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        }
        if (use_stdin) {
            stdin_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = STDIN_FILENO; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        }
        int chld_idx = cli_nfds;
        cli_pfds[cli_nfds].fd = child_sigchld_fd(); cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;

        int wake_idx = -1;
        if (wake_pipe_fd >= 0) {
            wake_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = wake_pipe_fd; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        }
        int tool_idx = -1;
        if (cli_tool_fd >= 0) {
            tool_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = cli_tool_fd; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        }

        int result_pipe_base = cli_nfds;
        cli_nfds = add_result_pipe_fds(cli_pfds, cli_nfds, cli_max_pfds);

        int nready = poll(cli_pfds, (nfds_t)cli_nfds, compute_timeout_ms());
        if (nready < 0) { if (errno == EINTR) continue; break; }

        drain_ready_result_pipes(cli_pfds, result_pipe_base, cli_nfds);

        if (cli_pfds[chld_idx].revents & POLLIN) {
            event_step_chld();

            if (!proc_is_daemon() && !proc_cli_turn_active()) {
                if (g_cli_done) goto done;
                cli_prompt();
            }
        }
        if (worker_idx >= 0 && (cli_pfds[worker_idx].revents & POLLIN)) {
            event_step_worker(worker_fd);

            if (!proc_is_daemon() && !proc_cli_turn_active()) {
                if (g_cli_done) goto done;
                cli_prompt();
            }
        }
        if (wake_idx >= 0 && (cli_pfds[wake_idx].revents & POLLIN)) {
            /* Sub-agent sessions woken by launch/completion advance here. */
            WakeMsg msg;
            while (read(wake_pipe_fd, &msg, sizeof(msg)) == (ssize_t)sizeof(msg)) {
                if (child_has_session(msg.session_id)) continue;
                run_advance(msg.session_id);
            }
            if (!proc_is_daemon() && !proc_cli_turn_active()) {
                if (g_cli_done) goto done;
                cli_prompt();
            }
        }
        if (tool_idx >= 0 && (cli_pfds[tool_idx].revents & POLLIN)) {
            event_step_tool_thread();
            if (!proc_is_daemon() && !proc_cli_turn_active()) {
                if (g_cli_done) goto done;
                cli_prompt();
            }
        }
        if (stdin_idx >= 0 && (cli_pfds[stdin_idx].revents & (POLLERR | POLLNVAL))) {
            /* stdin fd broken/closed — exit instead of spinning on POLLNVAL */
            LOG_ERROR_("stdin: poll error (revents=0x%x), exiting",
                       cli_pfds[stdin_idx].revents);
            goto done;
        }
        if (stdin_idx >= 0 && (cli_pfds[stdin_idx].revents & POLLIN)) {
            if (proc_cli_turn_active()) {
                LOG_DEBUG_("stdin: POLLIN but turn active, skipping");
                continue;
            }

            /* Read line directly (avoid stdio buffering issues with non-blocking fd) */
            static char linebuf[4096];
            static size_t linepos = 0;
            ssize_t n = read(STDIN_FILENO, linebuf + linepos, sizeof(linebuf) - linepos - 1);
            if (n <= 0) { if (n == 0) goto done; continue; } /* EOF or EAGAIN */
            linepos += (size_t)n;
            linebuf[linepos] = '\0';
            char *nl = strchr(linebuf, '\n');
            if (!nl) continue; /* partial line, wait for more */
            *nl = '\0';
            LOG_DEBUG_("stdin: read line [%s]", linebuf);

            if (!linebuf[0]) { linepos = 0; cli_prompt(); continue; }
            if (strcmp(linebuf, "exit") == 0 || strcmp(linebuf, "quit") == 0) goto done;

            /* Check if we're waiting for an approval decision — anywhere in
             * the CLI's session subtree, not just the root (a sub-agent's
             * request_config parks the same way and needs the same y/n). */
            {
                Approval *pa = approval_get_pending_subtree(proc_db(), proc_cli_session());
                if (pa) {
                    ApprovalDecision d;
                    if (strcasecmp(linebuf, "once") == 0 || strcasecmp(linebuf, "o") == 0)
                        d = APPROVAL_ONCE;
                    else if (strcasecmp(linebuf, "y") == 0 ||
                             strcasecmp(linebuf, "yes") == 0 ||
                             strcasecmp(linebuf, "ok") == 0 ||
                             strcasecmp(linebuf, "okay") == 0 ||
                             strcasecmp(linebuf, "approve") == 0)
                        d = APPROVAL_ALWAYS;
                    else
                        d = APPROVAL_DENY;
                    resolve_approval(pa->id, d, "cli:interactive", 0);
                    approval_free(pa);
                    /* Shift and continue */
                    size_t consumed = (size_t)(nl - linebuf) + 1;
                    linepos -= consumed;
                    if (linepos > 0) memmove(linebuf, nl + 1, linepos);
                    continue;
                }
            }

            cli_start_turn(linebuf);
            /* Shift any remaining data after the newline */
            size_t consumed = (size_t)(nl - linebuf) + 1;
            linepos -= consumed;
            if (linepos > 0) memmove(linebuf, nl + 1, linepos);
        }

        child_sweep_deadlines();

        /* DB periodic work — gated on interval */
        time_t now = time(NULL);
        if (now >= g_next_db_poll) {
            db_periodic();
            g_next_db_poll = now + POLL_DB_INTERVAL;
        }
    }

done:
    /* Print session cost */
    { int64_t cost = session_cost(proc_db(), proc_cli_session());
      if (cost > 0) fprintf(stderr, "\n[session cost: $%.6f]\n", (double)cost / 1e9); }

    /* A short -p run can exit before db_periodic's first tick, so parks that
     * are already past their deadline would otherwise be left 'pending' for
     * the next process to trip over. Sweep while the worker is still up, so a
     * resumed turn has somewhere to go; recovery below reclaims whatever it
     * leaves in flight. NOTE: the session is deliberately NOT forced idle here
     * — that would hide an unanswered awaiting_approval call from recovery. */
    approval_sweep_expired();
    /* Quiesce the worker pool before freeing anything it may still touch
     * (proc_tool_setup()/proc_cfg()/proc_db()). Mirrors the daemon shutdown order. */
    llm_worker_stop();
    tool_thread_stop();  /* drain in-flight tool threads before freeing state */
    agent_setup_destroy(&setup);
    child_sigchld_teardown();
    wake_close();
    free(cli_pfds);
    /* Drop our registry row, then run recovery: any still-transient sessions we
     * owned (e.g. -p exiting with background sub-agents in flight, or a turn cut
     * short) are now dead-owned and get reclaimed instead of orphaned. */
    process_unregister(proc_db(), proc_instance_id());
    db_recover_stale_sessions(proc_db());
    free(base_dir); config_free(proc_cfg()); db_close(proc_db()); free(db_path);
    return rc;
}

int main(int argc, char *argv[]) {
    /* --run-tool: early intercept for sandboxed file tool child. No DB, no key,
     * no config. The child reads its request from fd 3. */
    if (argc >= 2 && strcmp(argv[1], "--run-tool") == 0) return run_tool_main();

    /* sqlite3: the upstream shell, linked against the same amalgamation the
     * daemon uses. Deployment targets (Pogoplug, minimal containers) often
     * have no sqlite3 package, or one too old to read our JSONB columns.
     * Intercepted before DB/config init — it opens whatever file it is given,
     * including none. argv is handed over verbatim from argv[1]. */
    if (argc >= 2 && strcmp(argv[1], "sqlite3") == 0)
        return sqlite3_shell_main(argc - 1, argv + 1);

    /* --doctor: one-shot diagnostic bundle. Runs its own DB/config path so a
     * broken DB open is itself a finding — never bail on failures. */
    if (argc >= 2 && strcmp(argv[1], "--doctor") == 0) return doctor_main();

    /* install / uninstall: bare words (not flags) — the onboarding verb, not
     * a mode switch. No DB/config touched; writes go straight to the
     * filesystem + systemd. */
    if (argc >= 2 && strcmp(argv[1], "install") == 0) return install_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "uninstall") == 0) return uninstall_main(argc, argv);

    /* sensitive / secret-bind: operator verbs for the two escalation rules
     * (specs/trust.md). Deliberately CLI-only — no agent tool sets these. */
    if (argc >= 2 && strcmp(argv[1], "sensitive") == 0) return sensitive_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "secret-bind") == 0) return secret_bind_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "secret") == 0) return secret_main(argc, argv);

    /* route: operator verb binding a channel+chat to an agent (channel_routes).
     * CLI-only, same rationale as sensitive/secret-bind — an authority change. */
    if (argc >= 2 && strcmp(argv[1], "route") == 0) return route_main(argc, argv);

    /* rename-agent: operator identity surgery. CLI-only by design — no agent
     * tool renames anything (config-doc M2 deleted that arm). Takes a quiesce
     * lease so it works with the daemon running. */
    if (argc >= 2 && strcmp(argv[1], "rename-agent") == 0)
        return rename_agent_main(argc, argv);

    /* channel: operator verbs for the extension hot-swap flow (list/swap/
     * revert/restart). CLI-only — swapping channel code is an authority change. */
    if (argc >= 2 && strcmp(argv[1], "channel") == 0) return channel_cli_main(argc, argv);

    /* resp: read the llm_responses forensic archive ("[resp #N]" in error
     * messages). Read-only; exists because JSONB bodies need SQLite >= 3.45
     * and target boxes ship older system CLIs. */
    if (argc >= 2 && strcmp(argv[1], "backup") == 0) return backup_main(argc, argv);

    if (argc >= 2 && strcmp(argv[1], "resp") == 0) return resp_main(argc, argv);

    /* models: the provider availability probe (config-ax Phase 3). Read-only
     * from the config's point of view — it caches what the provider serves,
     * it never changes routing. */
    if (argc >= 2 && strcmp(argv[1], "models") == 0) return models_main(argc, argv);

    /* dashboard: print the tokenized /admin URL (token minted at daemon start). */
    if (argc >= 2 && strcmp(argv[1], "dashboard") == 0) return dashboard_main();

    int daemon_mode = 0, new_session = 0, host_mode = 0, auto_approve = 0;
    int channel_check = 0, channel_activate_flag = 0;
    const char *channel_mode = NULL;
    const char *channel_harness_scenario = NULL;
    LogLevel log_level_override = LOG_LEVEL_INFO;
    int log_level_set = 0;
    int64_t session_id = -1;
    const char *prompt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { print_usage(); return 0; }
        else if (strcmp(argv[i], "--version") == 0) {
            printf("cclaw %s (%s)\n", VERSION_COMMIT, BUILD_DATE);
            return 0;
        }
        else if (strcmp(argv[i], "--daemon") == 0) daemon_mode = 1;
        else if (strcmp(argv[i], "--channel") == 0) { if (++i >= argc) { fprintf(stderr, "--channel requires name\n"); return 1; } channel_mode = argv[i]; }
        else if (strcmp(argv[i], "--check") == 0) channel_check = 1;
        else if (strcmp(argv[i], "--activate") == 0) channel_activate_flag = 1;
        else if (strcmp(argv[i], "--harness") == 0) { if (++i >= argc) { fprintf(stderr, "--harness requires a scenario path\n"); return 1; } channel_harness_scenario = argv[i]; }
        else if (strncmp(argv[i], "--log-level=", 12) == 0) { log_level_override = log_level_parse(argv[i]+12); log_level_set = 1; }
        else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-v") == 0) { log_level_override = LOG_LEVEL_DEBUG; log_level_set = 1; }
        else if (strcmp(argv[i], "--trace") == 0 || strcmp(argv[i], "-vv") == 0) { log_level_override = LOG_LEVEL_TRACE; log_level_set = 1; }
        else if (strcmp(argv[i], "--new") == 0) new_session = 1;
        else if (strcmp(argv[i], "--trust-host") == 0) host_mode = 1;
        else if (strcmp(argv[i], "--auto-approve") == 0) auto_approve = 1;
        else if (strncmp(argv[i], "--llm-threads=", 14) == 0) g_llm_threads = atoi(argv[i]+14);
        else if (strcmp(argv[i], "-p") == 0) { if (++i >= argc) { fprintf(stderr, "-p requires arg\n"); return 1; } prompt = argv[i]; }
        else if (strcmp(argv[i], "-s") == 0) { if (++i >= argc) { fprintf(stderr, "-s requires arg\n"); return 1; } session_id = atoll(argv[i]); }
        else if (strncmp(argv[i], "--session-id=", 13) == 0) session_id = atoll(argv[i]+13);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 1; }
    }

    if (host_mode && daemon_mode)
        fprintf(stderr, "warning: --trust-host is ignored by --daemon; daemon agents "
                "sandbox per their sandbox_profile\n");

    if ((channel_check || channel_activate_flag || channel_harness_scenario) && !channel_mode) {
        fprintf(stderr, "--check / --activate / --harness require --channel <name>\n");
        return 1;
    }
    if ((channel_check ? 1 : 0) + (channel_activate_flag ? 1 : 0) + (channel_harness_scenario ? 1 : 0) > 1) {
        fprintf(stderr, "--check, --activate, and --harness are mutually exclusive\n");
        return 1;
    }

    {   const char *v = getenv("CCLAW_LLM_THREADS");
        if (v && atoi(v) > 0) g_llm_threads = atoi(v);
    }

    /* CLI tees logs to stderr so the interactive session sees them inline;
     * daemon and channel-runner children log strictly to syslog/journald. */
    int cli_tty = (!daemon_mode && channel_mode == NULL);
    cclaw_log_init(cli_tty);
    /* Provisional level so writes between here and the post-config
     * cclaw_log_set_level below aren't wide open (DB open, schema, seeding
     * all log). Refined once config resolves. */
    cclaw_log_set_level(log_level_set ? log_level_override
                                      : log_level_parse(getenv("CCLAW_LOG_LEVEL")));

    shutdown_init();
    crash_handler_init();

    /* ── Open DB ─────────────────────────────────────────────────── */
    db_configure_logging();   /* before the first db_open (sqlite3_initialize) */
    char *db_path = util_resolve_db_path();
    util_ensure_parent_dir(db_path);
    proc_set_db(db_open(db_path));
    if (!proc_db()) { fprintf(stderr, "cannot open DB: %s\n", db_path); free(db_path); return 1; }

    if (!db_schema_compat(proc_db())) {
        fprintf(stderr,
            "error: %s cannot be upgraded to schema v%d.\n"
            "The DB may be from a future build or too old. Delete it and restart:\n"
            "  rm %s %s-wal %s-shm\n",
            db_path, CCLAW_SCHEMA_VERSION, db_path, db_path, db_path);
        db_close(proc_db()); free(db_path); return 1;
    }

    /* Schema + seed in one exclusive transaction. When multiple processes
     * start concurrently (daemon + CLI), the first grabs the write lock and
     * does all DDL + seeding atomically; the others wait (busy handler) then
     * find everything already done (IF NOT EXISTS / COUNT>0 checks). */
    sqlite3_exec(proc_db(), "BEGIN EXCLUSIVE", NULL, NULL, NULL);
    if (db_ensure_schema(proc_db()) != 0) {
        sqlite3_exec(proc_db(), "ROLLBACK", NULL, NULL, NULL);
        db_close(proc_db()); free(db_path); return 1;
    }
    db_seed_defaults(proc_db());
    config_registry_sync(proc_db());
    sqlite3_exec(proc_db(), "COMMIT", NULL, NULL, NULL);

    { uint8_t sk[32]; if (secret_key_load_or_create(db_path, sk) == 0) db_set_secret_key(sk); }

    /* Reinstalls the shipped bundles on every start (files are a cache of
     * the binary's templates); idempotent against a current DB. */
    extension_install_builtin(proc_db(), db_path);

    proc_set_cfg(config_load(proc_db()));
    if (!proc_cfg()) { fprintf(stderr, "config load failed\n"); db_close(proc_db()); return 1; }
    if (log_level_set) {
        proc_cfg()->log_level = log_level_override;
        setenv("CCLAW_LOG_LEVEL", log_level_name(log_level_override), 1);
    } else if (cli_tty && !getenv("CCLAW_LOG_LEVEL")) {
        /* Interactive CLI defaults to errors-only on the tty so logs don't
         * clutter the conversation; -v/-vv or CCLAW_LOG_LEVEL opt back in.
         * (The daemon keeps the info default.) */
        proc_cfg()->log_level = LOG_LEVEL_ERROR;
    }
    cclaw_log_set_level(proc_cfg()->log_level);
    proc_set_log_level_env(log_level_name(proc_cfg()->log_level));
    proc_set_js_heap_env(config_get_int(proc_db(), "js_heap_mb"));

    /* Enable SQLite query profiling at debug level and above */
    db_set_slow_query_ms(config_get_int(proc_db(), "sql_slow_ms"));
    if (proc_cfg()->log_level >= LOG_LEVEL_DEBUG)
        db_enable_trace(proc_db());

    /* ── Channel lifecycle gate ──────────────────────────────────────
     * --activate (validated→active) is a pure DB transition, no JS touched —
     * reuses the already-open proc_db(). --check runs the same JS-load + onInit()
     * sequence the live runner does (via channel_runner_check), then this
     * process (not the runner) applies the draft/broken→validated transition
     * on success — keeping "did it pass" and "did we record that" separate. */
    if (channel_mode && channel_activate_flag) {
        int rc = channel_activate(proc_db(), channel_mode);
        if (rc == 0) {
            printf("channel '%s': validated -> active\n", channel_mode);
        } else {
            char *cur = channel_get_status(proc_db(), channel_mode);
            fprintf(stderr, "error: cannot activate '%s' (status=%s, need 'validated')\n",
                    channel_mode, cur ? cur : "unknown/missing");
            free(cur);
        }
        config_free(proc_cfg()); db_close(proc_db()); free(db_path);
        return rc == 0 ? 0 : 1;
    }
    if (channel_mode && channel_check) {
        config_free(proc_cfg()); db_close(proc_db());
        char *err = NULL;
        int rc = channel_runner_check(db_path, channel_mode, &err);
        if (rc == 0) {
            sqlite3 *cdb = db_open(db_path);
            int trc = cdb ? channel_mark_validated(cdb, channel_mode) : -1;
            if (cdb) db_close(cdb);
            if (trc == 0)
                printf("channel '%s': check passed, draft/broken -> validated\n", channel_mode);
            else
                fprintf(stderr, "channel '%s': check passed but the DB transition failed "
                                "(was it in 'draft' or 'broken'?)\n", channel_mode);
        } else {
            fprintf(stderr, "channel '%s': check FAILED: %s\n", channel_mode, err ? err : "?");
        }
        free(err);
        free(db_path);
        return rc == 0 ? 0 : 1;
    }
    if (channel_mode && channel_harness_scenario) {
        config_free(proc_cfg()); db_close(proc_db());
        int rc = channel_harness_run(db_path, channel_mode, channel_harness_scenario);
        free(db_path);
        return rc;
    }

    /* ── Channel mode ─────────────────────────────────────────────── */
    /* The daemon fork+execs `cclaw --channel <name>` (do_fork) for a clean
     * process image; we run the channel loop directly here — no separate
     * channel_runner binary, so ps shows `cclaw --channel <name>`. The runner
     * opens its own DB ctx, so drop ours first. */
    if (channel_mode) {
        config_free(proc_cfg()); db_close(proc_db());
        int rc = channel_runner_main(db_path, channel_mode);
        free(db_path);
        return rc;
    }

    /* ── Startup crash recovery (covers both CLI and daemon). GC dead registry
     * rows first so crashed predecessors are absent from the processes table,
     * making their sessions dead-owned and thus reclaimable. This process is not
     * yet registered, so it owns nothing — a live peer's row spares its own
     * sessions. Owner-scoped, so it's also safe on the periodic path. ── */
    process_gc_dead(proc_db(), PROCESS_TTL_SEC);
    if (db_recover_stale_sessions(proc_db()) != 0)
        LOG_WARN_("startup recovery failed");

    if (daemon_mode) return run_daemon(db_path);
    return run_cli(db_path, prompt, session_id, new_session, host_mode, auto_approve);
}
