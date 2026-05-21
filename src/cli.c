#define _POSIX_C_SOURCE 200809L
#include "cli.h"
#include "agent.h"
#include "db.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tool dispatch via registry */
static char *cli_dispatch(const char *name, const char *arguments, void *user_data) {
    ToolRegistry *reg = (ToolRegistry *)user_data;
    ToolEntry *e = tools_lookup(reg, name);
    if (!e) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
        return err;
    }
    return e->handler(arguments, e->user_data);
}

/* Print assistant response from the last entry in the session branch */
static void print_response(sqlite3 *db, int64_t session_id) {
    int count = 0;
    Entry *entries = session_get_branch(db, session_id, &count);
    if (!entries || count == 0) return;

    /* Walk backwards to find last assistant message with content */
    for (int i = count - 1; i >= 0; i--) {
        if (entries[i].message.role == ROLE_ASSISTANT && entries[i].message.content) {
            printf("%s\n", entries[i].message.content);
            break;
        }
    }
    entry_branch_free(entries, count);
}

int cli_run(const Config *cfg) {
    if (!cfg) return -1;
    if (!cfg->provider.api_key) {
        fprintf(stderr, "error: OPENROUTER_API_KEY not set\n");
        return -1;
    }

    sqlite3 *db = db_open(cfg->db_path);
    if (!db) {
        fprintf(stderr, "error: cannot open database '%s'\n", cfg->db_path);
        return -1;
    }

    int64_t session_id = session_create(db, "cli");
    if (session_id < 0) {
        fprintf(stderr, "error: cannot create session\n");
        db_close(db);
        return -1;
    }

    /* Register tools */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg);
    tool_file_read_register(&reg, cfg->workspace);
    tool_file_write_register(&reg, cfg->workspace);

    size_t tool_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &tool_count);

    /* Append system message */
    Message sys_msg = {.role = ROLE_SYSTEM, .content = "You are CClaw, a helpful AI assistant."};
    entry_append(db, session_id, &sys_msg);

    printf("cclaw cli (type 'exit' or Ctrl-D to quit)\n");

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while (1) {
        printf("> ");
        fflush(stdout);

        line_len = getline(&line, &line_cap, stdin);
        if (line_len < 0) break;  /* EOF */

        /* Strip trailing newline */
        if (line_len > 0 && line[line_len - 1] == '\n')
            line[line_len - 1] = '\0';

        if (line[0] == '\0') continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;

        /* Append user message */
        Message user_msg = {.role = ROLE_USER, .content = line};
        entry_append(db, session_id, &user_msg);

        /* Run agent */
        AgentContext ctx = {0};
        ctx.db = db;
        ctx.session_id = session_id;
        ctx.cfg = cfg;
        ctx.dispatch = cli_dispatch;
        ctx.dispatch_data = &reg;
        ctx.tools = schemas;
        ctx.tool_count = tool_count;
        ctx.debug = cfg->debug;

        int rc = agent_run(&ctx);
        if (rc != 0) {
            fprintf(stderr, "error: agent failed\n");
        } else {
            print_response(db, session_id);
        }
    }

    free(line);
    tools_free(&reg);
    db_close(db);
    return 0;
}
