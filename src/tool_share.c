#define _GNU_SOURCE
#include "tool_share.h"
#include "tool_args.h"
#include "config_registry.h"
#include "util.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHARE_DEFAULT_HOURS 24

static char *tool_share_handler(const char *arguments, void *user_data, int *is_error) {
    ToolChannelSendCtx *ctx = (ToolChannelSendCtx *)user_data;
    if (!ctx || !ctx->db) return tool_fail(is_error, "error: share_file unavailable");

    char *file = tool_args_str(ctx->db, arguments, "file");
    if (!file || !file[0]) {
        free(file);
        return tool_fail(is_error, "error: file is required (a path in your workspace)");
    }
    int max_hours = config_get_int(ctx->db, "share_max_hours");
    int hours = tool_args_int(ctx->db, arguments, "hours", SHARE_DEFAULT_HOURS);
    if (hours <= 0) hours = SHARE_DEFAULT_HOURS;
    if (max_hours > 0 && hours > max_hours) hours = max_hours;

    /* Same rule as channel_send's file: judged on the file actually opened.
     * Only its resolved path is kept — the web server re-opens it per
     * request and serves it only while it still resolves to this path. */
    char real[PATH_MAX];
    int fd = util_open_inside(ctx->workspace, file, real, sizeof(real));
    struct stat st;
    char *err = NULL;
    if (fd < 0)
        err = errno == EXDEV
            ? tool_fail(is_error, "error: '%s' is outside your workspace — only "
                        "files in your workspace can be shared", file)
            : tool_fail(is_error, "error: cannot open file '%s': %s", file,
                        strerror(errno));
    else if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        err = tool_fail(is_error, "error: '%s' is not a regular file", file);
    if (fd >= 0) close(fd);
    free(file);
    if (err) return err;

    unsigned char rnd[16];
    char token[33];
    if (util_random(rnd, sizeof(rnd)) != 0)
        return tool_fail(is_error, "error: no randomness for a share token");
    for (int i = 0; i < 16; i++) snprintf(token + i * 2, 3, "%02x", rnd[i]);

    /* The name in the URL is decoration for the reader (the token alone
     * selects the file), so it is reduced to characters needing no escape. */
    char name[80];
    const char *base = strrchr(real, '/');
    base = base ? base + 1 : real;
    size_t k = 0;
    for (; base[k] && k < sizeof(name) - 1; k++) {
        unsigned char ch = (unsigned char)base[k];
        name[k] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')
                  ? (char)ch : '_';
    }
    name[k] = '\0';

    /* Expired rows go when a new one arrives: no sweeper, and the table
     * stays as small as the shares that can still be opened. */
    sqlite3_exec(ctx->db, "DELETE FROM shares WHERE expires_at <= unixepoch();",
                 NULL, NULL, NULL);
    sqlite3_stmt *st2;
    int ok = 0;
    if (sqlite3_prepare_v2(ctx->db,
            "INSERT INTO shares(token, agent_name, path, name, expires_at)"
            " VALUES(?1, ?2, ?3, ?4, unixepoch() + ?5 * 3600);",
            -1, &st2, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st2, 1, token, -1, SQLITE_STATIC);
        sqlite3_bind_text(st2, 2, ctx->agent_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st2, 3, real, -1, SQLITE_STATIC);
        sqlite3_bind_text(st2, 4, name, -1, SQLITE_STATIC);
        sqlite3_bind_int(st2, 5, hours);
        ok = sqlite3_step(st2) == SQLITE_DONE;
        sqlite3_finalize(st2);
    }
    if (!ok) return tool_fail(is_error, "error: share failed (db)");

    char *url_base = config_web_base_url(ctx->db);
    char *out = tool_errf("%s/share/%s/%s\nvalid for %d hour%s (%lld bytes). "
                          "Anyone with this link can read the file until then.",
                          url_base ? url_base : "", token, name, hours,
                          hours == 1 ? "" : "s", (long long)st.st_size);
    free(url_base);
    return out;
}

static const char *SHARE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"file\":{\"type\":\"string\",\"description\":\"A file in your workspace\"},"
    "\"hours\":{\"type\":\"integer\",\"description\":\"How long the link works (default 24)\"}"
    "},\"required\":[\"file\"]}";

int tool_share_register(ToolRegistry *reg, ToolChannelSendCtx *ctx) {
    int rc = tools_register(reg, "share_file",
        "Get a temporary link to a file in your workspace, served by this "
        "daemon's web server. Use it when a chat attachment will not do — the "
        "file is too large, or the chat takes no files; to show someone a "
        "picture in their chat, channel_send with file is the direct way. "
        "Anyone holding the link can read the file until it expires.",
        SHARE_PARAMS, tool_share_handler, ctx);
    tools_set_recipe(reg, "share_file", (ToolRecipe){.vehicle = EXEC_INLINE});
    return rc;
}
