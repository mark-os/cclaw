/* resolve.c — the approval resolution flow: prompt the approver
 * (handle_approval_park), settle the decision (resolve_approval), apply
 * grants, and deliver late decisions. Process-coupled by nature (proc
 * accessors, run_advance re-entry, CLI/daemon prompting) — the db-pure
 * approval row model and summary rendering stay in approval.c. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "resolve.h"

#include "agent_config.h"
#include "agent_define.h"
#include "agent_setup.h"
#include "buf.h"
#include "channel.h"
#include "channel_api.h"
#include "config.h"
#include "cron.h"
#include "db.h"
#include "extension_manifest.h"
#include "log.h"
#include "loop.h"
#include "proc.h"
#include "secret_interp.h"
#include "tool_args.h"
#include "tool_request_config.h"
#include "util.h"
#include "wake.h"

/* ── apply_grant: apply an 'apply'-style capability grant ──────────
 * Extracted from resolve_approval so both the in-window path and the
 * post-window inbox path apply grants identically. Called only for an approved
 * (APPROVAL_ALWAYS) apply approval. Sets *rename_failed if a rename's disk step
 * failed (DB change rolled back); refreshes live caps on success.
 * grant_expires_at: 0 for a permanent grant, else a future unix timestamp
 * (--auto-approve path — see resolve.h). */
static void apply_grant(const Approval *a, const char *agent, int *rename_failed,
                        int64_t grant_expires_at) {
    *rename_failed = 0;
    if (strcmp(a->action, "request_changes") == 0) {
        /* One savepoint applies the whole document — grants, config values,
         * provider upsert (tool_request_config.c). Failure rolls everything
         * back and surfaces as apply-denied, never a partial grant set. */
        if (request_config_changes_apply(proc_db(), agent, a->args_json,
                                         grant_expires_at) != 0) {
            LOG_WARN_("request_changes apply failed");
            *rename_failed = 1; /* generic apply-failed: error result, no grant */
        }
    } else if (strcmp(a->action, "extension_promote") == 0) {
        char *bundle = tool_args_str(proc_db(), a->args_json, "bundle");
        char *ierr = NULL;
        if (!bundle || extension_install(proc_db(), bundle, agent, &ierr) != 0) {
            LOG_WARN_("extension_promote apply failed: %s", ierr ? ierr : "no bundle");
            *rename_failed = 1;
        }
        free(ierr);
        free(bundle);
    } else if (strcmp(a->action, "create_agent") == 0) {
        char *cerr = NULL;
        const char *adir = proc_tool_setup() ? proc_tool_setup()->req_cfg_ctx.agents_dir : NULL;
        if (agent_definition_apply(proc_db(), a->args_json, agent, adir, &cerr) != 0) {
            LOG_WARN_("create_agent apply failed: %s", cerr ? cerr : "?");
            *rename_failed = 1; /* generic apply-failed: error result, no grant */
        }
        free(cerr);
    } else if (strcmp(a->action, "update_agent") == 0) {
        char *cerr = NULL;
        if (agent_definition_update_apply(proc_db(), a->args_json, agent, &cerr) != 0) {
            LOG_WARN_("update_agent apply failed: %s", cerr ? cerr : "?");
            *rename_failed = 1; /* generic apply-failed: error result, no grant */
        }
        free(cerr);
    } else if (strcmp(a->action, "cron_set") == 0) {
        /* Standing job: approved once here, then it fires unattended. The
         * document is the same one cron_set validated before parking. */
        char *cerr = NULL;
        if (cron_upsert(proc_db(), agent, a->session_id, a->args_json,
                        NULL, &cerr) < 0) {
            LOG_WARN_("cron_set apply failed: %s", cerr ? cerr : "?");
            *rename_failed = 1; /* generic apply-failed: error result, no job */
        }
        free(cerr);
    } else if (strcmp(a->action, "rename_agent") == 0) {
        char *nn = tool_args_str(proc_db(), a->args_json, "name");
        char *pr = tool_args_str(proc_db(), a->args_json, "preamble");
        if (nn) {
            int rc = agent_rename(proc_db(), agent, nn, a->session_id);
            if (rc == 0 && proc_tool_setup()) {
                /* Disk rename */
                RequestConfigCtx *rctx = &proc_tool_setup()->req_cfg_ctx;
                if (rctx->agents_dir) {
                    char old_path[512], new_path[512];
                    snprintf(old_path, sizeof(old_path), "%s/%s", rctx->agents_dir, agent);
                    snprintf(new_path, sizeof(new_path), "%s/%s", rctx->agents_dir, nn);
                    struct stat st;
                    if (stat(old_path, &st) == 0) {
                        if (rename(old_path, new_path) != 0) {
                            /* Rollback DB rename on disk failure */
                            agent_rename(proc_db(), nn, agent, a->session_id);
                            *rename_failed = 1;
                            goto rename_done;
                        }
                    }
                }
                /* Optional preamble update */
                if (pr && pr[0]) {
                    const char *sql = "UPDATE agents SET system_prompt=? WHERE name=?";
                    sqlite3_stmt *s;
                    if (sqlite3_prepare_v2(proc_db(), sql, -1, &s, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(s, 1, pr, -1, SQLITE_STATIC);
                        sqlite3_bind_text(s, 2, nn, -1, SQLITE_STATIC);
                        sqlite3_step(s); sqlite3_finalize(s);
                    }
                }
                /* Update live agent name */
                snprintf((char *)rctx->agent_name, 64, "%s", nn);
                setenv("CCLAW_AGENT_NAME", nn, 1);
            }
        }
rename_done:
        free(nn); free(pr);
    }
    /* No caps rebind here: the dispatch loop reloads caps from grants before
     * every tool batch, so the applied grants take effect on the next dispatch. */
}

/* Post-window iff the block sweep already advanced this turn: the frozen
 * tool_call is no longer pending OR the session is no longer awaiting_approval.
 * A genuinely missing tool_call also reads as post-window (deliver via inbox). */
static int approval_is_post_window(int64_t session_id, const char *tool_call_id) {
    char tcs[32] = {0}, ss[32] = {0};
    sqlite3_stmt *s;
    const char *sql =
        "SELECT (SELECT status FROM tool_calls WHERE session_id=?1 AND call_id=?2"
        "        ORDER BY id DESC LIMIT 1),"
        "       (SELECT state FROM sessions WHERE id=?1);";
    if (sqlite3_prepare_v2(proc_db(), sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, session_id);
        if (tool_call_id) sqlite3_bind_text(s, 2, tool_call_id, -1, SQLITE_STATIC);
        else sqlite3_bind_null(s, 2);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(s, 0);
            const char *st = (const char *)sqlite3_column_text(s, 1);
            if (t) snprintf(tcs, sizeof tcs, "%s", t);
            if (st) snprintf(ss, sizeof ss, "%s", st);
        }
        sqlite3_finalize(s);
    }
    return strcmp(tcs, "pending") != 0 || strcmp(ss, "awaiting_approval") != 0;
}

/* Deliver a late decision (block window already lapsed) as a new inbox turn:
 * apply grants here too, but never mutate the (already-answered) turn state or
 * re-run a frozen call out of context. */
static void resolve_approval_post_window(const Approval *a, const char *agent,
                                         ApprovalDecision decision, const char *decided_via,
                                         int64_t grant_expires_at) {
    int approved = (decision != APPROVAL_DENY);
    int is_apply = a->resolve && strcmp(a->resolve, "apply") == 0;
    /* Only the timeout sweep expires an approval. Every other automatic
     * decision (auto:no-approver) is a real denial, and telling the model it
     * "expired without a decision — not a denial" invites it to re-request
     * forever. */
    int expired = decided_via && strcmp(decided_via, "auto:expired") == 0;
    if (is_apply) {
        if (approved && decision == APPROVAL_ALWAYS) {
            int rename_failed = 0;
            apply_grant(a, agent, &rename_failed, grant_expires_at);
            approval_deliver_postwindow(proc_db(), a,
                rename_failed ? APPROVAL_PW_APPLY_DENIED : APPROVAL_PW_APPLY_GRANTED);
        } else {
            approval_deliver_postwindow(proc_db(), a,
                expired ? APPROVAL_PW_EXPIRED : APPROVAL_PW_APPLY_DENIED);
        }
    } else {
        if (approved)
            approval_deliver_postwindow(proc_db(), a, APPROVAL_PW_RERUN_APPROVED);
        else
            approval_deliver_postwindow(proc_db(), a,
                expired ? APPROVAL_PW_EXPIRED : APPROVAL_PW_RERUN_DENIED);
    }
}

/* ── resolve_approval: approve/deny a parked approval ────────── */

void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via,
                      int64_t grant_expires_at) {
    int approved = (decision != APPROVAL_DENY);
    Approval *a = approval_resolve(proc_db(), approval_id, approved, decided_via);
    if (!a) return;

    int64_t session_id = a->session_id;
    const char *agent = session_get_agent_name(proc_db(), session_id);

    /* Block window already lapsed → deliver async, leave the turn untouched. */
    if (approval_is_post_window(session_id, a->tool_call_id)) {
        resolve_approval_post_window(a, agent, decision, decided_via, grant_expires_at);
        free((char *)agent);
        approval_free(a);
        wake_session(session_id);
        return;
    }

    /* Dispatch on the approval's resolve strategy. */
    if (!a->resolve || strcmp(a->resolve, "rerun") == 0) {
        /* ── "rerun": the frozen tool_call proceeds on approval. ── */
        if (decision == APPROVAL_DENY) {
            if (a->tool_call_id) {
                char buf[160];
                snprintf(buf, sizeof(buf), "error: %s denied (%s)", a->action, decided_via);
                ToolResult tr = { .tool_call_id = a->tool_call_id, .content = buf };
                Message msg = { .role = ROLE_TOOL, .tool_result = &tr,
                                .tool_name = a->action, .is_error = 1 };
                entry_append_with_iteration(proc_db(), session_id, &msg, 0);
                db_tool_call_set_status(proc_db(), session_id, a->tool_call_id, "done", decided_via);
            }
        } else if (decision == APPROVAL_ALWAYS && agent) {
            if (a->action && strcmp(a->action, "sensitive") == 0) {
                /* Sensitivity axis: no standing grant may satisfy a sensitive
                 * target — ALWAYS is coerced to ONCE (specs/trust.md). The
                 * approval still passes this one frozen call; the next call
                 * to the same target parks again. */
                LOG_INFO_("approval always coerced to once reason=sensitive tool=%s",
                          a->tool_name ? a->tool_name : "?");
            } else if (a->action && strcmp(a->action, "secret_bind") == 0) {
                /* Approve & bind: a durable binding needs a static target —
                 * only a url-carrying call has one. shell/js calls coerce to
                 * ONCE (nothing standing is minted from an unattributable
                 * target); pre-seed those with `cclaw secret-bind`. */
                char host[254];
                if (a->tool_name && strcmp(a->tool_name, "web_fetch") == 0 &&
                    a->args_json &&
                    web_args_url_host(proc_db(), a->args_json, host, sizeof(host))) {
                    size_t un = 0;
                    char **names = secret_placeholder_names(a->args_json, &un);
                    for (size_t i = 0; i < un; i++) {
                        if (db_secret_host_bind(proc_db(), names[i], host) == 0)
                            LOG_INFO_("secret binding recorded name=%s host=%s via=approval",
                                      names[i], host);
                    }
                    secret_names_free(names, un);
                } else {
                    LOG_INFO_("approval always coerced to once reason=secret_bind_no_static_target tool=%s",
                              a->tool_name ? a->tool_name : "?");
                }
            } else {
                /* "Allow and stop asking" — flip the standing mode to silent. */
                agent_config_set_tool_mode(proc_db(), agent, a->action, "silent");
            }
        }
        session_set_state(proc_db(), session_id, "tool_running");
        free((char *)agent);
        approval_free(a);
        wake_session(session_id);
        run_advance(session_id);
        return;
    }

    /* ── "apply": the tool's side effect is applied here, not re-run. ── */

    /* "once" is incoherent for apply-style approvals (ambient capabilities
     * have no single-use semantics). Reject as error. */
    if (decision == APPROVAL_ONCE) {
        char err[256];
        snprintf(err, sizeof(err),
                 "error: once-approval invalid for %s", a->action);
        if (a->tool_call_id) {
            ToolResult tr = { .tool_call_id = a->tool_call_id, .content = err };
            Message msg = { .role = ROLE_TOOL, .tool_result = &tr,
                            .tool_name = a->tool_name, .is_error = 1 };
            entry_append_with_iteration(proc_db(), session_id, &msg, 0);
            db_tool_call_set_status(proc_db(), session_id, a->tool_call_id, "done", decided_via);
        }
        session_set_state(proc_db(), session_id, "tool_running");
        free((char *)agent);
        approval_free(a);
        wake_session(session_id);
        run_advance(session_id);
        return;
    }

    int rename_failed = 0;
    if (decision == APPROVAL_ALWAYS)
        apply_grant(a, agent, &rename_failed, grant_expires_at);

    /* Build tool result message */
    char result_buf[256];
    if (rename_failed)
        snprintf(result_buf, sizeof(result_buf), "error: %s failed, rolled back",
                 a->action ? a->action : "apply");
    else if (decision == APPROVAL_ALWAYS && a->action &&
             strcmp(a->action, "extension_promote") == 0)
        snprintf(result_buf, sizeof(result_buf),
                 "approved: extension_promote — registered; its tools are "
                 "granted to you and callable immediately");
    else if (decision == APPROVAL_ALWAYS)
        snprintf(result_buf, sizeof(result_buf), "approved: %s", a->action);
    else
        snprintf(result_buf, sizeof(result_buf), "denied (%s): %s",
                 decided_via, a->action);

    if (a->tool_call_id) {
        ToolResult tr = { .tool_call_id = a->tool_call_id, .content = result_buf };
        Message msg = { .role = ROLE_TOOL, .tool_result = &tr,
                        .tool_name = a->tool_name,
                        .is_error = (decision == APPROVAL_DENY || rename_failed) };
        entry_append_with_iteration(proc_db(), session_id, &msg, 0);
        db_tool_call_set_status(proc_db(), session_id, a->tool_call_id, "done", decided_via);
    }

    session_set_state(proc_db(), session_id, "tool_running");
    free((char *)agent);
    approval_free(a);
    wake_session(session_id);
    run_advance(session_id);
}

/* ── deferred auto-decisions ─────────────────────────────────────────
 * An automatic decision (--auto-approve, or the auto-deny when there is no
 * approver at all) is reached from inside handle_approval_park, where the
 * parked tool_call is still CAS-claimed 'running' by the dispatch loop.
 * Resolving there makes approval_is_post_window() read the decision as *late*:
 * the model gets an inbox notice, the call never gets a tool result, and the
 * loop then unclaims it back to 'pending' — an orphan that the next run
 * re-dispatches with its original arguments. So record the decision and let
 * the dispatch loop apply it once the claim is released. */
static struct {
    int64_t id;                 /* 0 = nothing deferred */
    ApprovalDecision decision;
    int64_t grant_expires_at;
    char via[24];
} g_deferred;

static void approval_defer_decision(int64_t approval_id, ApprovalDecision decision,
                                    const char *decided_via, int64_t grant_expires_at) {
    g_deferred.id = approval_id;
    g_deferred.decision = decision;
    g_deferred.grant_expires_at = grant_expires_at;
    snprintf(g_deferred.via, sizeof(g_deferred.via), "%s", decided_via);
}

void approval_flush_deferred(void) {
    if (!g_deferred.id) return;
    int64_t id = g_deferred.id;
    ApprovalDecision decision = g_deferred.decision;
    int64_t expires = g_deferred.grant_expires_at;
    char via[sizeof(g_deferred.via)];
    snprintf(via, sizeof(via), "%s", g_deferred.via);
    /* Clear before resolving: resolve_approval re-enters run_advance, which
     * can park (and defer) again. */
    g_deferred.id = 0;
    resolve_approval(id, decision, via, expires);
}

/* ── handle_approval_park: prompt the approver ────────────────── */

void handle_approval_park(int64_t session_id) {
    Approval *a = approval_get_pending(proc_db(), session_id);
    if (!a) return;

    if (!proc_is_daemon()) {
        /* CLI mode */
        if (proc_auto_approve()) {
            /* --auto-approve: answer immediately, never prompt. "rerun"
             * approvals get a single-use ONCE; "apply" grants (ambient
             * capabilities have no once semantics — see resolve_approval)
             * get ALWAYS with a self-expiring deadline so no durable config
             * is left behind. Applies to any session in the CLI tree,
             * including sub-agents. */
            int is_apply = a->resolve && strcmp(a->resolve, "apply") == 0;
            if (is_apply) {
                int64_t expires = time(NULL) + approval_timeout_seconds(proc_db());
                approval_defer_decision(a->id, APPROVAL_ALWAYS, "cli:auto-approve", expires);
            } else {
                approval_defer_decision(a->id, APPROVAL_ONCE, "cli:auto-approve", 0);
            }
            approval_free(a);
            return;
        }
        if (!isatty(STDIN_FILENO)) {
            /* Non-interactive (-p mode): auto-deny */
            approval_defer_decision(a->id, APPROVAL_DENY, "auto:no-approver", 0);
            approval_free(a);
            return;
        }
        /* Interactive: prompt user. Name the asking session when it's a
         * sub-agent (session_id != proc_cli_session()) — otherwise a launch_agent
         * child's approval request is indistinguishable from the root's. */
        fprintf(stdout, "\n\033[1mApproval required\033[0m");
        if (session_id != proc_cli_session()) {
            char *sub_agent = session_get_agent_name(proc_db(), session_id);
            fprintf(stdout, " (sub-agent %s, session %lld)",
                    sub_agent ? sub_agent : "?", (long long)session_id);
            free(sub_agent);
        }
        {
            char *summary = approval_format_summary(proc_db(), a);
            fprintf(stdout, ": %s", summary ? summary : "?");
            free(summary);
        }
        if (a->resolve && strcmp(a->resolve, "apply") == 0)
            fprintf(stdout, "\nGrant? (y/n): ");
        else
            fprintf(stdout, "\nApprove? (y=always / o=once / n=no): ");
        fflush(stdout);
        proc_set_cli_turn_active(0);  /* unblock input loop for the y/n read */
        /* The actual y/n is read back in the main CLI input loop. */
        approval_free(a);
        return;
    }

    /* Daemon mode: enqueue outbox prompt if session has channel, else auto-deny.
     * channel_name/chat_id are read and finalized *before* the outbox
     * INSERT so this connection never holds an open SELECT across a write —
     * see specs note on read->write snapshot upgrades (instant SQLITE_BUSY). */
    const char *sql = "SELECT channel_name, chat_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    int has_channel = 0;
    char ch_name[64] = {0};
    char ch_id[64] = {0};
    if (sqlite3_prepare_v2(proc_db(), sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *ch = (const char *)sqlite3_column_text(stmt, 0);
            const char *cid = (const char *)sqlite3_column_text(stmt, 1);
            if (ch && ch[0]) {
                has_channel = 1;
                snprintf(ch_name, sizeof(ch_name), "%s", ch);
                snprintf(ch_id, sizeof(ch_id), "%s", cid ? cid : "0");
            }
        }
        sqlite3_finalize(stmt);
    }
    if (has_channel) {
        /* Route to the channel's configured admin destination(s), not the
         * requesting session's own chat — a session may be exposed on a
         * channel/chat where the human watching it isn't the one who should
         * decide grants (e.g. a sub-agent surfaced on a groupchat). Falls
         * back to the session's own chat_id when no admin_ids are
         * configured, so the common 1:1 setup needs no extra config. */
        char *admins = channel_config_get(proc_db(), ch_name, "admin_ids");
        if (admins && !admins[0]) { free(admins); admins = NULL; }

        char *agent = session_get_agent_name(proc_db(), session_id);
        char *summary = approval_format_summary(proc_db(), a);
        Buf pb = {0};
        /* The approval id is part of the prompt text, not just the button
         * payload: a channel without inline buttons (Discord) decides with
         * "/approve <id>", and the admin must see the id and the full summary
         * in the same message they are deciding from. */
        buf_appendf(&pb, "**Approval #%lld** — agent %s (session %lld, channel %s):\n%s"
                         "\nDecide here: /approve %lld or /deny %lld",
                    (long long)a->id, agent ? agent : "?",
                    (long long)session_id, ch_name, summary ? summary : "?",
                    (long long)a->id, (long long)a->id);
        free(agent);
        free(summary);
        char *prompt = buf_take(&pb);
        if (!prompt) prompt = strdup("approval requested");

        /* Buttons are labeled with their EFFECT, and a decision that cannot
         * happen is not offered: "apply" approvals have no once semantics
         * (resolve_approval rejects APPROVAL_ONCE outright), and ALWAYS is
         * coerced to ONCE for sensitivity parks and for secret_bind parks
         * with no static target (shell/js) — mirror resolve_approval's
         * coercion conditions exactly, or the button lies. */
        char keyboard[320];
        int is_apply = a->resolve && strcmp(a->resolve, "apply") == 0;
        int coerced = 0, can_bind = 0;
        if (a->action && strcmp(a->action, "sensitive") == 0) {
            coerced = 1;
        } else if (a->action && strcmp(a->action, "secret_bind") == 0) {
            char bh[254];
            can_bind = a->tool_name && strcmp(a->tool_name, "web_fetch") == 0 &&
                       a->args_json &&
                       web_args_url_host(proc_db(), a->args_json, bh, sizeof(bh));
            coerced = !can_bind;
        }
        if (is_apply) {
            snprintf(keyboard, sizeof(keyboard),
                     "[[{\"text\":\"Grant\",\"callback_data\":\"appr:%lld:yes\"},"
                     "{\"text\":\"Deny\",\"callback_data\":\"appr:%lld:no\"}]]",
                     (long long)a->id, (long long)a->id);
        } else if (coerced) {
            snprintf(keyboard, sizeof(keyboard),
                     "[[{\"text\":\"Allow once\",\"callback_data\":\"appr:%lld:once\"},"
                     "{\"text\":\"Deny\",\"callback_data\":\"appr:%lld:no\"}]]",
                     (long long)a->id, (long long)a->id);
        } else {
            snprintf(keyboard, sizeof(keyboard),
                     "[[{\"text\":\"%s\",\"callback_data\":\"appr:%lld:yes\"},"
                     "{\"text\":\"Once\",\"callback_data\":\"appr:%lld:once\"},"
                     "{\"text\":\"Deny\",\"callback_data\":\"appr:%lld:no\"}]]",
                     can_bind ? "Approve & bind host" : "Always allow",
                     (long long)a->id, (long long)a->id, (long long)a->id);
        }

        /* Two shapes: the admin fan-out addresses *user* ids (to_user=1, the
         * handler resolves a DM), the fallback addresses the session's own
         * chat id. See channel_notify_admins. */
        const char *ins_sql =
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4,"
            " 'keyboard', json(?5), 'to_user', 1));";
        const char *ins_chat_sql =
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4, 'keyboard', json(?5)));";
        if (admins && admins[0]) {
            char *ids[CHANNEL_ADMIN_IDS_MAX];
            int n_ids = split_and_trim(admins, ids, CHANNEL_ADMIN_IDS_MAX);
            for (int i = 0; i < n_ids; i++) {
                sqlite3_stmt *ins;
                if (sqlite3_prepare_v2(proc_db(), ins_sql, -1, &ins, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ins, 1, ch_name, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(ins, 2, session_id);
                    sqlite3_bind_text(ins, 3, ids[i], -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 4, prompt, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 5, keyboard, -1, SQLITE_STATIC);
                    sqlite3_step(ins); sqlite3_finalize(ins);
                }
            }
        } else {
            sqlite3_stmt *ins;
            if (sqlite3_prepare_v2(proc_db(), ins_chat_sql, -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, ch_name, -1, SQLITE_STATIC);
                sqlite3_bind_int64(ins, 2, session_id);
                sqlite3_bind_text(ins, 3, ch_id, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, prompt, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 5, keyboard, -1, SQLITE_STATIC);
                sqlite3_step(ins); sqlite3_finalize(ins);
            }
        }
        free(admins);
        free(prompt);
        if (proc_cfg() && proc_cfg()->db_path) channel_outbox_wake(proc_cfg()->db_path, ch_name);
    }
    if (!has_channel) {
        /* No channel binding — fail-closed: auto-deny */
        approval_defer_decision(a->id, APPROVAL_DENY, "auto:no-approver", 0);
    }
    approval_free(a);
}
