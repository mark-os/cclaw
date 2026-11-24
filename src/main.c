#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
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
#include "contain.h"
#include "doctor.h"
#include "install.h"
#include "wake.h"
#include "advance.h"
#include "cache_expiring.h"
#include "settle.h"
#include "channel.h"
#include "channel_api.h"
#include "channel_runner.h"
#include "secret.h"
#include "validate.h"
#include "secret_scan.h"
#include "tool_policy.h"
#include "secret_interp.h"
#include "secret_capture.h"
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
#include "update.h"
#include "update_channel.h"
#include "util.h"

/* vendor/sqlite3/shell.c, compiled with -Dmain=sqlite3_shell_main */
int sqlite3_shell_main(int argc, char **argv);

/* Every connection the shell opens can answer cclaw_actor(), so a --write to
 * a logged table lands in change_log as actor "sqlite3" instead of failing
 * the way a plain system sqlite3 does. */
static int sqlite3_verb_identity(sqlite3 *db, char **err, const void *api) {
    (void)err; (void)api;
    return db_identity_register(db, "sqlite3") == 0 ? SQLITE_OK : SQLITE_ERROR;
}

/* Called by the shell (SQLITE_SHELL_INIT_PROC, see Makefile) after it has
 * made its sqlite3_config() calls and in place of its own initialize. */
void cclaw_sqlite3_shell_init(void);
void cclaw_sqlite3_shell_init(void) {
    sqlite3_initialize();
    sqlite3_auto_extension((void (*)(void))sqlite3_verb_identity);
}

/* `cclaw sqlite3` wrapper enforcing the two safety properties the bare
 * upstream shell lacks (design settled 2026-07-21, ruled 2026-08-29):
 *   - read-only by default; `--write` (ours, stripped) opts into writes.
 *   - invoked as root against another user's DB, drop to the DB owner first,
 *     so we never plant root-owned -wal/-shm that lock the daemon out. */
static int sqlite3_verb_main(int argc, char **argv) {
    int write_mode = 0;
    char **out = malloc((size_t)(argc + 2) * sizeof(char *));
    if (!out) return 1;
    int outc = 0;
    out[outc++] = argv[0];
    const char *dbfile = NULL;
    /* Upstream options whose value could be mistaken for the DB filename. */
    static const char *valopts[] = { "-init", "-cmd", "-vfs", "-nullvalue",
        "-separator", "-colseparator", "-rowseparator", "-newline",
        "-mmap", "-maxsize", NULL };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--write") == 0) { write_mode = 1; continue; }
        if (!dbfile && argv[i][0] != '-') {
            dbfile = argv[i];
        } else if (argv[i][0] == '-') {
            const char *z = argv[i][1] == '-' ? argv[i] + 1 : argv[i];
            for (int k = 0; valopts[k]; k++)
                if (strcmp(z, valopts[k]) == 0 && i + 1 < argc) {
                    out[outc++] = argv[i++];
                    break;
                }
        }
        out[outc++] = argv[i];
    }
    out[outc] = NULL;
    if (!write_mode) {
        /* Inject right after argv[0]: options must precede the filename. */
        memmove(out + 2, out + 1, (size_t)outc * sizeof(char *));
        out[1] = (char *)"-readonly";
        outc++;
    }
    if (dbfile && geteuid() == 0) {
        struct stat st;
        if (stat(dbfile, &st) == 0 && st.st_uid != 0) {
            gid_t g = st.st_gid;
            if (setgroups(1, &g) != 0 ||
                setgid(st.st_gid) != 0 || setuid(st.st_uid) != 0) {
                fprintf(stderr, "cclaw sqlite3: cannot drop to uid %d "
                        "(owner of %s): %s\n",
                        (int)st.st_uid, dbfile, strerror(errno));
                free(out);
                return 1;
            }
        }
    }
    int rc = sqlite3_shell_main(outc, out);
    free(out);
    return rc;
}

/* Saved at startup for SIGUSR2 re-exec (see reexec_self). Captured *early* and
 * on purpose: `cclaw update` replaces the binary by rename(), after which
 * /proc/self/exe reads "<path> (deleted)" and still resolves to the old inode —
 * exec'ing it would faithfully relaunch the code we are trying to replace. The
 * path read here, before any replacement, is the one that opens the new file. */
static char g_exe_path[PATH_MAX];
static char **g_exe_argv;

/* Re-exec into the binary now at g_exe_path, keeping our pid. Only returns on
 * failure, and the caller must then exit NON-ZERO: a supervisor reads 0 as
 * "stopped on purpose" and leaves the daemon down (systemd Restart=on-failure
 * and an init script's respawn loop both do). */
static void reexec_self(void) {
    if (!g_exe_path[0] || !g_exe_argv) return;
    LOG_INFO_("re-exec: %s", g_exe_path);
    fflush(NULL);
    execv(g_exe_path, g_exe_argv);
    LOG_ERROR_("re-exec failed: %s (exiting instead)", strerror(errno));
}

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
        /* The CacheExpiring timer lives in the sessions table: wake when the
         * next idle session falls due, not up to a poll interval late — a
         * 5-minute cache leaves 30 seconds between due and lapsed. */
        time_t ce = cache_expiring_next(proc_db(), now);
        if (ce > 0 && ce < nearest)
            nearest = ce;
    }

    /* Tier 2: periodic DB poll */
    if (g_next_db_poll < nearest)
        nearest = g_next_db_poll;

    int ms = (int)((nearest - now) * 1000);
    return ms < 0 ? 0 : ms;
}

/* ── event_step: shared event handling for both daemon and CLI loops ── */

static void event_step_worker(int worker_fd) {
    (void)worker_fd;
    int64_t completed_sid;
    while (llm_worker_read(&completed_sid) == 0) {
        if (completed_sid == -1) continue;
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
        /* The default agent has no prompt of its own: the built-in layer
         * (config key system_prompt) renders for every agent, and identity
         * lives in the memory blocks seeded below. */
        sqlite3_exec(proc_db(),
            "INSERT OR IGNORE INTO agents(name, containment)"
            " VALUES('Assistant', 'standard');", NULL, NULL, NULL);
        /* One-row routing list: the seeded bootstrap model. Explicit — an
         * agent with no list is unroutable by design (model-routing.md R2). */
        sqlite3_exec(proc_db(),
            "INSERT OR IGNORE INTO agent_models(agent_name, model_id, pos)"
            " SELECT 'Assistant', 'openrouter/deepseek/deepseek-v4.1-flash', 0"
            " WHERE EXISTS (SELECT 1 FROM models"
            "               WHERE id='openrouter/deepseek/deepseek-v4.1-flash');",
            NULL, NULL, NULL);
        /* Seed default tools as grants — plus the operator-deputy extras the
         * baseline deliberately lacks (Livermore haircut: created agents get
         * no birthright agent-minting / global publishing / credential
         * minting). The FIRST agent is the operator's interface; proposing a
         * create/publish still parks an approval, which is the actual gate. */
        agent_grant_defaults(proc_db(), "Assistant", GRANT_ORIGIN("seed", 0));
        agent_config_grant(proc_db(), "Assistant", "tool", "create_agent", 0,
                           GRANT_ORIGIN("seed", 0));
        agent_config_grant(proc_db(), "Assistant", "tool", "extension_publish", 0,
                           GRANT_ORIGIN("seed", 0));
        agent_config_grant(proc_db(), "Assistant", "tool", "secret_create", 0,
                           GRANT_ORIGIN("seed", 0));
        agent_config_grant(proc_db(), "Assistant", "tool", "install_service", 0,
                           GRANT_ORIGIN("seed", 0));
        /* The default extensions every new agent gets — the bundled
         * compaction among them, installed just before this runs. */
        agent_attach_default_extensions(proc_db(), "Assistant");
        /* Disabled 'heartbeat' bare-wake job — visible/enable-able in cron_list. */
        cron_seed_heartbeat(proc_db(), "Assistant");
        /* default_agent needs no write — 'Assistant' is the registry default */
        /* Seed default memory blocks. Explicitly 'system' placement: identity
         * (who am I / who is the user) belongs in the system prompt, where it
         * changes rarely and stays in the cached prefix — unlike new blocks,
         * which default to the per-turn 'context' placement. */
        int64_t agent_block = memory_block_create(proc_db(), "Assistant", "AGENT",
            "Your identity, capabilities, and operational notes: facts that change what you do next time, not a record of what happened. Correct a note when it goes out of date; delete it when it stops mattering.",
            5000, "system");
        memory_block_create(proc_db(), "Assistant", "USER",
            "Who you work for: name, timezone, preferences, how they like to work. Learn it sideways, through the work — not a questionnaire. If they would be annoyed to repeat it later, keep it. A note changes your next turn, not this one: you are writing for your future self.",
            5000, "system");
        /* Onboarding is notes that ask to be deleted: when none is left it is
         * over, and no C tracks it (plan/projects/onboarding.md). They sit in
         * AGENT rather than a block of their own because an agent cannot
         * delete a block — an emptied one would ride the prompt for good. The
         * first note is the rule for the rest, so the base prompt says
         * nothing about a first run and the rule leaves with the checklist. */
        if (agent_block > 0) {
            static const char *const notes[] = {
                "Setup: the notes here that begin \"Setup:\" are a first-run checklist, a ritual and never a gate. The person's request comes first; take one when the conversation has room. Delete a note when it is done, or when the person says skip or later — that is an answer, not a fact to record. Delete this note when no other is left.",
                "Setup: you have no name yet. Introduce yourself as CClaw and ask what they would like to call you — they choose; do not suggest a name. Then replace this note with your name and one line on the character you will have.",
                "Setup: learn what to call the person and their timezone. Save to USER.",
                "Setup: offer a channel (Telegram, Discord or WhatsApp) so they can reach you from a phone — read the cclaw-channels skill first, and call it done only when a message sent through the channel has arrived. Offer to keep running after the terminal closes: install_service does it on this machine once they approve; another machine is the installing-cclaw skill.",
                "Setup: call search_config once and tell them plainly about anything that will get in the way (no sandbox on this host, a missing key).",
            };
            for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++)
                memory_note_add(proc_db(), agent_block, notes[i]);
        }
    }
    if (al) { for (int i = 0; i < ac; i++) free(al[i]); free(al); }
}

/* An agent whose containment this host cannot keep has its tool calls
 * refused at dispatch. That is the right refusal and a bad first impression:
 * say so at startup, naming a shipped containment this host can keep,
 * instead of leaving the operator to meet it one refused call at a time.
 * `agent` NULL = every agent (the daemon). Nothing is changed here — the
 * containment stays the operator's explicit choice. */
static void warn_if_uncontained(sqlite3 *db, const char *agent, int daemon) {
    const char *env = daemon ? NULL : getenv("CCLAW_CONTAINMENT");
    char why[160], hint[320], who[256] = "";
    unsigned caps = contain_caps(why, sizeof(why));
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT name, COALESCE(containment,'standard') FROM agents"
            " WHERE ?1 IS NULL OR name = ?1 ORDER BY name", -1, &st, NULL) != SQLITE_OK)
        return;
    if (agent) sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Containment c = {0};
        containment_load(db, env ? env : (const char *)sqlite3_column_text(st, 1), &c);
        if (contain_keepable(&c, caps)) continue;
        size_t n = strlen(who);
        snprintf(who + n, sizeof(who) - n, "%s%s (%s%s)", n ? ", " : "",
                 (const char *)sqlite3_column_text(st, 0), c.name,
                 c.known ? "" : ", no such row");
    }
    sqlite3_finalize(st);
    if (!who[0]) return;
    const char *alt = contain_suggest(caps);
    contain_hint(hint, sizeof(hint));
    if (!why[0]) hint[0] = '\0';
    if (daemon) {
        /* The daemon's log is syslog: one line. */
        LOG_ERROR_("daemon: this host cannot keep the containment of %s (%s%s%s) —"
                   " tool calls it cannot keep are refused. %s%sA shipped"
                   " containment this host can keep: '%s'; `cclaw --doctor` has more",
                   who, contain_backend(), why[0] ? ": " : "", why,
                   hint, hint[0] ? ". " : "", alt ? alt : "none");
        return;
    }
    fprintf(stderr,
            "warning: this host cannot keep the containment of %s (%s%s%s).\n"
            "  Tool calls it cannot keep will be refused.\n"
            "%s%s%s"
            "  A shipped containment this host can keep: '%s'%s.\n"
            "  `cclaw --doctor` shows what each tool gets.\n",
            who, contain_backend(), why[0] ? ": " : "", why,
            hint[0] ? "  " : "", hint, hint[0] ? "\n" : "",
            alt ? alt : "none",
            alt && strcmp(alt, "none") == 0 ? " (--no-containment)" : "");
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

    printf("cclaw %s (%s, %s) — daemon mode\n", VERSION, VERSION_COMMIT, BUILD_DATE);
    /* Resolved config, once, at default verbosity: a daemon that talks to the
     * wrong provider or writes to the wrong workspace should say so in the log
     * rather than in a support thread. The key is named by *source* only —
     * never any part of its value. */
    LOG_INFO_("daemon: config: provider=%s model=%s endpoint=%s wire=%s"
              " workspace=%s db=%s api_key=%s",
              proc_cfg()->provider.name ? proc_cfg()->provider.name : "(builtin)",
              proc_cfg()->provider.model ? proc_cfg()->provider.model : "(unset)",
              proc_cfg()->provider.base_url ? proc_cfg()->provider.base_url : "(unset)",
              endpoint_type_name(proc_cfg()->provider.endpoint_type),
              proc_cfg()->workspace ? proc_cfg()->workspace : "(unset)",
              proc_cfg()->db_path ? proc_cfg()->db_path : "(unset)",
              proc_cfg()->provider.api_key_source ? proc_cfg()->provider.api_key_source
                                             : "(none)");
    warn_if_uncontained(proc_db(), NULL, 1);
    /* A daemon with no key still starts (channels and the admin page are how
     * the operator fixes it), but it must not look healthy while every turn
     * fails. */
    if (!proc_cfg()->provider.api_key || !proc_cfg()->provider.api_key[0])
        LOG_ERROR_("daemon: no API key for any provider — every turn will fail."
                   " Put OPENROUTER_API_KEY (or another provider's key) in the"
                   " daemon's environment and restart; `cclaw --doctor` shows"
                   " which keys were found");
    proc_set_web_up(web_start(proc_cfg(), db_path) == 0);
    if (!proc_web_up())
        LOG_ERROR_("daemon: web server did not start on port %d (already in use,"
                   " or a bad web_bind) — /admin, shared files and webhooks are"
                   " unavailable", proc_cfg()->web_port);
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

    /* From here the daemon takes work: an update that fails after this point
     * is never undone by restoring the database. */
    update_verify_serving(proc_db());

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
                /* Session 0 is "look at the queues", not a session to advance
                 * (the dashboard, after recording an approval decision). */
                if (msg.session_id == 0) continue;
                if (child_has_session(msg.session_id)) continue;
                run_advance(msg.session_id);
            }
            approval_apply_queued();
        }
        if (fifo_idx >= 0 && (pfds[fifo_idx].revents & POLLIN)) {
            char drain[64];
            while (read(fifo_fd, drain, sizeof(drain)) > 0) {}
            channel_consume_events(proc_db());
            /* `cclaw approvals` nudges the same FIFO after queueing a
             * decision, so it is carried out now and not at the next poll. */
            approval_apply_queued();
        }
        if (pfds[d_worker_idx].revents & POLLIN)
            event_step_worker(daemon_worker_fd);
        if (pfds[d_tool_idx].revents & POLLIN)
            event_step_tool_thread();

        child_sweep_deadlines();
        channel_tick(proc_db());
        cache_expiring_tick();

        /* DB periodic work — gated on interval */
        time_t now = time(NULL);
        if (now >= g_next_db_poll) {
            db_periodic();
            /* Daemon-only: notices a newer release and drops a note in the
             * default agent's inbox. Self-gating and off by default. */
            update_check_tick(proc_db());
            g_next_db_poll = now + POLL_DB_INTERVAL;
        }
    }

    free(pfds);

    /* Shutdown */
    llm_worker_stop();
    tool_thread_stop();  /* drain in-flight tool threads before freeing state */
    channel_shutdown_all();
    web_stop();
    proc_set_web_up(0);
    child_sigchld_teardown();
    wake_close(); wake_fifo_close(fifo_fd, db_path);
    update_verify_clean_exit(proc_db());
    process_unregister(proc_db(), proc_instance_id());
    cron_set_script_runner(NULL);
    proc_set_tool_setup(NULL);
    agent_setup_destroy(&daemon_setup);
    config_free(proc_cfg()); db_close(proc_db());

    /* The database is closed and nothing of ours holds it: the moment a
     * pending `cclaw rollback` is allowed to swap it. Any shutdown, not just a
     * re-exec — a supervisor restart applies it just the same. */
    {
        char msg[8192];
        int rrc = update_rollback_apply(db_path, msg, sizeof(msg));
        if (rrc == 1) LOG_INFO_("%s", msg);
        else if (rrc < 0) LOG_ERROR_("%s", msg);
    }
    free(db_path);

    /* Last thing, after every subsystem is down and the DB is closed: become
     * the new binary. Descriptors survive exec, so this must come after the
     * teardown above, not instead of it. */
    if (shutdown_reexec_requested()) { reexec_self(); return 1; }
    return 0;
}

/* `cclaw -p`: is the one turn it came for finished? Its reply is printed when
 * the turn goes inactive, but an awaited Stop or ContextThreshold hook may
 * still hold the session — leaving then would abandon the hold to recovery
 * and a scripted run would never see what the daemon does with the same
 * turn. The hold is bounded by the hook's deadline (config hook_timeout or
 * the hook's own "timeout"). */
static int cli_single_turn_over(void) {
    return g_cli_done && !child_hook_holds(proc_cli_session());
}

/* Which provider a pasted key belongs to, by the prefix its issuer gives it.
 * Longest prefixes first: "sk-" alone is OpenAI's, and a prefix of three
 * others. NULL = not a shape we know. */
static const char *key_env_for(const char *key) {
    static const struct { const char *prefix, *env; } kinds[] = {
        { "sk-or-",  "OPENROUTER_API_KEY" },
        { "sk-ant-", "ANTHROPIC_API_KEY" },
        { "AIza",    "GEMINI_API_KEY" },
        { "gsk_",    "GROQ_API_KEY" },
        { "csk-",    "CEREBRAS_API_KEY" },
        { "sk-",     "OPENAI_API_KEY" },
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (strncmp(key, kinds[i].prefix, strlen(kinds[i].prefix)) == 0)
            return kinds[i].env;
    return NULL;
}

/* First run at a terminal with no key anywhere: ask for one, once, and keep
 * it encrypted in the DB (scope system — the same row a provider's
 * api_key_env resolves from), so neither this CLI nor a daemon installed
 * later needs an env file. The key is read with echo off and never printed.
 * On any failure nothing is stored and the caller's "no API key" error
 * stands. */
static void first_run_key_prompt(void) {
    fprintf(stderr,
            "cclaw needs a key for one LLM provider, and none was found.\n"
            "  Easiest: OpenRouter — create a key at https://openrouter.ai/keys\n"
            "  (an Anthropic, OpenAI, Gemini, Groq or Cerebras key works too).\n"
            "Paste the key and press Enter (it will not be shown; empty = quit): ");
    struct termios old, quiet;
    int have_tty = tcgetattr(STDIN_FILENO, &old) == 0;
    if (have_tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    char key[512];
    char *got = fgets(key, sizeof(key), stdin);
    if (have_tty) tcsetattr(STDIN_FILENO, TCSANOW, &old);
    fputc('\n', stderr);
    if (!got) return;
    size_t n = strlen(key);
    while (n && (key[n-1] == '\n' || key[n-1] == '\r' || key[n-1] == ' ')) key[--n] = '\0';
    char *k = key;
    while (*k == ' ') k++;
    if (!*k) return;
    const char *env = key_env_for(k);
    if (!env) {
        fprintf(stderr, "That does not look like a key from a provider cclaw knows.\n"
                        "Store it by name instead: cclaw secret set <PROVIDER>_API_KEY\n");
        return;
    }
    if (db_secret_set(proc_db(), env, k, "operator", "system") != 0) {
        fprintf(stderr, "Could not store the key (is %s writable?).\n", "~/.cclaw");
        return;
    }
    memset(key, 0, sizeof(key));
    Config *fresh = config_load(proc_db());
    if (!fresh) return;
    fresh->log_level = proc_cfg()->log_level;
    config_free(proc_cfg());
    proc_set_cfg(fresh);
    fprintf(stderr, "Stored encrypted as %s. Remove it with: cclaw secret rm %s\n\n",
            env, env);
}

static int run_cli(char *db_path, const char *prompt,
                   int64_t session_id, int new_session, int no_containment, int auto_approve) {
    /* ── CLI mode ────────────────────────────────────────────────── */
    proc_set_daemon(0);
    proc_set_auto_approve(auto_approve);
    g_next_db_poll = time(NULL);

    if ((!proc_cfg()->provider.api_key || !proc_cfg()->provider.api_key[0]) &&
        isatty(STDIN_FILENO) && isatty(STDERR_FILENO))
        first_run_key_prompt();
    if (!proc_cfg()->provider.api_key || !proc_cfg()->provider.api_key[0]) {
        fprintf(stderr,
                "error: no API key\n"
                "  cclaw needs a key for one LLM provider. The default is OpenRouter\n"
                "  (https://openrouter.ai/keys):\n"
                "    export OPENROUTER_API_KEY=sk-or-v1-...\n"
                "  ANTHROPIC_API_KEY, OPENAI_API_KEY, GEMINI_API_KEY and others work\n"
                "  the same way. `cclaw --doctor` shows which keys were found.\n");
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
    free(agent_sel);

    if (no_containment) setenv("CCLAW_CONTAINMENT", "none", 1);
    warn_if_uncontained(proc_db(), proc_agent_name(), 0);
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
    if (session_id < 0) {
        /* Usually the sessions.agent_name foreign key: the agent has no row. */
        fprintf(stderr, "error: cannot open a session for agent '%s': %s\n",
                proc_agent_name(), sqlite3_errmsg(proc_db()));
        agent_setup_destroy(&setup); free(base_dir); config_free(proc_cfg()); db_close(proc_db()); free(db_path); return 1; }
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

    /* Decisions recorded while no event loop was running (`cclaw approvals`
     * between two -p runs) are carried out now, so the turn about to start
     * already sees them: the grant is in place and the agent has been told.
     * Otherwise they would wait for db_periodic's first tick, after it. */
    approval_apply_queued();

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
                if (cli_single_turn_over()) goto done;
                if (!g_cli_done) cli_prompt();
            }
        }
        if (worker_idx >= 0 && (cli_pfds[worker_idx].revents & POLLIN)) {
            event_step_worker(worker_fd);

            if (!proc_is_daemon() && !proc_cli_turn_active()) {
                if (cli_single_turn_over()) goto done;
                if (!g_cli_done) cli_prompt();
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
                if (cli_single_turn_over()) goto done;
                if (!g_cli_done) cli_prompt();
            }
        }
        if (tool_idx >= 0 && (cli_pfds[tool_idx].revents & POLLIN)) {
            event_step_tool_thread();
            if (!proc_is_daemon() && !proc_cli_turn_active()) {
                if (cli_single_turn_over()) goto done;
                if (!g_cli_done) cli_prompt();
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
                Approval *pa = approval_get_parked_subtree(proc_db(), proc_cli_session());
                if (pa) {
                    /* Only an explicit answer decides, and `y` is the least
                     * that works (approval_answer_fit — the rule every
                     * surface shares). Anything else re-asks: a stray line is
                     * most often the next message, typed without noticing
                     * the prompt. */
                    ApprovalShape shape = approval_shape(pa);
                    ApprovalDecision d;
                    if (approval_answer_fit(shape, approval_answer_from_word(linebuf),
                                            &d, NULL) != 0) {
                        fprintf(stdout, "Not an answer, and not sent as a message. %s\n%s",
                                approval_shape_choices(shape), approval_shape_prompt(shape));
                        fflush(stdout);
                    } else {
                        resolve_approval(pa->id, d, "cli:interactive", 0);
                    }
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
            /* The tick can end a hold no event announced (turn_settle_sweep). */
            if (!proc_cli_turn_active() && cli_single_turn_over()) goto done;
        }
    }

done:
    /* A run with nobody to ask left requests open: say how to answer them,
     * and exit 3 so a script can tell "finished" from "finished, waiting on
     * you". stderr — stdout is the reply alone. */
    if (approval_left_pending() > 0) {
        fprintf(stderr, "\n%d approval(s) pending. See them: cclaw approvals\n"
                        "Answer:   cclaw approvals approve <id> [once|always] | deny <id>\n"
                        "Continue: cclaw -p \"...\" -s %lld\n",
                approval_left_pending(), (long long)proc_cli_session());
        rc = 3;
    }
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
    g_exe_argv = argv;
    { ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path) - 1);
      if (n > 0) g_exe_path[n] = '\0'; else g_exe_path[0] = '\0'; }

    /* --run-tool: early intercept for every sandboxed tool child. No DB, no key,
     * no config. The child reads its request from fd 3. */
    if (argc >= 2 && strcmp(argv[1], "--run-tool") == 0) return run_tool_main();

    /* sqlite3: the upstream shell, linked against the same amalgamation the
     * daemon uses. Deployment targets (Pogoplug, minimal containers) often
     * have no sqlite3 package, or one too old to read our JSONB columns.
     * Intercepted before DB/config init — it opens whatever file it is given,
     * including none. Read-only unless --write; drops root to the DB owner. */
    if (argc >= 2 && strcmp(argv[1], "sqlite3") == 0)
        return sqlite3_verb_main(argc - 1, argv + 1);

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

    /* route: operator verb binding a channel+chat to an agent (channel_chats).
     * CLI-only, same rationale as sensitive/secret-bind — an authority change. */
    if (argc >= 2 && strcmp(argv[1], "route") == 0) return route_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "approvals") == 0) return approvals_main(argc, argv);

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

    /* update: replace this binary with a newer tagged release. Own DB open,
     * like the other verbs — it must work with the daemon up or down. */
    if (argc >= 2 && strcmp(argv[1], "update") == 0) return update_main(argc, argv);
    /* rollback: undo the last update — snapshot back as the database, .prev
     * back as the binary. Same "own DB open, daemon up or down" shape. */
    if (argc >= 2 && strcmp(argv[1], "rollback") == 0) return rollback_main(argc, argv);
    /* publish: sign this build into a pre-release update channel. */
    if (argc >= 2 && strcmp(argv[1], "publish") == 0) return publish_main(argc, argv);

    if (argc >= 2 && strcmp(argv[1], "resp") == 0) return resp_main(argc, argv);

    /* models: the provider availability probe (config-ax Phase 3). Read-only
     * from the config's point of view — it caches what the provider serves,
     * it never changes routing. */
    if (argc >= 2 && strcmp(argv[1], "models") == 0) return models_main(argc, argv);

    /* dashboard: print the tokenized /admin URL (token minted at daemon start). */
    if (argc >= 2 && strcmp(argv[1], "dashboard") == 0) return dashboard_main();

    int daemon_mode = 0, new_session = 0, no_containment = 0, auto_approve = 0;
    int channel_check = 0, channel_activate_flag = 0;
    const char *channel_mode = NULL;
    const char *channel_harness_scenario = NULL;
    LogLevel log_level_override = LOG_LEVEL_INFO;
    int log_level_set = 0;
    int64_t session_id = -1;
    const char *prompt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { print_usage(); return 0; }
        /* `cclaw publish` reads the commit out of this line (update_channel.c). */
        else if (strcmp(argv[i], "--version") == 0 ||
                 (i == 1 && strcmp(argv[i], "version") == 0)) {
            printf("cclaw %s (%s, %s)\n", VERSION, VERSION_COMMIT, BUILD_DATE);
            return 0;
        }
        /* Machine-readable compatibility handshake. `cclaw update` runs this
         * on a *downloaded* binary before installing it, so the format is a
         * contract with future builds: one line, two key=value pairs. */
        else if (strcmp(argv[i], "--schema-range") == 0) {
            printf("min=%d current=%d\n", CCLAW_SCHEMA_MIN, CCLAW_SCHEMA_VERSION);
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
        else if (strcmp(argv[i], "--no-containment") == 0) no_containment = 1;
        else if (strcmp(argv[i], "--auto-approve") == 0) auto_approve = 1;
        else if (strncmp(argv[i], "--llm-threads=", 14) == 0) g_llm_threads = atoi(argv[i]+14);
        else if (strcmp(argv[i], "-p") == 0) { if (++i >= argc) { fprintf(stderr, "-p requires arg\n"); return 1; } prompt = argv[i]; }
        else if (strcmp(argv[i], "-s") == 0) { if (++i >= argc) { fprintf(stderr, "-s requires arg\n"); return 1; } session_id = atoll(argv[i]); }
        else if (strncmp(argv[i], "--session-id=", 13) == 0) session_id = atoll(argv[i]+13);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 1; }
    }

    if (no_containment && daemon_mode)
        fprintf(stderr, "warning: --no-containment is ignored by --daemon; daemon "
                "agents are contained per their containment setting\n");

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

    /* A pending `cclaw rollback` is applied here, before this process opens
     * the database: the swap needs the file closed, and the build that knows
     * the protocol is this one. Success means the binary under us changed —
     * re-exec so the restored build is what actually runs. */
    if (daemon_mode) {
        char msg[8192];
        int rrc = update_rollback_apply(db_path, msg, sizeof(msg));
        if (rrc == 1) { LOG_INFO_("%s", msg); free(db_path); reexec_self(); return 1; }
        if (rrc < 0) LOG_ERROR_("%s", msg);
    }

    proc_set_db(db_open(db_path));
    if (!proc_db()) { fprintf(stderr, "cannot open DB: %s\n", db_path); free(db_path); return 1; }
    db_set_actor(proc_db(), daemon_mode ? "daemon" : "cli");

    /* Post-update crash-loop guard: count this start against an armed
     * update.verify marker BEFORE db_schema_compat below, which is where the
     * schema patches actually run. A build that dies inside its own patch —
     * or whose patch fails, which exits here every time — would otherwise
     * never be counted, and that is the failure the .prev build alone
     * recovers from, because the database has not moved yet. A build that
     * dies after migrating takes the snapshot back with it. Either way:
     * re-exec into the restored build with nothing running and the DB closed. */
    int undo = daemon_mode ? update_verify_startup(proc_db()) : 0;
    if (undo) {
        db_close(proc_db());
        if (undo == 2) update_verify_rollback(db_path);
        free(db_path);
        reexec_self();   /* returns only if exec itself failed */
        return 1;
    }

    if (!db_schema_compat(proc_db())) {
        fprintf(stderr,
            "error: %s cannot be upgraded to schema v%d.\n"
            "A DB from a newer build needs that build. A DB older than v%d\n"
            "migrates by a hop: run the v0.8.0 release on it once (that patches\n"
            "the DB to v70), then this build. Or start over:\n"
            "  rm %s %s-wal %s-shm\n",
            db_path, CCLAW_SCHEMA_VERSION, CCLAW_SCHEMA_MIN,
            db_path, db_path, db_path);
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
     * the binary's templates); idempotent against a current DB. A channel
     * runner skips it: the daemon that forked it has just done this, and N
     * runners repeating it only contend for the same files and write lock. */
    if (!channel_mode) extension_install_builtin(proc_db(), db_path);

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
            if (cdb) db_set_actor(cdb, "cli");
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
    return run_cli(db_path, prompt, session_id, new_session, no_containment, auto_approve);
}
