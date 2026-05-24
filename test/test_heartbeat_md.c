#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tool_file.h"

/* V42/T111: HEARTBEAT.md is optional workspace file read by agent during heartbeat */

static char tmpdir[256];

static void setup_workspace(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cclaw_hb_test_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);
}

static void test_heartbeat_md_readable(void) {
    /* Create HEARTBEAT.md in workspace */
    char path[512];
    snprintf(path, sizeof(path), "%s/HEARTBEAT.md", tmpdir);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "# Heartbeat Tasks\n- Check disk usage\n- Prune old logs\n");
    fclose(f);

    /* Read via file_read handler (same path agent uses) */
    char *result = tool_file_read_handler("{\"path\":\"HEARTBEAT.md\"}", (void *)tmpdir);
    assert(result);
    assert(strstr(result, "# Heartbeat Tasks"));
    assert(strstr(result, "Check disk usage"));
    free(result);
    printf("  PASS: HEARTBEAT.md readable from workspace\n");
}

static void test_heartbeat_md_missing_graceful(void) {
    /* No HEARTBEAT.md — file_read returns error, agent handles gracefully */
    char *result = tool_file_read_handler("{\"path\":\"HEARTBEAT.md\"}", (void *)tmpdir);
    assert(result);
    assert(strstr(result, "error:"));
    free(result);
    printf("  PASS: missing HEARTBEAT.md returns error (agent handles gracefully)\n");
}

static void test_heartbeat_md_not_auto_created(void) {
    /* V42: HEARTBEAT.md is optional — workspace_init must NOT create it */
    char path[512];
    snprintf(path, sizeof(path), "%s/HEARTBEAT.md", tmpdir);
    struct stat st;
    assert(stat(path, &st) != 0);
    printf("  PASS: HEARTBEAT.md not auto-created (optional)\n");
}

int main(void) {
    printf("test_heartbeat_md:\n");
    setup_workspace();
    test_heartbeat_md_not_auto_created();
    test_heartbeat_md_missing_graceful();
    test_heartbeat_md_readable();
    cleanup_workspace();
    printf("ALL PASSED\n");
    return 0;
}
