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
    const char *workspace = (const char *)user_data;
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

    /* V1: verify path is within workspace */
    char resolved[PATH_MAX];
    if (!path_in_workspace(fullpath, workspace, resolved, sizeof(resolved))) {
        return strdup("error: path outside workspace");
    }

    FILE *f = fopen(resolved, "rb");
    if (!f) return strdup("error: cannot open file");

    char *buf = malloc(FILE_READ_MAX + 1);
    if (!buf) { fclose(f); return strdup("error: out of memory"); }

    size_t n = fread(buf, 1, FILE_READ_MAX, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int tool_file_read_register(ToolRegistry *reg, const char *workspace) {
    return tools_register(reg, "file_read",
                          "Read a file within the workspace directory",
                          FILE_READ_PARAMS_JSON, tool_file_read_handler,
                          (void *)workspace);
}
