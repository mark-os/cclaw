#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "dashboard.h"
#include "util.h"
#include "admin_api.h"
#include "buf.h"
#include "civetweb.h"
#include "config_registry.h"
#include "db.h"
#include "approval.h"
#include "wake.h"
#include "web.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Server-rendered admin dashboard: one GET page, one POST action endpoint,
 * zero client JS. Auth is a single generated token — cookie for the session,
 * ?token= for the first visit (LAN threat model; TLS/tunnelling later). */

/* A memory note can be 5000+ characters, and form encoding triples the
 * bytes of anything non-alphanumeric. */
#define FORM_MAX 32768

static sqlite3 *s_db;

/* ── helpers ─────────────────────────────────────────────────── */

/* Every DB string goes through this before touching the page. */
static void buf_html(Buf *b, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
        case '&':  buf_append(b, "&amp;", 5); break;
        case '<':  buf_append(b, "&lt;", 4); break;
        case '>':  buf_append(b, "&gt;", 4); break;
        case '"':  buf_append(b, "&quot;", 6); break;
        case '\'': buf_append(b, "&#39;", 5); break;
        default:   buf_append(b, s, 1);
        }
    }
}

static int token_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (la == lb) ? 0 : 1;
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)(a[i] ^ b[i % (lb ? lb : 1)]);
    return diff == 0;
}

static int ensure_token(sqlite3 *db) {
    char *cur = config_get(db, "web_admin_token");
    int have = (cur && cur[0]);
    free(cur);
    if (have) return 0;
    unsigned char rnd[16];
    if (util_random(rnd, sizeof(rnd)) != 0) return -1;
    char tok[33];
    for (int i = 0; i < 16; i++)
        snprintf(tok + i * 2, 3, "%02x", rnd[i]);
    return config_set(db, "web_admin_token", tok);
}

/* 0 = unauthenticated, 1 = valid cookie, 2 = valid ?token= (set cookie) */
int dashboard_auth_check(struct mg_connection *conn, int allow_query) {
    char *want = config_get(s_db, "web_admin_token");
    if (!want || !want[0]) { free(want); return 0; }
    int ok = 0;
    char got[80];
    const char *cookie = mg_get_header(conn, "Cookie");
    if (cookie && mg_get_cookie(cookie, "cclaw_admin", got, sizeof(got)) > 0 &&
        token_eq(got, want))
        ok = 1;
    if (!ok && allow_query) {
        const struct mg_request_info *ri = mg_get_request_info(conn);
        if (ri->query_string &&
            mg_get_var(ri->query_string, strlen(ri->query_string),
                       "token", got, sizeof(got)) > 0 &&
            token_eq(got, want))
            ok = 2;
    }
    free(want);
    return ok;
}

static int resp_plain(struct mg_connection *conn, int status, const char *st_text,
                      const char *body) {
    mg_printf(conn,
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        status, st_text, strlen(body), body);
    return status;
}

/* 303 back to the page; set_cookie non-NULL on the ?token= first visit. */
static int resp_303(struct mg_connection *conn, const char *location,
                    const char *set_cookie_token) {
    mg_printf(conn, "HTTP/1.1 303 See Other\r\nLocation: %s\r\n"
                    "Cache-Control: no-store\r\n", location);
    if (set_cookie_token)
        mg_printf(conn, "Set-Cookie: cclaw_admin=%s; Path=/; HttpOnly; SameSite=Strict\r\n",
                  set_cookie_token);
    mg_printf(conn, "Content-Length: 0\r\n\r\n");
    return 303;
}

/* ── page sections ───────────────────────────────────────────── */

static void render_providers_models(Buf *b) {
    AdminModel *models = NULL;
    size_t nm = 0;
    admin_list_models(s_db, &models, &nm);

    buf_appendf(b, "<h2>Providers &amp; Models</h2>");

    /* Query providers directly for full detail */
    sqlite3_stmt *pst;
    const char *psql = "SELECT name, base_url, endpoint_type, api_key_env"
                       " FROM providers ORDER BY priority;";
    if (sqlite3_prepare_v2(s_db, psql, -1, &pst, NULL) != SQLITE_OK) {
        admin_models_free(models, nm);
        return;
    }

    while (sqlite3_step(pst) == SQLITE_ROW) {
        const char *prov_name = (const char *)sqlite3_column_text(pst, 0);
        const char *base_url = (const char *)sqlite3_column_text(pst, 1);
        const char *endpoint_type = (const char *)sqlite3_column_text(pst, 2);
        const char *api_key_env = (const char *)sqlite3_column_text(pst, 3);
        if (!prov_name) continue;

        /* Resolve key presence */
        int has_key = 0;
        if (api_key_env && api_key_env[0]) {
            const char *env = getenv(api_key_env);
            if (env && env[0]) {
                has_key = 1;
            } else {
                char *sec = db_secret_get_system(s_db, api_key_env);
                if (sec) { has_key = 1; free(sec); }
            }
        }

        /* Provider header row */
        buf_appendf(b, "<table><tr style=\"background:#eee\"><td colspan=6><b>");
        buf_html(b, prov_name);
        buf_appendf(b, "</b> &mdash; ");
        buf_html(b, base_url);
        buf_appendf(b, " [");
        buf_html(b, endpoint_type);
        buf_appendf(b, "] key: %s", has_key ? "&#10003;" : "&#10007;");
        /* Set key form */
        buf_appendf(b, " <form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=set_key>"
                       "<input type=hidden name=provider value=\"");
        buf_html(b, prov_name);
        buf_appendf(b, "\"><input type=password name=key size=20 placeholder=\"");
        buf_html(b, api_key_env ? api_key_env : "key");
        buf_appendf(b, "\"><button>set</button></form>");
        /* Remove button */
        buf_appendf(b, " <form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=remove_provider>"
                       "<input type=hidden name=provider value=\"");
        buf_html(b, prov_name);
        buf_appendf(b, "\"><button>remove</button></form>");
        buf_appendf(b, "</td></tr>");

        /* Model header */
        buf_appendf(b, "<tr><th>model</th><th>status</th><th>ctx</th>"
                       "<th>fails</th><th></th></tr>");

        /* Models for this provider */
        for (size_t mi = 0; mi < nm; mi++) {
            if (!models[mi].provider || strcmp(models[mi].provider, prov_name) != 0)
                continue;
            AdminModel *m = &models[mi];
            buf_appendf(b, "<tr><td>");
            buf_html(b, m->model);
            buf_appendf(b, "</td><td>");
            buf_html(b, m->status);
            if (m->degraded_left > 0)
                buf_appendf(b, " (%ds)", m->degraded_left);
            buf_appendf(b, "</td><td>%d</td><td>%lld</td><td>",
                        m->context_window, (long long)m->consec_failures);
            /* Set as default button */
            buf_appendf(b, "<form class=i method=post action=/admin/act>"
                           "<input type=hidden name=action value=set_default_model>"
                           "<input type=hidden name=model_id value=\"");
            buf_html(b, m->id);
            buf_appendf(b, "\"><button>set as default</button></form> ");
            /* Disable/enable button */
            buf_appendf(b, "<form class=i method=post action=/admin/act>"
                           "<input type=hidden name=action value=toggle_model>"
                           "<input type=hidden name=model_id value=\"");
            buf_html(b, m->id);
            buf_appendf(b, "\"><button>%s</button></form>",
                        (m->status && strcmp(m->status, "disabled") == 0)
                        ? "enable" : "disable");
            buf_appendf(b, "</td></tr>");
        }

        /* Add model form within this provider */
        buf_appendf(b, "<tr><td colspan=6>"
                       "<form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=add_model>"
                       "<input type=hidden name=provider value=\"");
        buf_html(b, prov_name);
        buf_appendf(b, "\"><input name=model size=28 placeholder=\"model string\">"
                       " <input name=context_window size=8 placeholder=\"ctx\">"
                       " <button>add model</button></form></td></tr>");
        buf_appendf(b, "</table>");
    }
    sqlite3_finalize(pst);

    /* Add provider form */
    buf_appendf(b, "<h3>Add provider</h3>"
                   "<form method=post action=/admin/act>"
                   "<input type=hidden name=action value=add_provider>"
                   "<input name=name size=16 placeholder=\"name\" required>"
                   " <input name=base_url size=36 placeholder=\"https://...\" required>"
                   " <select name=endpoint_type>"
                   "<option value=openai>openai</option>"
                   "<option value=gemini>gemini</option>"
                   "<option value=anthropic>anthropic</option>"
                   "<option value=responses>responses</option>"
                   "<option value=gemini_interactions>gemini_interactions</option>"
                   "</select>"
                   " <input name=api_key_env size=20 placeholder=\"API_KEY_ENV\">"
                   " <button>add</button></form>");

    admin_models_free(models, nm);
}

static void render_agents(Buf *b) {
    AdminAgent *agents = NULL;
    size_t na = 0;
    admin_list_agents(s_db, &agents, &na);
    AdminModel *models = NULL;
    size_t nm = 0;
    admin_list_models(s_db, &models, &nm);

    buf_appendf(b, "<h2>Agents</h2><table><tr><th>name</th><th>primary model</th>"
                   "<th>secondary model</th><th>max_iter</th><th>sandbox</th><th></th></tr>");
    for (size_t i = 0; i < na; i++) {
        AdminAgent *a = &agents[i];
        buf_appendf(b, "<tr><td>");
        buf_html(b, a->name);
        buf_appendf(b, "</td><td colspan=2>"
                       "<form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=set_agent_model>"
                       "<input type=hidden name=agent value=\"");
        buf_html(b, a->name);
        buf_appendf(b, "\"><select name=primary_model><option value=\"\">(default)</option>");
        for (size_t mi = 0; mi < nm; mi++) {
            buf_appendf(b, "<option value=\"");
            buf_html(b, models[mi].id);
            buf_appendf(b, "\"%s>",
                        (a->primary_model && models[mi].id && strcmp(a->primary_model, models[mi].id) == 0)
                        ? " selected" : "");
            buf_html(b, models[mi].model);
            buf_appendf(b, " @ ");
            buf_html(b, models[mi].provider);
            buf_appendf(b, "</option>");
        }
        buf_appendf(b, "</select> <select name=secondary_model><option value=\"\">(none)</option>");
        for (size_t mi = 0; mi < nm; mi++) {
            buf_appendf(b, "<option value=\"");
            buf_html(b, models[mi].id);
            buf_appendf(b, "\"%s>",
                        (a->secondary_model && models[mi].id && strcmp(a->secondary_model, models[mi].id) == 0)
                        ? " selected" : "");
            buf_html(b, models[mi].model);
            buf_appendf(b, " @ ");
            buf_html(b, models[mi].provider);
            buf_appendf(b, "</option>");
        }
        buf_appendf(b, "</select> <button>assign</button></form>");
        buf_appendf(b, "</td><td>%d</td><td>", a->max_iterations);
        buf_html(b, a->sandbox_profile);
        buf_appendf(b, "</td><td>"
                       "<form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=create_session>"
                       "<input type=hidden name=agent value=\"");
        buf_html(b, a->name);
        buf_appendf(b, "\"><button>new session</button></form></td></tr>");
    }
    buf_appendf(b, "</table>");

    admin_agents_free(agents, na);
    admin_models_free(models, nm);
}

static void render_sessions(Buf *b) {
    AdminSession *sessions = NULL;
    size_t ns = 0;
    admin_list_sessions(s_db, 30, &sessions, &ns);
    AdminChannel *channels = NULL;
    size_t nc = 0;
    admin_list_channels(s_db, &channels, &nc);

    buf_appendf(b, "<h2>Sessions</h2><table><tr><th>id</th><th>agent</th>"
                   "<th>channel</th><th>chat_id</th><th>state</th>"
                   "<th>created</th><th></th></tr>");
    for (size_t i = 0; i < ns; i++) {
        AdminSession *s = &sessions[i];
        buf_appendf(b, "<tr><td>%lld</td><td>", (long long)s->id);
        buf_html(b, s->agent_name);
        buf_appendf(b, "</td><td>");
        buf_html(b, s->channel_name);
        buf_appendf(b, "</td><td>");
        buf_html(b, s->chat_id);
        buf_appendf(b, "</td><td>");
        buf_html(b, s->state);
        buf_appendf(b, "</td><td>");
        buf_html(b, s->created_at);
        buf_appendf(b, "</td><td>"
                       "<form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=attach_channel>"
                       "<input type=hidden name=session_id value=%lld>"
                       "<select name=channel_name><option value=\"\">--</option>",
                    (long long)s->id);
        for (size_t ci = 0; ci < nc; ci++) {
            buf_appendf(b, "<option value=\"");
            buf_html(b, channels[ci].name);
            buf_appendf(b, "\">");
            buf_html(b, channels[ci].name);
            buf_appendf(b, "</option>");
        }
        buf_appendf(b, "</select>"
                       "<input name=channel_id size=12 placeholder=\"channel_id\">"
                       " <button>attach</button></form></td></tr>");
    }
    buf_appendf(b, "</table>");

    admin_sessions_free(sessions, ns);
    admin_channels_free(channels, nc);
}

static void render_channels(Buf *b) {
    AdminChannel *channels = NULL;
    size_t nc = 0;
    admin_list_channels(s_db, &channels, &nc);
    AdminAgent *agents = NULL;
    size_t na = 0;
    admin_list_agents(s_db, &agents, &na);

    buf_appendf(b, "<h2>Channels</h2><table><tr><th>name</th><th>extension</th>"
                   "<th>type</th><th>status</th><th>route</th><th></th></tr>");
    for (size_t i = 0; i < nc; i++) {
        AdminChannel *c = &channels[i];
        buf_appendf(b, "<tr><td>");
        buf_html(b, c->name);
        buf_appendf(b, "</td><td>");
        buf_html(b, c->extension_name);
        buf_appendf(b, "</td><td>");
        buf_html(b, c->type);
        buf_appendf(b, "</td><td>");
        buf_html(b, c->status);
        buf_appendf(b, "</td><td>");
        if (c->route_agent) {
            buf_html(b, c->route_agent);
            if (c->route_session > 0)
                buf_appendf(b, " (#%lld)", (long long)c->route_session);
        } else {
            buf_appendf(b, "<em>none</em>");
        }
        buf_appendf(b, "</td><td>"
                       "<form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=set_channel_route>"
                       "<input type=hidden name=channel_name value=\"");
        buf_html(b, c->name);
        buf_appendf(b, "\"><select name=agent_name><option value=\"\">(none)</option>");
        for (size_t ai = 0; ai < na; ai++) {
            buf_appendf(b, "<option value=\"");
            buf_html(b, agents[ai].name);
            buf_appendf(b, "\"%s>",
                        (c->route_agent && agents[ai].name &&
                         strcmp(c->route_agent, agents[ai].name) == 0) ? " selected" : "");
            buf_html(b, agents[ai].name);
            buf_appendf(b, "</option>");
        }
        buf_appendf(b, "</select> <button>route</button></form></td></tr>");
    }
    buf_appendf(b, "</table>");

    admin_channels_free(channels, nc);
    admin_agents_free(agents, na);
}

static void render_grants(Buf *b, const char *agent) {
    buf_appendf(b, "<h2>Grants</h2><form class=i method=get action=/admin>"
                   "<select name=agent>");
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, "SELECT name FROM agents ORDER BY name",
                           -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            if (!name) continue;
            buf_appendf(b, "<option value=\"");
            buf_html(b, name);
            buf_appendf(b, "\"%s>", (agent && strcmp(agent, name) == 0) ? " selected" : "");
            buf_html(b, name);
            buf_appendf(b, "</option>");
        }
        sqlite3_finalize(st);
    }
    buf_appendf(b, "</select><button>view</button></form>");
    if (!agent || !agent[0]) {
        buf_appendf(b, "<p>select an agent</p>");
        return;
    }

    AdminGrant *list = NULL;
    size_t n = 0;
    admin_list_grants(s_db, agent, &list, &n);
    buf_appendf(b, "<table><tr><th>kind</th><th>value</th><th></th></tr>");
    for (size_t i = 0; i < n; i++) {
        buf_appendf(b, "<tr><td>");
        buf_html(b, list[i].kind);
        buf_appendf(b, "</td><td>");
        buf_html(b, list[i].value);
        buf_appendf(b, "</td><td><form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=revoke>"
                       "<input type=hidden name=agent value=\"");
        buf_html(b, agent);
        buf_appendf(b, "\"><input type=hidden name=grant_id value=%lld>"
                       "<button>revoke</button></form></td></tr>",
                    (long long)list[i].id);
    }
    buf_appendf(b, "</table>");
    admin_grants_free(list, n);

    buf_appendf(b, "<form class=i method=post action=/admin/act>"
                   "<input type=hidden name=action value=grant>"
                   "<input type=hidden name=agent value=\"");
    buf_html(b, agent);
    buf_appendf(b, "\"><select name=kind><option>tool</option><option>host</option>"
                   "<option>read_path</option><option>write_path</option></select>"
                   "<input name=value size=32 list=tools>"
                   "<button>grant</button></form><datalist id=tools>");
    char **tools = NULL;
    size_t tn = 0;
    admin_list_tool_names(s_db, &tools, &tn);
    for (size_t i = 0; i < tn; i++) {
        buf_appendf(b, "<option value=\"");
        buf_html(b, tools[i]);
        buf_appendf(b, "\">");
    }
    buf_appendf(b, "</datalist>");
    admin_tool_names_free(tools, tn);
}

static void render_approval_row(Buf *b, const AdminApproval *a) {
    buf_appendf(b, "<tr><td>%lld</td><td>%lld</td><td>",
                (long long)a->id, (long long)a->session_id);
    buf_html(b, a->agent_name);
    buf_appendf(b, "</td><td>");
    buf_html(b, a->tool_name);
    buf_appendf(b, "</td><td>");
    buf_html(b, a->park_reason);
    buf_appendf(b, "</td><td class=args>");
    buf_html(b, a->args_json);
    buf_appendf(b, "</td><td>");
}

static void render_approvals(Buf *b) {
    AdminApproval *list = NULL;
    size_t n = 0;
    admin_list_pending_approvals(s_db, NULL, &list, &n);
    buf_appendf(b, "<h2>Pending approvals</h2>");
    if (n == 0)
        buf_appendf(b, "<p>none</p>");
    else
        buf_appendf(b, "<table><tr><th>id</th><th>session</th><th>agent</th>"
                       "<th>tool</th><th>action</th><th>args</th><th></th></tr>");
    for (size_t i = 0; i < n; i++) {
        render_approval_row(b, &list[i]);
        static const struct { const char *val, *label; } dec[] = {
            {"once", "approve once"}, {"always", "always"}, {"deny", "deny"}};
        for (size_t d = 0; d < 3; d++) {
            buf_appendf(b, "<form class=i method=post action=/admin/act>"
                           "<input type=hidden name=action value=approve>"
                           "<input type=hidden name=approval_id value=%lld>"
                           "<input type=hidden name=decision value=%s>"
                           "<button>%s</button></form> ",
                        (long long)list[i].id, dec[d].val, dec[d].label);
        }
        buf_appendf(b, "</td></tr>");
    }
    if (n) buf_appendf(b, "</table>");
    admin_approvals_free(list, n);

    list = NULL;
    n = 0;
    admin_list_denied_approvals(s_db, NULL, 20, &list, &n);
    buf_appendf(b, "<h2>Denied history</h2>");
    if (n == 0)
        buf_appendf(b, "<p>none</p>");
    else
        buf_appendf(b, "<table><tr><th>id</th><th>session</th><th>agent</th>"
                       "<th>tool</th><th>action</th><th>args</th><th></th></tr>");
    for (size_t i = 0; i < n; i++) {
        render_approval_row(b, &list[i]);
        buf_appendf(b, "<form class=i method=post action=/admin/act>"
                       "<input type=hidden name=action value=grant_history>"
                       "<input type=hidden name=approval_id value=%lld>"
                       "<button>grant</button></form></td></tr>",
                    (long long)list[i].id);
    }
    if (n) buf_appendf(b, "</table>");
    admin_approvals_free(list, n);
}

/* ── memory ──────────────────────────────────────────────────── */

static void mem_form_open(Buf *b, const char *action, long long block_id) {
    buf_appendf(b, "<form class=i method=post action=/admin/act>"
                   "<input type=hidden name=action value=%s>"
                   "<input type=hidden name=block_id value=%lld>", action, block_id);
}

/* <option> per agent; `skip_block` > 0 leaves out agents already attached. */
static void mem_agent_options(Buf *b, long long skip_block) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db,
            "SELECT name FROM agents WHERE ?1=0 OR name NOT IN"
            " (SELECT agent_name FROM agent_memory_blocks WHERE block_id=?1)"
            " ORDER BY name", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, skip_block);
    while (sqlite3_step(st) == SQLITE_ROW) {
        buf_appendf(b, "<option>");
        buf_html(b, (const char *)sqlite3_column_text(st, 0));
        buf_appendf(b, "</option>");
    }
    sqlite3_finalize(st);
}

static void render_memory_block(Buf *b, long long id, const char *label,
                                const char *owner, const char *desc,
                                int limit, int read_only, const char *placement,
                                int used) {
    buf_appendf(b, "<h3>#%lld ", id);
    buf_html(b, label);
    buf_appendf(b, " <small>owner: ");
    buf_html(b, owner && owner[0] ? owner : "operator");
    buf_appendf(b, " · %d/%d chars</small></h3>", used, limit);

    mem_form_open(b, "mem_block_update", id);
    buf_appendf(b, "<input name=description size=60 value=\"");
    buf_html(b, desc);
    buf_appendf(b, "\"> <select name=placement>"
                   "<option%s>system</option><option%s>context</option></select>"
                   " limit <input name=char_limit size=6 value=%d>"
                   " <label><input type=checkbox name=read_only value=1%s> read-only"
                   " for every agent</label> <button>save block</button></form> ",
                strcmp(placement, "system") == 0 ? " selected" : "",
                strcmp(placement, "context") == 0 ? " selected" : "",
                limit, read_only ? " checked" : "");
    mem_form_open(b, "mem_block_delete", id);
    buf_appendf(b, "<button>delete block</button></form>");

    /* Who has it. */
    buf_appendf(b, "<p>Attached to: ");
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(s_db,
            "SELECT agent_name, read_only FROM agent_memory_blocks"
            " WHERE block_id=?1 ORDER BY agent_name", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *ag = (const char *)sqlite3_column_text(st, 0);
            int ro = sqlite3_column_int(st, 1);
            n++;
            buf_html(b, ag);
            buf_appendf(b, " (%s) ", ro ? "read-only" : "read-write");
            mem_form_open(b, "mem_attach", id);
            buf_appendf(b, "<input type=hidden name=agent_name value=\"");
            buf_html(b, ag);
            buf_appendf(b, "\">%s<button>make %s</button></form> ",
                        ro ? "" : "<input type=hidden name=read_only value=1>",
                        ro ? "read-write" : "read-only");
            mem_form_open(b, "mem_detach", id);
            buf_appendf(b, "<input type=hidden name=agent_name value=\"");
            buf_html(b, ag);
            buf_appendf(b, "\"><button>detach</button></form> · ");
        }
        sqlite3_finalize(st);
    }
    if (n == 0) buf_appendf(b, "nobody · ");
    mem_form_open(b, "mem_attach", id);
    buf_appendf(b, "<select name=agent_name>");
    mem_agent_options(b, id);
    buf_appendf(b, "</select> <label><input type=checkbox name=read_only value=1 checked>"
                   " read-only</label> <button>attach</button></form></p>");

    /* Notes. */
    buf_appendf(b, "<table><tr><th>id</th><th>changed</th><th>note</th><th></th></tr>");
    if (sqlite3_prepare_v2(s_db, "SELECT id, text, date(updated_at,'unixepoch') FROM memory_notes"
                                 " WHERE block_id=?1 ORDER BY id", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            long long nid = sqlite3_column_int64(st, 0);
            buf_appendf(b, "<tr><td>%lld</td><td>%s</td><td>", nid,
                        (const char *)sqlite3_column_text(st, 2));
            mem_form_open(b, "mem_note_set", id);
            buf_appendf(b, "<input type=hidden name=note_id value=%lld>"
                           "<textarea name=text rows=3 cols=90>", nid);
            buf_html(b, (const char *)sqlite3_column_text(st, 1));
            buf_appendf(b, "</textarea> <button>save</button></form></td><td>");
            mem_form_open(b, "mem_note_delete", id);
            buf_appendf(b, "<input type=hidden name=note_id value=%lld>"
                           "<button>delete</button></form></td></tr>", nid);
        }
        sqlite3_finalize(st);
    }
    buf_appendf(b, "<tr><td>new</td><td></td><td>");
    mem_form_open(b, "mem_note_add", id);
    buf_appendf(b, "<textarea name=text rows=2 cols=90></textarea>"
                   " <button>add note</button></form></td><td></td></tr></table>");
}

/* Every block with its notes and attachments. The operator's edits here are
 * not bound by read_only or char_limit — those constrain agents. A change to
 * a 'system' block reaches an agent at its next session; 'context' blocks at
 * its next turn. */
static void render_memory(Buf *b) {
    buf_appendf(b, "<h2>Memory</h2><p>A block is a named container; a note is one"
                   " item in it. Changes to a <i>context</i> block show at the"
                   " agent's next turn, to a <i>system</i> block at its next"
                   " session.</p>");
    /* Collect block ids first: the per-block renderer runs its own queries. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db,
            "SELECT b.id, b.label, COALESCE(b.agent_name,''), COALESCE(b.description,''),"
            " b.char_limit, b.read_only, b.placement,"
            " (SELECT COALESCE(SUM(length(text)),0) FROM memory_notes n WHERE n.block_id=b.id)"
            " FROM memory_blocks b ORDER BY COALESCE(b.agent_name,''), b.id",
            -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            render_memory_block(b, sqlite3_column_int64(st, 0),
                                (const char *)sqlite3_column_text(st, 1),
                                (const char *)sqlite3_column_text(st, 2),
                                (const char *)sqlite3_column_text(st, 3),
                                sqlite3_column_int(st, 4), sqlite3_column_int(st, 5),
                                (const char *)sqlite3_column_text(st, 6),
                                sqlite3_column_int(st, 7));
        sqlite3_finalize(st);
    }
    buf_appendf(b, "<h3>New block</h3><form method=post action=/admin/act>"
                   "<input type=hidden name=action value=mem_block_create>"
                   "label <input name=label size=16> description"
                   " <input name=description size=50> <select name=placement>"
                   "<option>context</option><option>system</option></select>"
                   " owner <select name=agent_name><option value=\"\">(operator,"
                   " attach later)</option>");
    mem_agent_options(b, 0);
    buf_appendf(b, "</select> <button>create</button></form>");
}

/* POST actions for the section above. Returns 0 on success. */
static int memory_act(const char *action, const char *body, size_t blen) {
    char idbuf[32] = "", agent[128] = "", small[512] = "", placement[16] = "";
    mg_get_var(body, blen, "block_id", idbuf, sizeof(idbuf));
    mg_get_var(body, blen, "agent_name", agent, sizeof(agent));
    long long block = atoll(idbuf);
    int ro = mg_get_var(body, blen, "read_only", small, sizeof(small)) > 0;

    if (strcmp(action, "mem_block_create") == 0) {
        char label[80] = "", desc[512] = "";
        mg_get_var(body, blen, "label", label, sizeof(label));
        mg_get_var(body, blen, "description", desc, sizeof(desc));
        mg_get_var(body, blen, "placement", placement, sizeof(placement));
        return memory_block_create(s_db, agent[0] ? agent : NULL, label, desc,
                                   5000, placement) > 0 ? 0 : -1;
    }
    if (block <= 0) return -1;
    if (strcmp(action, "mem_block_update") == 0) {
        char desc[512] = "", lim[16] = "";
        mg_get_var(body, blen, "description", desc, sizeof(desc));
        mg_get_var(body, blen, "placement", placement, sizeof(placement));
        mg_get_var(body, blen, "char_limit", lim, sizeof(lim));
        if (memory_block_update(s_db, block, desc, placement[0] ? placement : NULL) != 0)
            return -1;
        return memory_block_set_limits(s_db, block, atoi(lim), ro);
    }
    if (strcmp(action, "mem_block_delete") == 0) return memory_block_delete(s_db, block);
    if (strcmp(action, "mem_attach") == 0)
        return agent[0] ? memory_block_attach(s_db, agent, block, ro) : -1;
    if (strcmp(action, "mem_detach") == 0)
        return agent[0] ? memory_block_detach(s_db, agent, block) : -1;

    char nid[32] = "";
    mg_get_var(body, blen, "note_id", nid, sizeof(nid));
    if (strcmp(action, "mem_note_delete") == 0)
        return memory_note_delete(s_db, block, atoll(nid));

    /* The two actions that carry note text: decode into a buffer as large as
     * the form itself, since a note has no fixed small bound. */
    char *text = malloc(blen + 1);
    if (!text) return -1;
    int rc = -1;
    if (mg_get_var(body, blen, "text", text, blen + 1) > 0) {
        if (strcmp(action, "mem_note_add") == 0)
            rc = memory_note_add(s_db, block, text) > 0 ? 0 : -1;
        else if (strcmp(action, "mem_note_set") == 0)
            rc = memory_note_set(s_db, block, atoll(nid), text);
    }
    free(text);
    return rc;
}

static int render_page(struct mg_connection *conn) {
    const struct mg_request_info *ri = mg_get_request_info(conn);
    char agent[128] = "";
    if (ri->query_string)
        mg_get_var(ri->query_string, strlen(ri->query_string),
                   "agent", agent, sizeof(agent));

    Buf b = {0};
    buf_appendf(&b,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width, initial-scale=1\">"
        "<title>cclaw admin</title><style>"
        "body{font-family:system-ui,sans-serif;margin:1.5em;max-width:70em}"
        "table{border-collapse:collapse;margin:.5em 0 1em}"
        "td,th{border:1px solid #bbb;padding:.25em .6em;text-align:left;"
        "vertical-align:top}"
        "h2{margin:1.2em 0 .2em;border-bottom:1px solid #bbb}"
        "form.i{display:inline;margin:0}"
        "td.args{max-width:28em;overflow-wrap:anywhere;font-family:monospace;"
        "font-size:.85em}"
        "</style></head><body><h1>cclaw admin</h1>");
    render_providers_models(&b);
    render_agents(&b);
    render_sessions(&b);
    render_channels(&b);
    render_memory(&b);
    render_grants(&b, agent);
    render_approvals(&b);
    buf_appendf(&b, "</body></html>");

    char *out = buf_take(&b);
    if (!out) return resp_plain(conn, 500, "Internal Server Error", "oom\n");
    mg_printf(conn,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: %zu\r\n\r\n", strlen(out));
    mg_write(conn, out, strlen(out));
    free(out);
    return 200;
}

/* ── POST /admin/act ─────────────────────────────────────────── */

static int handle_act(struct mg_connection *conn) {
    /* Cookie only: the token never rides in a form, and a cross-site page
     * can't read or guess the cookie's value to satisfy it. */
    if (dashboard_auth_check(conn, 0) != 1)
        return resp_plain(conn, 403, "Forbidden", "forbidden\n");

    /* One mg_read returns what has arrived, not the whole body — a long
     * note spans several TCP segments. Read until the body ends or the
     * buffer is full. */
    /* Heap, not stack: this runs on a web-server worker thread whose stack
     * is small, and the handler below adds its own buffers on top. */
    char *body = malloc(FORM_MAX);
    if (!body) return resp_plain(conn, 500, "Internal Server Error", "oom\n");
    int len = 0, got;
    while (len < FORM_MAX - 1 &&
           (got = mg_read(conn, body + len, (size_t)(FORM_MAX - 1 - len))) > 0)
        len += got;
    body[len] = '\0';
    size_t blen = (size_t)len;

    char action[32] = "", agent[128] = "";
    mg_get_var(body, blen, "action", action, sizeof(action));
    mg_get_var(body, blen, "agent", agent, sizeof(agent));

    int rc = -1;
    char v1[512] = "", v2[512] = "", v3[512] = "", v4[512] = "";
    if (strcmp(action, "set_key") == 0) {
        if (mg_get_var(body, blen, "provider", v1, sizeof(v1)) > 0 &&
            mg_get_var(body, blen, "key", v2, sizeof(v2)) > 0)
            rc = admin_set_key(s_db, v1, v2);
    } else if (strcmp(action, "set_model") == 0) {
        if (mg_get_var(body, blen, "provider", v1, sizeof(v1)) > 0 &&
            mg_get_var(body, blen, "model", v2, sizeof(v2)) > 0)
            rc = admin_set_model(s_db, v1, v2);
    } else if (strcmp(action, "set_endpoint") == 0) {
        if (mg_get_var(body, blen, "provider", v1, sizeof(v1)) > 0 &&
            mg_get_var(body, blen, "url", v2, sizeof(v2)) > 0)
            rc = admin_set_endpoint(s_db, v1, v2);
    } else if (strcmp(action, "switch_model") == 0) {
        if (mg_get_var(body, blen, "model_id", v1, sizeof(v1)) > 0)
            rc = admin_switch_model(s_db, v1, v2, sizeof(v2));
    } else if (strcmp(action, "toggle_model") == 0) {
        if (mg_get_var(body, blen, "model_id", v1, sizeof(v1)) > 0)
            rc = admin_toggle_model(s_db, v1);
    } else if (strcmp(action, "set_default_model") == 0) {
        /* Legacy form action: with per-agent routing lists this is the same
         * emergency lever as switch_model — head of every agent's list. */
        if (mg_get_var(body, blen, "model_id", v1, sizeof(v1)) > 0)
            rc = admin_switch_model(s_db, v1, v2, sizeof(v2));
    } else if (strcmp(action, "add_provider") == 0) {
        if (mg_get_var(body, blen, "name", v1, sizeof(v1)) > 0 &&
            mg_get_var(body, blen, "base_url", v2, sizeof(v2)) > 0) {
            mg_get_var(body, blen, "endpoint_type", v3, sizeof(v3));
            mg_get_var(body, blen, "api_key_env", v4, sizeof(v4));
            rc = admin_add_provider(s_db, v1, v2, v3[0] ? v3 : "openai", v4);
        }
    } else if (strcmp(action, "remove_provider") == 0) {
        if (mg_get_var(body, blen, "provider", v1, sizeof(v1)) > 0)
            rc = admin_remove_provider(s_db, v1);
    } else if (strcmp(action, "add_model") == 0) {
        if (mg_get_var(body, blen, "provider", v1, sizeof(v1)) > 0 &&
            mg_get_var(body, blen, "model", v2, sizeof(v2)) > 0) {
            mg_get_var(body, blen, "context_window", v3, sizeof(v3));
            rc = admin_add_model(s_db, v1, v2, v3[0] ? atoi(v3) : 0);
        }
    } else if (strcmp(action, "remove_model") == 0) {
        if (mg_get_var(body, blen, "model_id", v1, sizeof(v1)) > 0)
            rc = admin_remove_model(s_db, v1);
    } else if (strcmp(action, "set_agent_model") == 0) {
        if (agent[0]) {
            mg_get_var(body, blen, "primary_model", v1, sizeof(v1));
            mg_get_var(body, blen, "secondary_model", v2, sizeof(v2));
            rc = admin_set_agent_model(s_db, agent, v1[0] ? v1 : NULL, v2[0] ? v2 : NULL);
        }
    } else if (strcmp(action, "create_session") == 0) {
        if (agent[0]) {
            int64_t sid = admin_create_session(s_db, agent);
            rc = (sid > 0) ? 0 : -1;
        }
    } else if (strcmp(action, "attach_channel") == 0) {
        if (mg_get_var(body, blen, "session_id", v1, sizeof(v1)) > 0 &&
            mg_get_var(body, blen, "channel_name", v2, sizeof(v2)) > 0 && v2[0]) {
            mg_get_var(body, blen, "chat_id", v3, sizeof(v3));
            rc = v3[0] ? admin_attach_session_channel(s_db, atoll(v1), v2, v3)
                       : -1;   /* a pin needs a real chat id */
        }
    } else if (strcmp(action, "set_channel_route") == 0) {
        if (mg_get_var(body, blen, "channel_name", v1, sizeof(v1)) > 0) {
            mg_get_var(body, blen, "agent_name", v2, sizeof(v2));
            rc = admin_set_channel_route(s_db, v1, v2[0] ? v2 : NULL);
        }
    } else if (strcmp(action, "grant") == 0) {
        if (agent[0] &&
            mg_get_var(body, blen, "kind", v1, sizeof(v1)) > 0 &&
            mg_get_var(body, blen, "value", v2, sizeof(v2)) > 0)
            rc = admin_grant_capability(s_db, agent, v1, v2);
    } else if (strcmp(action, "revoke") == 0) {
        if (mg_get_var(body, blen, "grant_id", v1, sizeof(v1)) > 0)
            rc = admin_revoke_grant_by_id(s_db, atoll(v1));
    } else if (strcmp(action, "approve") == 0) {
        if (mg_get_var(body, blen, "approval_id", v1, sizeof(v1)) > 0 &&
            mg_get_var(body, blen, "decision", v2, sizeof(v2)) > 0) {
            ApprovalDecision d = strcmp(v2, "always") == 0 ? APPROVAL_ALWAYS
                               : strcmp(v2, "once") == 0   ? APPROVAL_ONCE
                                                           : APPROVAL_DENY;
            /* Recorded here, carried out by the event loop: resolving runs
             * tools and advances a session, which this thread must not do. */
            rc = approval_decision_queue(s_db, atoll(v1), d, "dashboard");
            if (rc == 0) wake_session(0);
        }
    } else if (strncmp(action, "mem_", 4) == 0) {
        rc = memory_act(action, body, blen);
    } else if (strcmp(action, "grant_history") == 0) {
        if (mg_get_var(body, blen, "approval_id", v1, sizeof(v1)) > 0)
            rc = admin_grant_from_history(s_db, atoll(v1));
    }

    free(body);
    if (rc != 0)
        return resp_plain(conn, 400, "Bad Request", "action failed\n");

    char loc[192] = "/admin";
    if (agent[0]) {
        Buf q = {0};
        buf_appendf(&q, "/admin?agent=");
        /* form value round-trips URL-encoded; conservative re-encode */
        for (const char *p = agent; *p; p++) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')
                buf_appendf(&q, "%c", *p);
            else
                buf_appendf(&q, "%%%02X", (unsigned char)*p);
        }
        char *qs = buf_take(&q);
        if (qs) { snprintf(loc, sizeof(loc), "%s", qs); free(qs); }
    }
    return resp_303(conn, loc, NULL);
}

/* ── entry ───────────────────────────────────────────────────── */

static int handle_admin_locked(struct mg_connection *conn);

static int handle_admin(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    web_db_lock();
    int rc = handle_admin_locked(conn);
    web_db_unlock();
    return rc;
}

static int handle_admin_locked(struct mg_connection *conn) {
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri ? ri->local_uri : "";
    const char *method = ri->request_method ? ri->request_method : "";

    if (strcmp(uri, "/admin/act") == 0 && strcmp(method, "POST") == 0)
        return handle_act(conn);
    if (strcmp(uri, "/admin") != 0 && strcmp(uri, "/admin/") != 0)
        return resp_plain(conn, 404, "Not Found", "not found\n");

    int auth = dashboard_auth_check(conn, 1);
    if (auth == 0)
        return resp_plain(conn, 403, "Forbidden", "forbidden\n");
    if (auth == 2) {
        /* ?token= visit: move the token into a cookie and drop it from the
         * URL so it doesn't linger in the address bar / referrers. */
        char *tok = config_get(s_db, "web_admin_token");
        int rc = resp_303(conn, "/admin", tok);
        free(tok);
        return rc;
    }
    return render_page(conn);
}

char *dashboard_url(sqlite3 *db) {
    char *tok = config_get(db, "web_admin_token");
    char *base = (tok && tok[0]) ? config_web_base_url(db) : NULL;
    size_t n = base ? strlen(base) + strlen(tok) + 32 : 0;
    char *url = base ? malloc(n) : NULL;
    if (url) snprintf(url, n, "%s/admin?token=%s", base, tok);
    free(base);
    free(tok);
    return url;
}

int dashboard_register(struct mg_context *ctx, sqlite3 *db) {
    if (!ctx || !db) return -1;
    s_db = db;
    if (ensure_token(db) != 0) return -1;
    mg_set_request_handler(ctx, "/admin", handle_admin, NULL);
    return 0;
}
