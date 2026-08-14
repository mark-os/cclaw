#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"

#include "db.h"
#include "loop.h"
#include "proc.h"

/* ── CLI terminal output ─────────────────────────────────────────────
 * The CLI runs in the terminal's canonical mode: the kernel echoes input
 * and owns line editing, so the prompt below is never deletable and we
 * never enter raw mode. Output here is the program's UI (stdout), distinct
 * from diagnostics (syslog via LOG_*). */

#define CLI_PROMPT "> "   /* set to "" to drop the prompt symbol */

/* ANSI only when stdout is a real terminal — piped / -p output stays clean. */
static int cli_ansi(void) {
    static int v = -1;
    if (v < 0) v = isatty(STDOUT_FILENO) ? 1 : 0;
    return v;
}

void cli_prompt(void) {
    fputs(CLI_PROMPT, stdout);
    fflush(stdout);
}

/* Transient "working" cue shown between the user's Enter and the turn's first
 * output. The response isn't streamed, so a tool-less turn would otherwise
 * show nothing while the model runs; the first real output clears it. */
static int g_cli_indicator;
static void cli_indicator_show(void) {
    if (!cli_ansi()) return;
    fputs("\033[2m…\033[0m", stdout);
    fflush(stdout);
    g_cli_indicator = 1;
}
void cli_indicator_clear(void) {
    if (g_cli_indicator && cli_ansi()) { fputs("\r\033[K", stdout); fflush(stdout); }
    g_cli_indicator = 0;
}

/* CLI progress: "[tool_name {"arg":"value"}]" dimmed, args truncated */
void cli_print_tool_call(const char *name, const char *args) {
    cli_indicator_clear();
    fprintf(stdout, "\n\033[2m[%s", name);
    if (args && args[0] && strcmp(args, "{}") != 0) {
        if (strlen(args) <= 200) fprintf(stdout, " %s", args);
        else fprintf(stdout, " %.197s...", args);
    }
    fprintf(stdout, "]\033[0m ");
    fflush(stdout);
}

void print_usage(void) {
    printf("usage: cclaw [options]\n"
           "       cclaw install [--system]    set up a long-lived daemon (systemd unit)\n"
           "       cclaw uninstall [--system]  stop + remove that unit\n"
           "       cclaw sensitive add|rm <host> | list\n"
           "                                   label a target sensitive: every tool call\n"
           "                                   touching it parks for approval, no standing\n"
           "                                   grant applies, and the proxy denies it ambiently\n"
           "       cclaw secret-bind <name> <host> | rm <name> <host> | list\n"
           "                                   bind a secret to its legitimate hosts; a call\n"
           "                                   submitting the secret elsewhere parks, and a\n"
           "                                   secret-carrying shell/js call can reach ONLY\n"
           "                                   its bound hosts\n"
           "       cclaw secret set <NAME> [value] | rm <NAME> | list\n"
           "                                   mint/remove a DB-backed secret (no value arg\n"
           "                                   reads one line from stdin); born with zero host\n"
           "                                   bindings, so its first use always parks\n"
           "       cclaw route add <channel> <chat_id> <agent> | rm <channel> <chat_id> | list\n"
           "                                   bind a channel+chat to an agent; chat_id '*' is\n"
           "                                   the channel-wide default (else falls back to\n"
           "                                   default_agent)\n"
           "       cclaw channel [list] | swap <channel> <ext> | revert <channel> | restart <channel>\n"
           "                                   hot-swap the extension behind a live channel\n"
           "                                   (previous kept as revert target; a swap that\n"
           "                                   crash-loops auto-reverts and notifies admins)\n"
           "       cclaw dashboard             print the tokenized admin dashboard URL\n"
           "                                   (token minted on first daemon start)\n"
           "       cclaw backup [dest]         VACUUM INTO snapshot (default\n"
           "                                   <db>.backup.<timestamp>); safe on a live\n"
           "                                   daemon, refuses to overwrite\n"
           "       cclaw resp [<id> [req] | list [n]]\n"
           "                                   read the raw LLM response archive; bare form\n"
           "                                   shows the last failure (what \"[resp #N]\" in an\n"
           "                                   error message cites)\n"
           "       cclaw models [query] [--provider NAME] [--page N] [--refresh]\n"
           "                                   models a provider's catalog lists (cached\n"
           "                                   12h); listed, not registered — routing still\n"
           "                                   uses the `models` rows\n"
           "\n"
           "modes (default: interactive CLI):\n"
           "  --daemon           run as daemon (telegram, web, cron)\n"
           "  --channel <name>   run one channel's event loop in this process\n"
           "                     (normally fork+exec'd by the daemon, not run by hand)\n"
           "  --channel <name> --check     validate: manifest + JS load + onInit(),\n"
           "                                no event loop; draft/broken -> validated on pass\n"
           "  --channel <name> --activate  validated -> active (daemon execs it next tick)\n"
           "  --channel <name> --harness <scenario.json>  offline fixture-replay test\n"
           "                                against a scratch DB (no real network/DB touched)\n"
           "\n"
           "options:\n"
           "  -p <prompt>        single-turn: send prompt, print response, exit\n"
           "  -s <id>            session id\n"
           "  --trust-host       no kernel sandbox, no rlimits, no egress proxy,\n"
           "                     full env — tool allowlist still applies (CLI\n"
           "                     only, ignored by --daemon)\n"
           "  --auto-approve     answer parked approvals without prompting;\n"
           "                     single-use (rerun tools run once, capability\n"
           "                     grants expire — nothing durable is left behind)\n"
           "  --new              create a new session\n"
           "  --log-level=LEVEL  set log level (error|info|debug|trace)\n"
           "  -v, --debug        debug logging (timing, context stats)\n"
           "  -vv, --trace       trace logging (full req/resp JSON)\n"
           "  --doctor           print redacted diagnostic report and exit\n"
           "  --help             show this help\n"
           "\n"
           "note: running as root weakens mount-based read-only enforcement\n"
           "within sandboxed children (namespace root maps to real root).\n");
}

/* Session picker (preserved from old main.c) */
int64_t cli_select_session(sqlite3 *db, int64_t requested_id, int new_session) {
    if (new_session) return session_create(db, "cli", proc_agent_name(), -1, 0);
    if (requested_id > 0) return requested_id;

    const char *sql =
        "SELECT s.id, s.created_at,"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id ASC LIMIT 1),"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id DESC LIMIT 1)"
        " FROM sessions s ORDER BY s.updated_at DESC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return session_create(db, "cli", proc_agent_name(), -1, 0);

    typedef struct { int64_t id; time_t created; char first[52]; char last[52]; } Row;
    int cap = 8, count = 0;
    Row *rows = malloc((size_t)cap * sizeof(Row));
    if (!rows) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) { cap *= 2; Row *tmp = realloc(rows, (size_t)cap * sizeof(Row)); if (!tmp) { free(rows); sqlite3_finalize(stmt); return -1; } rows = tmp; }
        rows[count].id = sqlite3_column_int64(stmt, 0);
        rows[count].created = (time_t)sqlite3_column_int64(stmt, 1);
        const char *fp = (const char *)sqlite3_column_text(stmt, 2);
        const char *lp = (const char *)sqlite3_column_text(stmt, 3);
        snprintf(rows[count].first, sizeof(rows[count].first), "%s", fp ? fp : "");
        snprintf(rows[count].last, sizeof(rows[count].last), "%s", lp ? lp : "");
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0) { free(rows); return session_create(db, "cli", proc_agent_name(), -1, 0); }
    if (!isatty(STDIN_FILENO)) { int64_t r = rows[0].id; free(rows); return r; }

    printf("sessions:\n");
    for (int i = 0; i < count; i++) {
        char tb[20]; struct tm tm; localtime_r(&rows[i].created, &tm);
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", &tm);
        for (char *p = rows[i].first; *p; p++) if (*p == '\n') *p = ' ';
        printf("  %d) [%lld] %s | %s\n", i+1, (long long)rows[i].id, tb,
               rows[i].first[0] ? rows[i].first : "(empty)");
    }
    printf("  n) new session\nselect: "); fflush(stdout);

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) { free(rows); return -1; }
    int64_t result;
    if (buf[0] == 'n' || buf[0] == 'N') result = session_create(db, "cli", proc_agent_name(), -1, 0);
    else { int ch = atoi(buf); result = (ch >= 1 && ch <= count) ? rows[ch-1].id : -1; }
    free(rows);
    return result;
}

/* ── CLI turn trigger ───────────────────────────────────────────── */

void cli_start_turn(const char *input) {
    inbox_insert_scanned(proc_db(), proc_cli_session(), "cli", NULL, input);
    proc_set_cli_turn_active(1);
    cli_indicator_show();
    run_advance(proc_cli_session());
}
