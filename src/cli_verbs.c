/* Operator CLI verbs, split from main.c (2026-07-19). Self-contained: each
 * verb opens the DB via verb_db_open() and shares nothing with the daemon
 * event loop. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cclaw.h"
#include "cli_verbs.h"
#include "channel.h"
#include "config.h"
#include "config_registry.h"
#include "dashboard.h"
#include "db.h"
#include "log.h"
#include "secret.h"
#include "validate.h"
#include "secret_store.h"
#include <time.h>
#include "util.h"

/* Shared opener for the operator verbs (sensitive / secret-bind): open the
 * real DB with the schema-generation guard + ensure-schema, so the verbs
 * work on a fresh box before any agent run has initialized the DB. */
static sqlite3 *verb_db_open(void) {
    char *db_path = util_resolve_db_path();
    if (!db_path) { fprintf(stderr, "error: cannot resolve DB path\n"); return NULL; }
    sqlite3 *db = db_open(db_path);
    if (!db) { fprintf(stderr, "error: cannot open %s\n", db_path); free(db_path); return NULL; }
    if (!db_schema_compat(db)) {
        fprintf(stderr, "error: %s was created by a different cclaw schema — delete it\n", db_path);
        sqlite3_close(db); free(db_path); return NULL;
    }
    free(db_path);
    if (db_ensure_schema(db) != 0) {
        fprintf(stderr, "error: schema init failed\n");
        sqlite3_close(db); return NULL;
    }
    return db;
}

/* `cclaw sensitive add|rm|list [host]` — operator verb for the sensitivity
 * axis (specs/trust.md). Labels are global target properties, deliberately
 * NOT settable via any agent tool: only a human at the CLI (or sqlite3)
 * can label or unlabel a target. */
int sensitive_main(int argc, char *argv[]) {
    const char *sub = (argc >= 3) ? argv[2] : NULL;
    const char *host = (argc >= 4) ? argv[3] : NULL;
    sqlite3 *db = verb_db_open();
    if (!db) return 1;
    int rc = 0;
    if (sub && strcmp(sub, "list") == 0) {
        int n = 0;
        char **hosts = db_sensitive_hosts(db, &n);
        if (!hosts) printf("(no sensitive hosts)\n");
        for (int i = 0; i < n; i++) { printf("%s\n", hosts[i]); free(hosts[i]); }
        free(hosts);
    } else if (sub && host && strcmp(sub, "add") == 0) {
        rc = db_sensitive_host_add(db, host) == 0 ? 0 : 1;
        if (rc == 0) printf("sensitive host added: %s\n", host);
        else fprintf(stderr, "error: add failed\n");
    } else if (sub && host && strcmp(sub, "rm") == 0) {
        rc = db_sensitive_host_rm(db, host) == 0 ? 0 : 1;
        if (rc == 0) printf("sensitive host removed: %s\n", host);
        else fprintf(stderr, "error: rm failed\n");
    } else {
        fprintf(stderr, "usage: cclaw sensitive add|rm <host> | list\n"
                        "  host: exact (\"pay.example.com\") or suffix (\".example.com\");\n"
                        "  bare domains cover their subdomains\n");
        rc = 2;
    }
    sqlite3_close(db);
    return rc;
}

/* `cclaw secret-bind <name> <host> | rm <name> <host> | list` — operator verb
 * for the fail-closed credential rule (specs/trust.md): pre-seed or revoke
 * secret→host bindings. Bindings also accrete from ALWAYS approvals of
 * url-carrying calls ("approve & bind"). */
int secret_bind_main(int argc, char *argv[]) {
    sqlite3 *db = verb_db_open();
    if (!db) return 1;
    int rc = 0;
    if (argc >= 3 && strcmp(argv[2], "list") == 0) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT secret_name, host FROM secret_hosts"
                " ORDER BY secret_name, host", -1, &st, NULL) == SQLITE_OK) {
            int any = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                printf("%s -> %s\n", sqlite3_column_text(st, 0),
                       sqlite3_column_text(st, 1));
                any = 1;
            }
            sqlite3_finalize(st);
            if (!any) printf("(no secret bindings)\n");
        }
    } else if (argc >= 5 && strcmp(argv[2], "rm") == 0) {
        rc = db_secret_host_unbind(db, argv[3], argv[4]) == 0 ? 0 : 1;
        if (rc == 0) printf("binding removed: %s -> %s\n", argv[3], argv[4]);
        else fprintf(stderr, "error: rm failed\n");
    } else if (argc >= 4 && strcmp(argv[2], "rm") != 0) {
        rc = db_secret_host_bind(db, argv[2], argv[3]) == 0 ? 0 : 1;
        if (rc == 0) printf("binding added: %s -> %s\n", argv[2], argv[3]);
        else fprintf(stderr, "error: bind failed\n");
    } else {
        fprintf(stderr, "usage: cclaw secret-bind <name> <host> | rm <name> <host> | list\n"
                        "  host: exact (\"api.github.com\") or suffix (\".github.com\");\n"
                        "  bare domains cover their subdomains\n");
        rc = 2;
    }
    sqlite3_close(db);
    return rc;
}

/* `cclaw secret set <NAME> [value] | rm <NAME> | list` — operator verb for
 * the DB-backed secret store (specs/security.md). `set` with no value arg
 * reads one line from stdin, keeping the plaintext out of shell history.
 * `list` never prints values. A newly set secret has zero secret_hosts rows
 * (unless pre-seeded with `cclaw secret-bind`), so its first use always
 * parks — the same fail-closed rule as env-collected secrets. */
int secret_main(int argc, char *argv[]) {
    char *db_path = util_resolve_db_path();
    if (!db_path) { fprintf(stderr, "error: cannot resolve DB path\n"); return 1; }
    sqlite3 *db = verb_db_open();
    if (!db) { free(db_path); return 1; }
    { uint8_t sk[32]; if (secret_key_load_or_create(db_path, sk) == 0) db_set_secret_key(sk); }
    free(db_path);

    int rc = 0;
    if (argc >= 3 && strcmp(argv[2], "list") == 0) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT name, source, scope, created_at FROM secrets ORDER BY name",
                -1, &st, NULL) == SQLITE_OK) {
            int any = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                printf("%s  source=%s  scope=%s  created=%lld\n",
                       sqlite3_column_text(st, 0), sqlite3_column_text(st, 1),
                       sqlite3_column_text(st, 2),
                       (long long)sqlite3_column_int64(st, 3));
                any = 1;
            }
            sqlite3_finalize(st);
            if (!any) printf("(no secrets)\n");
        }
    } else if (argc >= 4 && strcmp(argv[2], "rm") == 0) {
        rc = db_secret_rm(db, argv[3]) == 0 ? 0 : 1;
        if (rc == 0) printf("secret removed: %s\n", argv[3]);
        else fprintf(stderr, "error: rm failed\n");
    } else if (argc >= 3 && strcmp(argv[2], "set") == 0) {
        const char *name = (argc >= 4) ? argv[3] : NULL;
        if (!name || !is_valid_secret_name(name)) {
            fprintf(stderr, "error: invalid secret name (expected ^[A-Z][A-Z0-9_]*$)\n");
            rc = 2;
        } else {
            char *value = NULL;
            if (argc >= 5) {
                value = strdup(argv[4]);
            } else {
                char line[4096];
                if (fgets(line, sizeof(line), stdin)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                        line[--len] = '\0';
                    value = strdup(line);
                }
            }
            if (!value || !value[0]) {
                fprintf(stderr, "error: no value provided\n");
                rc = 2;
            } else if (!db_secret_key_loaded()) {
                fprintf(stderr, "error: master key unavailable\n");
                rc = 1;
            } else {
                rc = db_secret_set(db, name, value, "operator", "agent") == 0 ? 0 : 1;
                if (rc == 0) printf("secret set: %s\n", name);
                else fprintf(stderr, "error: set failed\n");
            }
            if (value) { explicit_bzero(value, strlen(value)); free(value); }
        }
    } else {
        fprintf(stderr, "usage: cclaw secret set <NAME> [value] | rm <NAME> | list\n"
                        "  set with no value reads one line from stdin\n"
                        "  name: ^[A-Z][A-Z0-9_]*$\n");
        rc = 2;
    }
    db_wipe_secret_key();
    sqlite3_close(db);
    return rc;
}

/* `cclaw route add <channel> <chat_id> <agent> | rm <channel> <chat_id> | list`
 * — operator verb binding a channel+chat to an agent (channel_routes). A
 * chat_id of '*' is the channel-wide default. Resolution at dispatch is exact
 * (channel,chat_id) -> (channel,'*') -> config default_agent, so with no rows
 * every chat falls back to the default. Deliberately CLI-only: re-pointing a
 * chat at another agent is an authority change, not agent self-service. */
/* `cclaw channel <list|swap|revert|restart>` — operator verbs for the channel
 * hot-swap flow. Deliberately CLI-only, same rationale as route/sensitive: an
 * authority change over what code fronts a channel. The daemon's reconcile in
 * channel_tick picks up pointer/status changes; bounce covers a live process. */
int channel_cli_main(int argc, char *argv[]) {
    const char *sub = (argc >= 3) ? argv[2] : NULL;
    sqlite3 *db = verb_db_open();
    if (!db) return 1;
    int rc = 0;
    if (!sub || strcmp(sub, "list") == 0) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT name, status, extension_name,"
                "       COALESCE(prev_extension_name, ''), COALESCE(pid, 0)"
                " FROM channels ORDER BY name", -1, &st, NULL) == SQLITE_OK) {
            int any = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *prev = (const char *)sqlite3_column_text(st, 3);
                printf("%s: status=%s extension=%s pid=%d%s%s\n",
                       sqlite3_column_text(st, 0), sqlite3_column_text(st, 1),
                       sqlite3_column_text(st, 2), sqlite3_column_int(st, 4),
                       prev && prev[0] ? " revert-target=" : "",
                       prev ? prev : "");
                any = 1;
            }
            sqlite3_finalize(st);
            if (!any) printf("(no channels registered)\n");
        }
    } else if (strcmp(sub, "swap") == 0 && argc >= 5) {
        int r = channel_swap(db, argv[3], argv[4]);
        if (r == -2) { fprintf(stderr, "error: extension '%s' not registered\n", argv[4]); rc = 1; }
        else if (r != 0) { fprintf(stderr, "error: channel '%s' not found\n", argv[3]); rc = 1; }
        else printf("channel %s now runs extension '%s' (previous kept as revert "
                    "target; daemon respawns the process within seconds)\n",
                    argv[3], argv[4]);
    } else if (strcmp(sub, "revert") == 0 && argc >= 4) {
        if (channel_revert(db, argv[3]) != 0) {
            fprintf(stderr, "error: nothing to revert for '%s'\n", argv[3]);
            rc = 1;
        } else printf("channel %s reverted to its previous extension\n", argv[3]);
    } else if (strcmp(sub, "restart") == 0 && argc >= 4) {
        if (channel_bounce(db, argv[3]) != 0) {
            fprintf(stderr, "error: channel '%s' not found\n", argv[3]);
            rc = 1;
        } else printf("channel %s restarting (daemon respawns it within seconds)\n", argv[3]);
    } else {
        fprintf(stderr, "usage: cclaw channel [list] | swap <channel> <extension>"
                        " | revert <channel> | restart <channel>\n");
        rc = 1;
    }
    db_close(db);
    return rc;
}

int route_main(int argc, char *argv[]) {
    const char *sub = (argc >= 3) ? argv[2] : NULL;
    sqlite3 *db = verb_db_open();
    if (!db) return 1;
    int rc = 0;
    if (sub && strcmp(sub, "list") == 0) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT channel_name, channel_id, agent_name, delivery_mode, session_id,"
                "       tool_filter"
                " FROM channel_routes ORDER BY channel_name, channel_id",
                -1, &st, NULL) == SQLITE_OK) {
            int any = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                printf("%s %s -> %s (mode %s", sqlite3_column_text(st, 0),
                       sqlite3_column_text(st, 1), sqlite3_column_text(st, 2),
                       sqlite3_column_text(st, 3));
                if (sqlite3_column_type(st, 4) != SQLITE_NULL)
                    printf(", session %lld", (long long)sqlite3_column_int64(st, 4));
                if (sqlite3_column_type(st, 5) != SQLITE_NULL)
                    printf(", tools %s", sqlite3_column_text(st, 5));
                printf(")\n");
                any = 1;
            }
            sqlite3_finalize(st);
            if (!any) printf("(no routes — unrouted senders are dropped unless"
                             " admin or <ext>.allow_unknown=1)\n");
        }
    } else if (sub && strcmp(sub, "add") == 0 && argc >= 6) {
        const char *ch = argv[3], *cid = argv[4], *agent = argv[5];
        const char *mode = NULL;
        const char *tools = NULL;
        int64_t pin_session = 0;
        int bad = 0;
        for (int i = 6; i < argc; i++) {
            if (strcmp(argv[i], "--tools") == 0 && i + 1 < argc) {
                tools = argv[++i];
            } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
                mode = argv[++i];
                if (strcmp(mode, "auto") != 0 && strcmp(mode, "explicit") != 0) {
                    fprintf(stderr, "error: --mode must be auto or explicit\n");
                    bad = 1;
                }
            } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
                pin_session = atoll(argv[++i]);
                if (pin_session <= 0) {
                    fprintf(stderr, "error: --session needs a positive session id\n");
                    bad = 1;
                }
            } else {
                fprintf(stderr, "error: unknown route add option '%s'\n", argv[i]);
                bad = 1;
            }
        }
        if (bad) { sqlite3_close(db); return 1; }
        /* --tools name,name,... -> JSON array for channel_routes.tool_filter.
         * Built with json_group_array so quoting is SQLite's problem. */
        char *filter_json = NULL;
        if (tools) {
            sqlite3_stmt *fj;
            if (sqlite3_prepare_v2(db,
                    "SELECT json_group_array(trim(value))"
                    " FROM json_each('[\"' || replace(?1, ',', '\",\"') || '\"]')"
                    " WHERE trim(value) <> ''",
                    -1, &fj, NULL) == SQLITE_OK) {
                sqlite3_bind_text(fj, 1, tools, -1, SQLITE_STATIC);
                if (sqlite3_step(fj) == SQLITE_ROW &&
                    sqlite3_column_type(fj, 0) != SQLITE_NULL) {
                    const char *j = (const char *)sqlite3_column_text(fj, 0);
                    if (j && strcmp(j, "[]") != 0) filter_json = strdup(j);
                }
                sqlite3_finalize(fj);
            }
            if (!filter_json || strchr(tools, '"') || strchr(tools, '\\')) {
                fprintf(stderr, "error: --tools needs a comma-separated list of tool names\n");
                free(filter_json);
                sqlite3_close(db);
                return 1;
            }
        }
        /* Group-shaped ids (negative — Telegram groups) default to explicit:
         * silent-by-default listen-and-decide until the operator says auto. */
        if (!mode) mode = (cid[0] == '-') ? "explicit" : "auto";
        /* Warn (don't refuse) if the agent isn't registered yet — the operator
         * may create it later; a route to a missing agent just drops until then. */
        sqlite3_stmt *ck;
        int exists = 0;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM agents WHERE name=?", -1, &ck, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ck, 1, agent, -1, SQLITE_STATIC);
            exists = (sqlite3_step(ck) == SQLITE_ROW);
            sqlite3_finalize(ck);
        }
        if (!exists) fprintf(stderr, "warning: agent '%s' not found in agents table\n", agent);
        /* Friction note: routing a specific external chat to an agent that
         * holds grants, with no --tools, hands the sender full authority. */
        if (!tools && strcmp(cid, "*") != 0) {
            sqlite3_stmt *gk;
            int has_grants = 0;
            if (sqlite3_prepare_v2(db, "SELECT 1 FROM grants WHERE agent_name=? LIMIT 1",
                                   -1, &gk, NULL) == SQLITE_OK) {
                sqlite3_bind_text(gk, 1, agent, -1, SQLITE_STATIC);
                has_grants = (sqlite3_step(gk) == SQLITE_ROW);
                sqlite3_finalize(gk);
            }
            if (has_grants)
                fprintf(stderr, "note: route grants full '%s' authority to this chat;"
                                " consider --tools to attenuate\n", agent);
        }
        if (pin_session > 0) {
            sqlite3_stmt *sk;
            int sess_ok = 0;
            if (sqlite3_prepare_v2(db, "SELECT 1 FROM sessions WHERE id=?", -1, &sk, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(sk, 1, pin_session);
                sess_ok = (sqlite3_step(sk) == SQLITE_ROW);
                sqlite3_finalize(sk);
            }
            if (!sess_ok)
                fprintf(stderr, "warning: session %lld not found — pin ignored until it exists\n",
                        (long long)pin_session);
        }
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO channel_routes(channel_name, channel_id, agent_name, session_id, delivery_mode, tool_filter)"
                " VALUES(?1,?2,?3,CASE WHEN ?4>0 THEN ?4 END,?5,?6)"
                " ON CONFLICT(channel_name, channel_id) DO UPDATE SET"
                "  agent_name=excluded.agent_name, session_id=excluded.session_id,"
                "  delivery_mode=excluded.delivery_mode, tool_filter=excluded.tool_filter",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, ch, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, cid, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, agent, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 4, pin_session);
            sqlite3_bind_text(st, 5, mode, -1, SQLITE_STATIC);
            if (filter_json) sqlite3_bind_text(st, 6, filter_json, -1, SQLITE_STATIC);
            rc = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
            sqlite3_finalize(st);
        } else rc = 1;
        if (rc == 0) {
            printf("route set: %s %s -> %s (mode %s", ch, cid, agent, mode);
            if (pin_session > 0) printf(", session %lld", (long long)pin_session);
            if (filter_json) printf(", tools %s", filter_json);
            printf(")\n");
        } else fprintf(stderr, "error: add failed\n");
        free(filter_json);
    } else if (sub && strcmp(sub, "rm") == 0 && argc >= 5) {
        const char *ch = argv[3], *cid = argv[4];
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "DELETE FROM channel_routes WHERE channel_name=? AND channel_id=?",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, ch, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, cid, -1, SQLITE_STATIC);
            rc = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
            int changed = sqlite3_changes(db);
            sqlite3_finalize(st);
            if (rc == 0) printf(changed ? "route removed: %s %s\n" : "no such route: %s %s\n", ch, cid);
        } else rc = 1;
    } else {
        fprintf(stderr, "usage: cclaw route add <channel> <chat_id> <agent>"
                        " [--mode auto|explicit] [--session <id>] [--tools name,name,...]\n"
                        "       cclaw route rm  <channel> <chat_id>\n"
                        "       cclaw route list\n"
                        "  chat_id '*' is the channel-wide default;\n"
                        "  resolution: exact (channel,chat_id) -> (channel,'*') -> gate\n"
                        "  (unrouted senders drop unless admin or allow_unknown=1)\n"
                        "  --mode default: explicit for group ids (negative), else auto\n"
                        "  --session pins the chat to one session\n"
                        "  --tools limits sessions created for this route to those tools\n"
                        "  (frozen at session creation; a route edit needs a new session)\n");
        rc = 2;
    }
    sqlite3_close(db);
    return rc;
}

/* Print one llm_responses row: header + body rendered as JSON. Bodies are
 * stored as JSONB, which system sqlite3 CLIs older than 3.45 (e.g. Debian
 * bookworm's 3.40) can't decode — the vendored SQLite in this binary is the
 * one guaranteed reader. `which` is "body" or "request_body". */
static int resp_print(sqlite3 *db, const char *where, int64_t id, const char *which) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT id, status, model, session_id, turn_id,"
        "       datetime(created_at,'unixepoch','localtime'),"
        "       CASE WHEN %s IS NULL THEN NULL"
        "            WHEN json_valid(%s, 8) THEN json_pretty(%s)"
        "            ELSE CAST(%s AS TEXT) END"
        " FROM llm_responses WHERE %s ORDER BY id DESC LIMIT 1",
        which, which, which, which, where);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_parameter_count(st) > 0) sqlite3_bind_int64(st, 1, id);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = 1;
        printf("resp #%lld status=%s model=%s session=%lld turn=%lld at %s\n",
               (long long)sqlite3_column_int64(st, 0), sqlite3_column_text(st, 1),
               sqlite3_column_text(st, 2), (long long)sqlite3_column_int64(st, 3),
               (long long)sqlite3_column_int64(st, 4), sqlite3_column_text(st, 5));
        const char *body = (const char *)sqlite3_column_text(st, 6);
        printf("%s\n", body ? body : strcmp(which, "body") == 0
               ? "(no body — provider sent nothing)"
               : "(no request archived — only failed attempts keep the request)");
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

/* `cclaw dashboard` — print the tokenized admin dashboard URL. The token is
 * generated by the daemon's web_start; before the first daemon run there is
 * nothing to print. */
int dashboard_main(void) {
    sqlite3 *db = verb_db_open();
    if (!db) return 1;
    char *url = dashboard_url(db);
    sqlite3_close(db);
    if (!url) {
        fprintf(stderr, "no admin token yet — run the daemon once (cclaw --daemon)\n");
        return 1;
    }
    printf("%s\n", url);
    free(url);
    return 0;
}

/* `cclaw backup [dest]` — write a consistent single-file snapshot via
 * VACUUM INTO. Default dest: <db>.backup.<yyyymmdd-HHMMSS>. Refuses to
 * overwrite and respects the disk floor. Restore is a documented manual
 * procedure (stop daemon → move snapshot over cclaw.db → delete -wal/-shm →
 * restart); see specs/operations.md. Safe to run against a live daemon:
 * VACUUM INTO takes a read snapshot, so a mid-turn session is captured
 * consistently and recovery reconciles it on restart. */
int backup_main(int argc, char *argv[]) {
    sqlite3 *db = verb_db_open();
    if (!db) return 1;

    char dest[1024];
    if (argc >= 3 && argv[2][0]) {
        snprintf(dest, sizeof(dest), "%s", argv[2]);
    } else {
        const char *path = sqlite3_db_filename(db, "main");
        if (!path || !path[0]) {
            fprintf(stderr, "error: cannot resolve DB path for default backup name\n");
            sqlite3_close(db);
            return 1;
        }
        char ts[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
        snprintf(dest, sizeof(dest), "%s.backup.%s", path, ts);
    }

    /* Disk floor: a snapshot is roughly the DB's size — refuse under the floor
     * so a backup can't be the write that fills an SD card. */
    int floor_mb = config_default_int("disk_min_free_mb");
    if (floor_mb > 0) {
        long free_mb = db_free_mb(db);
        if (free_mb >= 0 && free_mb < floor_mb) {
            fprintf(stderr, "error: free space %ldMB below floor %dMB — backup refused\n",
                    free_mb, floor_mb);
            sqlite3_close(db);
            return 1;
        }
    }

    long long bytes = -1;
    int rc = db_backup_to(db, dest, &bytes);
    sqlite3_close(db);
    if (rc != 0) {
        fprintf(stderr, "error: backup failed (see log; likely %s exists or disk error)\n", dest);
        return 1;
    }
    printf("%s (%lld bytes)\n", dest, bytes);
    return 0;
}

/* `cclaw resp` — read the llm_responses forensic archive. What "[resp #N]" in
 * an error message cites. */
int resp_main(int argc, char *argv[]) {
    const char *sub = (argc >= 3) ? argv[2] : NULL;
    sqlite3 *db = verb_db_open();
    if (!db) return 1;
    int rc = 0;
    if (sub && strcmp(sub, "list") == 0) {
        int n = (argc >= 4) ? atoi(argv[3]) : 20;
        if (n <= 0) n = 20;
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT id, status, model, session_id, turn_id,"
                "       datetime(created_at,'unixepoch','localtime'), COALESCE(length(body),0)"
                " FROM llm_responses ORDER BY id DESC LIMIT ?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, n);
            int any = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                printf("#%-5lld %-13s %s session=%lld turn=%lld %s %d bytes\n",
                       (long long)sqlite3_column_int64(st, 0), sqlite3_column_text(st, 1),
                       sqlite3_column_text(st, 2), (long long)sqlite3_column_int64(st, 3),
                       (long long)sqlite3_column_int64(st, 4), sqlite3_column_text(st, 5),
                       sqlite3_column_int(st, 6));
                any = 1;
            }
            sqlite3_finalize(st);
            if (!any) printf("(archive empty — see config llm_response_archive_max)\n");
        }
    } else if (!sub) {
        /* Bare `cclaw resp`: the most recent failure. */
        int r = resp_print(db, "status != 'ok'", 0, "body");
        if (r == 1) { printf("(no archived failures)\n"); }
        else if (r < 0) rc = 1;
    } else if (atoll(sub) > 0) {
        const char *which = (argc >= 4 && strcmp(argv[3], "req") == 0) ? "request_body" : "body";
        int r = resp_print(db, "id = ?1", atoll(sub), which);
        if (r == 1) { fprintf(stderr, "error: no archived response #%s (pruned? see `cclaw resp list`)\n", sub); rc = 1; }
        else if (r < 0) rc = 1;
    } else {
        fprintf(stderr, "usage: cclaw resp             # most recent failure (what \"[resp #N]\" cites)\n"
                        "       cclaw resp <id> [req]  # one archived row; `req` prints the request we sent\n"
                        "       cclaw resp list [n]    # recent archive rows, newest first\n");
        rc = 2;
    }
    sqlite3_close(db);
    return rc;
}
