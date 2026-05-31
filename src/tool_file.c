#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tool_file.h"
#include <cJSON.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_READ_MAX (256 * 1024)

static const char *FILE_READ_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to read (relative to workspace)\"}"
    "},\"required\":[\"path\"]}";

/* V1: check resolved path starts with resolved workspace */
static int path_in_workspace(const char *filepath, const char *workspace, char *resolved, size_t resolved_size) {
    char ws_resolved[PATH_MAX];
    if (!realpath(workspace, ws_resolved)) return 0;
    if (!realpath(filepath, resolved)) return 0;

    size_t ws_len = strlen(ws_resolved);
    if (ws_len >= resolved_size) return 0;

    /* Path must start with workspace and next char must be '/' or '\0' */
    if (strncmp(resolved, ws_resolved, ws_len) != 0) return 0;
    if (resolved[ws_len] != '/' && resolved[ws_len] != '\0') return 0;
    return 1;
}

char *tool_file_read_handler(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    const char *workspace = ctx ? ctx->workspace : NULL;
    if (!workspace) return strdup("error: no workspace configured");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(json, "path");
    if (!cJSON_IsString(path_item) || !path_item->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: missing or empty 'path' field");
    }

    /* Build full path: workspace + "/" + path (if relative) */
    const char *req_path = path_item->valuestring;
    char fullpath[PATH_MAX];
    if (req_path[0] == '/') {
        snprintf(fullpath, sizeof(fullpath), "%s", req_path);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", workspace, req_path);
    }
    cJSON_Delete(json);

    /* V1: verify path is within workspace, extra_read_path, or cclaw_path (T228) */
    char resolved[PATH_MAX];
    if (!path_in_workspace(fullpath, workspace, resolved, sizeof(resolved))) {
        /* T118: check extra read path */
        if (ctx->extra_read_path &&
            path_in_workspace(fullpath, ctx->extra_read_path, resolved, sizeof(resolved)))
            goto read_file;
        /* T228: check CCLAW_PATH (CWD, read-only) */
        if (ctx->cclaw_path &&
            path_in_workspace(fullpath, ctx->cclaw_path, resolved, sizeof(resolved)))
            goto read_file;
        return strdup("error: path outside workspace");
    }

read_file:;

    FILE *f = fopen(resolved, "rb");
    if (!f) return strdup("error: cannot open file");

    char *buf = malloc(FILE_READ_MAX + 1);
    if (!buf) { fclose(f); return strdup("error: out of memory"); }

    size_t n = fread(buf, 1, FILE_READ_MAX, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int tool_file_read_register(ToolRegistry *reg, FileReadCtx *ctx) {
    return tools_register(reg, "file_read",
                          "Read a file within the workspace directory",
                          FILE_READ_PARAMS_JSON, tool_file_read_handler,
                          (void *)ctx);
}

static const char *FILE_WRITE_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to write (relative to workspace)\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
    "},\"required\":[\"path\",\"content\"]}";

/* V1: validate parent dir exists within workspace (for new files realpath won't work) */
static int parent_in_workspace(const char *filepath, const char *workspace, char *fullpath_out, size_t out_size) {
    char ws_resolved[PATH_MAX];
    if (!realpath(workspace, ws_resolved)) return 0;
    size_t ws_len = strlen(ws_resolved);

    /* Find last slash to get parent dir */
    const char *last_slash = strrchr(filepath, '/');
    if (!last_slash) {
        /* File in workspace root — parent is workspace itself */
        snprintf(fullpath_out, out_size, "%s/%s", ws_resolved, filepath);
        return 1;
    }

    /* Resolve parent directory */
    char parent[PATH_MAX];
    size_t plen = (size_t)(last_slash - filepath);
    if (plen >= sizeof(parent)) return 0;
    memcpy(parent, filepath, plen);
    parent[plen] = '\0';

    char parent_resolved[PATH_MAX];
    if (!realpath(parent, parent_resolved)) return 0;

    if (strncmp(parent_resolved, ws_resolved, ws_len) != 0) return 0;
    if (parent_resolved[ws_len] != '/' && parent_resolved[ws_len] != '\0') return 0;

    snprintf(fullpath_out, out_size, "%s/%s", parent_resolved, last_slash + 1);
    return 1;
}

char *tool_file_write_handler(const char *arguments, void *user_data) {
    const char *workspace = (const char *)user_data;
    if (!workspace) return strdup("error: no workspace configured");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(json, "path");
    if (!cJSON_IsString(path_item) || !path_item->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: missing or empty 'path' field");
    }

    cJSON *content_item = cJSON_GetObjectItemCaseSensitive(json, "content");
    if (!cJSON_IsString(content_item)) {
        cJSON_Delete(json);
        return strdup("error: missing 'content' field");
    }

    const char *req_path = path_item->valuestring;
    const char *content = content_item->valuestring;

    /* Build full path */
    char fullpath[PATH_MAX];
    if (req_path[0] == '/') {
        snprintf(fullpath, sizeof(fullpath), "%s", req_path);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", workspace, req_path);
    }

    /* V1: try realpath first (file exists → overwrite case) */
    char resolved[PATH_MAX];
    char ws_resolved[PATH_MAX];
    if (!realpath(workspace, ws_resolved)) {
        cJSON_Delete(json);
        return strdup("error: invalid workspace");
    }
    size_t ws_len = strlen(ws_resolved);

    char *write_path = NULL;
    if (realpath(fullpath, resolved)) {
        /* File exists — check it's in workspace */
        if (strncmp(resolved, ws_resolved, ws_len) != 0 ||
            (resolved[ws_len] != '/' && resolved[ws_len] != '\0')) {
            cJSON_Delete(json);
            return strdup("error: path outside workspace");
        }
        write_path = resolved;
    } else {
        /* New file — validate parent is in workspace */
        char validated[PATH_MAX];
        if (!parent_in_workspace(fullpath, workspace, validated, sizeof(validated))) {
            cJSON_Delete(json);
            return strdup("error: path outside workspace");
        }
        write_path = validated;
        /* Copy to resolved so it persists after this block */
        snprintf(resolved, sizeof(resolved), "%s", validated);
        write_path = resolved;
    }

    FILE *f = fopen(write_path, "wb");
    if (!f) {
        cJSON_Delete(json);
        return strdup("error: cannot open file for writing");
    }

    size_t content_len = strlen(content);
    size_t written = fwrite(content, 1, content_len, f);
    fclose(f);
    cJSON_Delete(json);

    if (written != content_len) {
        return strdup("error: incomplete write");
    }

    char *result = malloc(64);
    if (!result) return strdup("ok");
    snprintf(result, 64, "wrote %zu bytes", written);
    return result;
}

int tool_file_write_register(ToolRegistry *reg, const char *workspace) {
    return tools_register(reg, "file_write",
                          "Write content to a file within the workspace directory",
                          FILE_WRITE_PARAMS_JSON, tool_file_write_handler,
                          (void *)workspace);
}
