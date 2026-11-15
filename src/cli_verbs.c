/* Operator CLI verbs, split from main.c (2026-07-19). Self-contained: each
 * verb opens the DB via verb_db_open() and shares nothing with the daemon
 * event loop. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include "cclaw.h"
#include "cli_verbs.h"
#include "channel.h"
#include "config.h"
#include "config_registry.h"
#include "dashboard.h"
#include "agent_config.h"
#include "db.h"
#include "log.h"
#include "models_cache.h"
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
    db_set_actor(db, "cli");   /* change_log: an operator verb did this */
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
 * secret→host bindings. Bindings also mint from an approved request_config
 * secret_bindings document (D17). */
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

/* `cclaw secret set <NAME> [value] | rm <NAME> | list | grant <NAME> <agent>
 * | revoke <NAME> <agent>` — operator verb for the DB-backed secret store
 * (specs/security.md). `set` with no value arg reads one line from stdin,
 * keeping the plaintext out of shell history. `list` never prints values.
 * A newly set secret is granted to no agent (using a secret is a grant) and
 * has zero secret_hosts rows (unless pre-seeded with `cclaw secret-bind`):
 * `grant` decides who may use it, bindings decide where it may go. `grant`
 * also takes the name of an environment (CCLAW_SECRET_*) secret, which has no
 * row here. `rm` removes the name's bindings and grants with it. */
int secret_main(int argc, char *argv[]) {
    char *db_path = util_resolve_db_path();
    if (!db_path) { fprintf(stderr, "error: cannot resolve DB path\n"); return 1; }
    sqlite3 *db = verb_db_open();
    if (!db) { free(db_path); return 1; }
    { uint8_t sk[32]; if (secret_key_load_or_create(db_path, sk) == 0) db_set_secret_key(sk); }
    free(db_path);

    int rc = 0;
    if (argc >= 3 && strcmp(argv[2], "list") == 0) {
        /* Stored secrets, then names granted with no stored row (environment
         * secrets); for each, who may use it. */
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT n.name, s.source, s.scope, s.created_at,"
                "  (SELECT group_concat(agent_name, ',') FROM (SELECT agent_name"
                "     FROM grants g WHERE g.kind='secret' AND g.value=n.name"
                "      AND (g.expires_at IS NULL OR g.expires_at > unixepoch())"
                "    ORDER BY agent_name))"
                " FROM (SELECT name FROM secrets UNION"
                "       SELECT value FROM grants WHERE kind='secret') n"
                " LEFT JOIN secrets s ON s.name=n.name ORDER BY n.name",
                -1, &st, NULL) == SQLITE_OK) {
            int any = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *who = (const char *)sqlite3_column_text(st, 4);
                if (sqlite3_column_type(st, 1) == SQLITE_NULL)
                    printf("%s  (environment)  granted=%s\n",
                           sqlite3_column_text(st, 0), who ? who : "(nobody)");
                else
                    printf("%s  source=%s  scope=%s  created=%lld  granted=%s\n",
                           sqlite3_column_text(st, 0), sqlite3_column_text(st, 1),
                           sqlite3_column_text(st, 2),
                           (long long)sqlite3_column_int64(st, 3),
                           strcmp((const char *)sqlite3_column_text(st, 2), "system") == 0
                               ? "(daemon only)" : who ? who : "(nobody)");
                any = 1;
            }
            sqlite3_finalize(st);
            if (!any) printf("(no secrets)\n");
        }
    } else if (argc >= 4 && strcmp(argv[2], "rm") == 0) {
        int n[3];
        rc = db_secret_rm(db, argv[3], n) == 0 ? 0 : 1;
        if (rc == 0)
            printf("secret removed: %s (%d stored value, %d host binding(s), "
                   "%d grant(s))\n", argv[3], n[0], n[1], n[2]);
        else fprintf(stderr, "error: rm failed\n");
    } else if (argc >= 5 && (strcmp(argv[2], "grant") == 0 ||
                             strcmp(argv[2], "revoke") == 0)) {
        const char *name = argv[3], *agent = argv[4];
        int grant = strcmp(argv[2], "grant") == 0;
        int system = 0, agent_ok = 0;
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT EXISTS(SELECT 1 FROM secrets WHERE name=?1 AND scope='system'),"
                "       EXISTS(SELECT 1 FROM agents WHERE name=?2)",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, agent, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                system = sqlite3_column_int(st, 0);
                agent_ok = sqlite3_column_int(st, 1);
            }
            sqlite3_finalize(st);
        }
        if (!is_valid_secret_name(name)) {
            fprintf(stderr, "error: invalid secret name (expected ^[A-Z][A-Z0-9_]*$)\n");
            rc = 2;
        } else if (!agent_ok) {
            fprintf(stderr, "error: no agent named '%s'\n", agent);
            rc = 2;
        } else if (grant && system) {
            fprintf(stderr, "error: %s is a system secret (a provider key) — the "
                            "daemon uses it; no agent can be granted it\n", name);
            rc = 2;
        } else if (grant) {
            rc = agent_config_grant(db, agent, "secret", name, 0,
                                    GRANT_ORIGIN("operator", 0)) == 0 ? 0 : 1;
            if (rc == 0) printf("agent %s may now use secret %s\n", agent, name);
            else fprintf(stderr, "error: grant failed\n");
        } else {
            rc = agent_config_revoke(db, agent, "secret", name) == 0 ? 0 : 1;
            if (rc == 0) printf("agent %s may no longer use secret %s\n", agent, name);
            else fprintf(stderr, "error: revoke failed\n");
        }
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
                if (rc == 0) {
                    char *who = NULL;
                    sqlite3_stmt *st;
                    if (sqlite3_prepare_v2(db,
                            "SELECT group_concat(agent_name, ', ') FROM grants"
                            " WHERE kind='secret' AND value=?1", -1, &st, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
                        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
                            who = strdup((const char *)sqlite3_column_text(st, 0));
                        sqlite3_finalize(st);
                    }
                    if (who) printf("secret set: %s (usable by: %s)\n", name, who);
                    else printf("secret set: %s — no agent may use it yet: "
                                "cclaw secret grant %s <agent>\n", name, name);
                    free(who);
                }
                else fprintf(stderr, "error: set failed\n");
            }
            if (value) { explicit_bzero(value, strlen(value)); free(value); }
        }
    } else {
        fprintf(stderr, "usage: cclaw secret set <NAME> [value] | rm <NAME> | list\n"
                        "                    | grant <NAME> <agent> | revoke <NAME> <agent>\n"
                        "  set with no value reads one line from stdin\n"
                        "  a new secret is usable by no agent until granted\n"
                        "  name: ^[A-Z][A-Z0-9_]*$\n");
        rc = 2;
    }
    db_wipe_secret_key();
    sqlite3_close(db);
    return rc;
}

/* `cclaw route add <channel> <chat_id> <agent> | rm <channel> <chat_id> | list`
 * — operator verb pinning a chat to a session (channel_chats); the session
 * names the agent. add creates the session unless --session pins an existing
 * one. chat_id '*' is sugar for channels.default_agent (open-door policy for
 * unrouted chats), not a route. Deliberately CLI-only: re-pointing a chat is
 * an authority change, not agent self-service. */
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
                "SELECT name, status, COALESCE(extension_name, '(none)'),"
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
                    "target until this one has run 5 minutes; daemon respawns "
                    "the process within seconds)\n",
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
        int any = 0;
        /* Every chat the database knows, routed first. An unrouted chat is
         * one whose messages the gate drops (or hands to the channel's
         * default agent) — listed so the operator sees who has been
         * knocking. */
        if (sqlite3_prepare_v2(db,
                "SELECT c.channel_name, c.chat_id, c.session_id, s.agent_name,"
                "       c.delivery_mode, c.tool_filter, c.guidance, c.title,"
                "       date(COALESCE(c.last_message_at, c.first_seen_at),'unixepoch'),"
                "       c.notified_at IS NOT NULL"
                " FROM channel_chats c LEFT JOIN sessions s ON s.id = c.session_id"
                " ORDER BY c.session_id IS NULL, c.channel_name, c.chat_id",
                -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *title = (const char *)sqlite3_column_text(st, 7);
                if (sqlite3_column_type(st, 2) == SQLITE_NULL) {
                    printf("%s %s -> not routed (%s%slast seen %s%s)\n",
                           sqlite3_column_text(st, 0), sqlite3_column_text(st, 1),
                           title ? title : "", title ? ", " : "",
                           sqlite3_column_text(st, 8),
                           sqlite3_column_int(st, 9) ? ", operator told" : "");
                    any = 1;
                    continue;
                }
                printf("%s %s -> session %lld (%s, mode %s",
                       sqlite3_column_text(st, 0), sqlite3_column_text(st, 1),
                       (long long)sqlite3_column_int64(st, 2),
                       sqlite3_column_text(st, 3), sqlite3_column_text(st, 4));
                if (sqlite3_column_type(st, 5) != SQLITE_NULL)
                    printf(", tools %s", sqlite3_column_text(st, 5));
                if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
                    /* Presence + a taste of it: the full text can be a
                     * paragraph, and `route list` is a one-line-per-chat view. */
                    const char *sp = (const char *)sqlite3_column_text(st, 6);
                    printf(", prompt \"%.40s%s\"", sp, strlen(sp) > 40 ? "…" : "");
                }
                if (title) printf(", %s", title);
                printf(")\n");
                any = 1;
            }
            sqlite3_finalize(st);
        }
        if (sqlite3_prepare_v2(db,
                "SELECT name, default_agent, default_tool_filter FROM channels"
                " WHERE default_agent IS NOT NULL ORDER BY name",
                -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                printf("%s (any chat) -> new session for %s [default_agent]",
                       sqlite3_column_text(st, 0), sqlite3_column_text(st, 1));
                if (sqlite3_column_type(st, 2) != SQLITE_NULL)
                    printf(" (tools %s)", sqlite3_column_text(st, 2));
                printf("\n");
                any = 1;
            }
            sqlite3_finalize(st);
        }
        if (!any) printf("(no routes and no channel default_agent —"
                         " unrouted chats drop unless admin)\n");
    } else if (sub && strcmp(sub, "add") == 0 && argc >= 6) {
        const char *ch = argv[3], *cid = argv[4], *agent = argv[5];
        const char *mode = NULL;
        const char *tools = NULL;
        const char *prompt = NULL;
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
            } else if (strcmp(argv[i], "--prompt") == 0 && i + 1 < argc) {
                prompt = argv[++i];
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
        /* --tools name,name,... -> JSON array for channel_chats.tool_filter
         * (channels.default_tool_filter when chat_id is '*').
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
        /* '*' = channel-wide default agent (channels.default_agent): open
         * door + who serves new chats. Not a route — routes pin sessions.
         * --tools here is the *unrouted* filter (channels.default_tool_filter):
         * every chat the gate records on this channel from now on starts with
         * it, so a server-wide bot answers strangers with an attenuated tool
         * set while an explicitly routed chat keeps its own (or full)
         * authority. Chats already recorded keep theirs. */
        if (strcmp(cid, "*") == 0) {
            if (mode || pin_session > 0) {
                fprintf(stderr, "error: '*' sets channels.default_agent —"
                                " --mode/--session don't apply\n");
                free(filter_json);
                sqlite3_close(db);
                return 1;
            }
            sqlite3_stmt *up;
            rc = 1;
            if (sqlite3_prepare_v2(db,
                    "UPDATE channels SET default_agent=?2, default_tool_filter=?3"
                    " WHERE name=?1",
                    -1, &up, NULL) == SQLITE_OK) {
                sqlite3_bind_text(up, 1, ch, -1, SQLITE_STATIC);
                sqlite3_bind_text(up, 2, agent, -1, SQLITE_STATIC);
                if (filter_json) sqlite3_bind_text(up, 3, filter_json, -1, SQLITE_STATIC);
                rc = (sqlite3_step(up) == SQLITE_DONE && sqlite3_changes(db) > 0)
                     ? 0 : 1;
                sqlite3_finalize(up);
            }
            if (rc == 0) {
                printf("default agent set: %s (any chat) -> %s", ch, agent);
                if (filter_json) printf(" (tools %s)", filter_json);
                printf("\n");
            } else
                fprintf(stderr, "error: no such channel '%s'\n", ch);
            free(filter_json);
            sqlite3_close(db);
            return rc;
        }
        /* Groups default to explicit: silent-by-default listen-and-decide
         * until the operator says auto. A chat that has spoken says what it
         * is (its envelope's kind); one that never spoke is judged by its id
         * (negative — a Telegram group). */
        char kind[8] = "";
        if (!mode) {
            sqlite3_stmt *kq;
            if (sqlite3_prepare_v2(db, "SELECT kind FROM channel_chats"
                                       " WHERE channel_name=?1 AND chat_id=?2",
                                   -1, &kq, NULL) == SQLITE_OK) {
                sqlite3_bind_text(kq, 1, ch, -1, SQLITE_STATIC);
                sqlite3_bind_text(kq, 2, cid, -1, SQLITE_STATIC);
                if (sqlite3_step(kq) == SQLITE_ROW && sqlite3_column_text(kq, 0))
                    snprintf(kind, sizeof(kind), "%s", sqlite3_column_text(kq, 0));
                sqlite3_finalize(kq);
            }
            mode = strcmp(kind, "group") == 0 ? "explicit"
                 : strcmp(kind, "dm") == 0    ? "auto"
                 : chat_default_mode(cid);
        }
        /* A route creates (or pins) a session bound to the agent, so the
         * agent must exist — sessions.agent_name is a real FK. */
        sqlite3_stmt *ck;
        int exists = 0;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM agents WHERE name=?", -1, &ck, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ck, 1, agent, -1, SQLITE_STATIC);
            exists = (sqlite3_step(ck) == SQLITE_ROW);
            sqlite3_finalize(ck);
        }
        if (!exists) {
            fprintf(stderr, "error: agent '%s' not found in agents table\n", agent);
            free(filter_json);
            sqlite3_close(db);
            return 1;
        }
        /* Friction note: routing a specific external chat to an agent that
         * holds grants, with no --tools, hands the sender full authority. */
        if (!tools) {
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
        /* --session must name an existing session of the named agent — a
         * mismatch is refused, never silently rebound. */
        if (pin_session > 0) {
            sqlite3_stmt *sk;
            char *sess_agent = NULL;
            if (sqlite3_prepare_v2(db, "SELECT agent_name FROM sessions WHERE id=?",
                                   -1, &sk, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(sk, 1, pin_session);
                if (sqlite3_step(sk) == SQLITE_ROW) {
                    const char *v = (const char *)sqlite3_column_text(sk, 0);
                    sess_agent = v ? strdup(v) : NULL;
                }
                sqlite3_finalize(sk);
            }
            if (!sess_agent || strcmp(sess_agent, agent) != 0) {
                fprintf(stderr, sess_agent
                        ? "error: session %lld belongs to '%s', not '%s'\n"
                        : "error: session %lld not found\n",
                        (long long)pin_session, sess_agent, agent);
                free(sess_agent);
                free(filter_json);
                sqlite3_close(db);
                return 1;
            }
            free(sess_agent);
        }
        /* One transaction: the pin (a fresh session unless --session; a
         * session another chat held leaves that chat on a fresh one) and the
         * chat's settings, which every session of the chat reads live. */
        rc = 1;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK) {
            char name[192];
            snprintf(name, sizeof(name), "route:%s:%s", ch, cid);
            int ok = pin_session > 0
                ? channel_pin_session(db, ch, cid, pin_session) == 0
                : (pin_session = channel_pin_new_session(db, ch, cid, agent, name)) > 0;
            sqlite3_stmt *st;
            if (ok && sqlite3_prepare_v2(db,
                    "UPDATE channel_chats SET delivery_mode=?3, tool_filter=?4,"
                    /* ?6 = "--prompt was given". Omitting it on a re-add keeps
                     * the existing guidance (prose an operator tuned is not
                     * something to lose to a re-pin); `--prompt ""` passes
                     * NULL and clears. */
                    "  guidance=CASE WHEN ?6 THEN ?5 ELSE guidance END"
                    " WHERE channel_name=?1 AND chat_id=?2",
                    -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, ch, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 2, cid, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 3, mode, -1, SQLITE_STATIC);
                if (filter_json) sqlite3_bind_text(st, 4, filter_json, -1, SQLITE_STATIC);
                if (prompt && prompt[0]) sqlite3_bind_text(st, 5, prompt, -1, SQLITE_STATIC);
                sqlite3_bind_int(st, 6, prompt != NULL);
                ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
                sqlite3_finalize(st);
            } else ok = 0;
            rc = (ok && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? 0 : 1;
            if (rc != 0) {
                fprintf(stderr, "error: %s\n", sqlite3_errmsg(db));
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            }
        }
        if (rc == 0) {
            printf("route set: %s %s -> session %lld (%s, mode %s",
                   ch, cid, (long long)pin_session, agent, mode);
            if (filter_json) printf(", tools %s", filter_json);
            if (prompt && prompt[0]) printf(", prompt set");
            printf(")\n");
        } else fprintf(stderr, "error: add failed\n");
        free(filter_json);
    } else if (sub && strcmp(sub, "rm") == 0 && argc >= 5) {
        const char *ch = argv[3], *cid = argv[4];
        sqlite3_stmt *st;
        if (strcmp(cid, "*") == 0) {
            if (sqlite3_prepare_v2(db,
                    "UPDATE channels SET default_agent=NULL,"
                    " default_tool_filter=NULL WHERE name=?",
                    -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, ch, -1, SQLITE_STATIC);
                rc = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
                int changed = sqlite3_changes(db);
                sqlite3_finalize(st);
                if (rc == 0)
                    printf(changed ? "default agent cleared: %s\n"
                                   : "no such channel: %s\n", ch);
            } else rc = 1;
        } else if (sqlite3_prepare_v2(db,
                /* The chat's row stays — its sessions and cron jobs still
                 * belong to it, and its settings return if it is routed again.
                 * Its next message meets the gate, and the operator is told
                 * again if that drops it. */
                "UPDATE channel_chats SET session_id=NULL, notified_at=NULL"
                " WHERE channel_name=? AND chat_id=? AND session_id IS NOT NULL",
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
                        "                                            [--prompt <text>]\n"
                        "       cclaw route rm  <channel> <chat_id>\n"
                        "       cclaw route list\n"
                        "  a route pins the chat to a session; the session names the agent\n"
                        "  (add creates the session unless --session pins an existing one)\n"
                        "  chat_id '*' sets/clears channels.default_agent instead —\n"
                        "  the open-door default for chats with no route\n"
                        "  (without it, unrouted chats drop unless admin)\n"
                        "  --mode default: explicit for group ids (negative), else auto\n"
                        "  (with '*', --tools sets the filter every chat the gate records\n"
                        "   on that channel from now on starts with)\n"
                        "  --tools limits what this chat's session may use; --mode and\n"
                        "  --prompt (appended to its system prompt; empty string clears)\n"
                        "  are the chat's too — all read live, an edit applies next turn\n"
                        "  list also shows chats nobody routed; rm keeps the chat's row\n");
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
        "SELECT id, status, model, session_id, iteration_id,"
        "       datetime(created_at,'unixepoch','localtime'), provider_id,"
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
        const char *provider_id = (const char *)sqlite3_column_text(st, 6);
        printf("resp #%lld status=%s model=%s session=%lld iter=%lld at %s provider_id=%s\n",
               (long long)sqlite3_column_int64(st, 0), sqlite3_column_text(st, 1),
               sqlite3_column_text(st, 2), (long long)sqlite3_column_int64(st, 3),
               (long long)sqlite3_column_int64(st, 4), sqlite3_column_text(st, 5),
               provider_id ? provider_id : "-");
        const char *body = (const char *)sqlite3_column_text(st, 7);
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

/* ── `cclaw rename-agent <old> <new> [--timeout N]` ───────────────
 * Operator identity surgery, deliberately CLI-only: an agent cannot rename
 * itself (config-doc M2 deleted that tool arm). Works daemon-up or -down —
 * the quiesce lease is a DB row, so the running daemon honours it at its own
 * turn-open point without any IPC.
 *
 * Flow: take the lease → drain in-flight sessions (refreshing the lease so a
 * long drain can't expire it) → rename both storage domains with SIGINT
 * blocked → tell the agent → release. Every early exit releases the lease;
 * a crash doesn't need to, because the lease expires. */

static volatile sig_atomic_t g_rename_interrupted;
static void rename_sigint(int sig) { (void)sig; g_rename_interrupted = 1; }

/* Wait until the agent has no live-owned busy session. Returns 0 quiesced,
 * -1 interrupted, -2 timed out. */
static int rename_drain(sqlite3 *db, const char *name, const char *holder,
                        int timeout_s) {
    time_t deadline = time(NULL) + timeout_s;
    int64_t last_reported = -1;
    for (;;) {
        char state[32];
        int64_t sid = agent_busy_session(db, name, state, sizeof(state));
        if (sid == 0) return 0;
        if (g_rename_interrupted) return -1;
        if (time(NULL) >= deadline) return -2;
        if (sid != last_reported) {
            printf("waiting for session %lld (state=%s)\n", (long long)sid, state);
            fflush(stdout);
            last_reported = sid;
        }
        /* The drain can outlast the lease it is protecting — refresh every
         * pass so the window it holds is always the full timeout ahead. */
        agent_hold_refresh(db, name, timeout_s, holder);
        sleep(1);
    }
}

/* The agent's own memory prose may name it; nothing else can rewrite that, so
 * hand it the fact at its next turn. Queued on its most recent session — the
 * rename already cascaded, so the row is under the NEW name. */
static void rename_notify(sqlite3 *db, const char *new_name,
                          const char *old_name) {
    int64_t sid = 0;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT id FROM sessions WHERE agent_name=?1"
            " ORDER BY updated_at DESC, id DESC LIMIT 1;", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, new_name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) sid = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    if (sid <= 0) return;                       /* never talked to — nothing to tell */
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Operator renamed you from %s to %s — update your memory if it "
             "mentions your name.", old_name, new_name);
    inbox_insert(db, sid, "system", NULL, msg);
}

int rename_agent_main(int argc, char *argv[]) {
    const char *old_name = (argc >= 4) ? argv[2] : NULL;
    const char *new_name = (argc >= 4) ? argv[3] : NULL;
    int timeout_s = 60;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) timeout_s = atoi(argv[++i]);
        else if (strncmp(argv[i], "--timeout=", 10) == 0) timeout_s = atoi(argv[i] + 10);
    }
    if (!old_name || !new_name || timeout_s <= 0) {
        fprintf(stderr, "usage: cclaw rename-agent <old> <new> [--timeout N]\n");
        return 2;
    }

    sqlite3 *db = verb_db_open();
    if (!db) return 1;

    char holder[40];
    snprintf(holder, sizeof(holder), "cli:%ld", (long)getpid());

    if (agent_hold_acquire(db, old_name, timeout_s, holder) != 0) {
        char who[64] = "?";
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(hold_holder,'?') FROM agents WHERE name=?1"
                " AND hold_until > unixepoch();", -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, old_name, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW)
                snprintf(who, sizeof(who), "%s", sqlite3_column_text(s, 0));
            else
                who[0] = '\0';
            sqlite3_finalize(s);
        }
        if (who[0])
            fprintf(stderr, "error: agent '%s' is already held by %s — "
                            "another rename is in progress\n", old_name, who);
        else
            fprintf(stderr, "error: no such agent '%s'\n", old_name);
        sqlite3_close(db);
        return 1;
    }

    struct sigaction sa = {0}, old_sa;
    sa.sa_handler = rename_sigint;
    sigaction(SIGINT, &sa, &old_sa);

    int rc = 1;
    int drained = rename_drain(db, old_name, holder, timeout_s);
    if (drained == -1) {
        fprintf(stderr, "error: interrupted — nothing was renamed\n");
        goto done;
    }
    if (drained == -2) {
        fprintf(stderr, "error: agent '%s' still busy after %ds — nothing was "
                        "renamed\n", old_name, timeout_s);
        goto done;
    }

    /* The commit + directory move is the one window where an interrupt could
     * split the two storage domains. Block SIGINT across it; the drain above
     * and the notice below are interruptible. */
    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, &prev);

    char agents_dir[PATH_MAX];
    agent_dir_resolve(NULL, sqlite3_db_filename(db, "main"),
                      agents_dir, sizeof(agents_dir));
    int r = agent_rename_full(db, old_name, new_name, 0, agents_dir);
    sigprocmask(SIG_SETMASK, &prev, NULL);

    if (r != 0) {
        const char *why =
            r == -2 ? "an agent with that name already exists" :
            r == -3 ? "invalid new name" :
            r == -4 ? "no such agent" :
            r == -5 ? "SPLIT STATE: DB renamed but the directory move and its "
                      "rollback both failed — fix by hand (see the log)" :
                      "busy or DB error; nothing was renamed";
        fprintf(stderr, "error: rename failed — %s\n", why);
        goto done;
    }

    rename_notify(db, new_name, old_name);
    printf("renamed agent %s -> %s (workspace %s/%s)\n",
           old_name, new_name, agents_dir, new_name);
    rc = 0;

done:
    /* hold_holder rode the rename with the row, so release under whichever
     * name the agent now has. */
    if (agent_hold_release(db, rc == 0 ? new_name : old_name, holder) != 0)
        agent_hold_release(db, old_name, holder);
    sigaction(SIGINT, &old_sa, NULL);
    sqlite3_close(db);
    return rc;
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
                "SELECT id, status, model, session_id, iteration_id,"
                "       datetime(created_at,'unixepoch','localtime'), COALESCE(length(body),0),"
                "       provider_id"
                " FROM llm_responses ORDER BY id DESC LIMIT ?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, n);
            int any = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *provider_id = (const char *)sqlite3_column_text(st, 7);
                printf("#%-5lld %-13s %s session=%lld iter=%lld %s %d bytes provider_id=%s\n",
                       (long long)sqlite3_column_int64(st, 0), sqlite3_column_text(st, 1),
                       sqlite3_column_text(st, 2), (long long)sqlite3_column_int64(st, 3),
                       (long long)sqlite3_column_int64(st, 4), sqlite3_column_text(st, 5),
                       sqlite3_column_int(st, 6), provider_id ? provider_id : "-");
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

/* `cclaw models [query] [--provider NAME] [--refresh]` — the availability
 * catalog listing from the operator side: what the provider advertises, as
 * opposed to the `models` rows routing actually uses. The secret key is loaded because
 * a provider's api_key_env usually resolves to a stored secret, not an env
 * var. Same output as the agent surface, a looser row cap — an operator
 * reading a terminal can take 100 lines, a context window cannot. */
int models_main(int argc, char *argv[]) {
    const char *provider = NULL, *query = NULL;
    int force = 0, page = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--refresh") == 0) force = 1;
        else if (strcmp(argv[i], "--provider") == 0) {
            if (++i >= argc) { fprintf(stderr, "--provider requires a name\n"); return 2; }
            provider = argv[i];
        } else if (strcmp(argv[i], "--page") == 0) {
            if (++i >= argc) { fprintf(stderr, "--page requires a number\n"); return 2; }
            page = atoi(argv[i]);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: cclaw models [query] [--provider NAME] [--page N] [--refresh]\n");
            return 2;
        } else if (!query) {
            query = argv[i];
        }
    }

    char *db_path = util_resolve_db_path();
    if (!db_path) { fprintf(stderr, "error: cannot resolve DB path\n"); return 1; }
    sqlite3 *db = verb_db_open();
    if (!db) { free(db_path); return 1; }
    { uint8_t sk[32]; if (secret_key_load_or_create(db_path, sk) == 0) db_set_secret_key(sk); }
    free(db_path);

    char err[256] = "", *listing = NULL;
    int rc = models_cache_query(db, provider, query, 100, page, force,
                                &listing, err, sizeof(err));
    if (rc != 0) fprintf(stderr, "error: %s\n", err[0] ? err : "probe failed");
    else printf("%s", listing);
    free(listing);
    sqlite3_close(db);
    return rc == 0 ? 0 : 1;
}
