/* The secret key and DB ciphertext must not be readable from a sandboxed
 * child, even when a grant mounts the directory that holds them.
 *
 * This guards a regression that was live and silent: the broker used to set
 * cfg->db_path = NULL ("no DB in this process"), which made the mask a no-op
 * in every profile. Nothing caught it, because with no grant the key is
 * already invisible by omission — the sandbox simply never mounts ~/.cclaw.
 * So the test that matters is the *granted* case: mount the directory, then
 * prove the key still reads empty.
 *
 * Assertions are on content, not mode bits. The mask binds an empty file over
 * the real one, so the content check holds whether or not the caller is root
 * (a root invoker gets an identity uid_map and CAP_DAC_OVERRIDE inside the ns,
 * which defeats a mode-0000 check but cannot resurrect replaced content). */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/run_tool.h"
#include "test_run_tool_shell.h"

#define ROOT     "/tmp/cclaw_keymask"
#define STATEDIR ROOT "/state"
#define DB       STATEDIR "/cclaw.db"
#define KEYF     STATEDIR "/.cclaw_key"
#define WS       ROOT "/ws"

/* Distinct markers so a leak names which file leaked. */
#define KEY_MARK "KEYBYTES_SHOULD_NEVER_APPEAR"
#define DB_MARK  "DBBYTES_SHOULD_NEVER_APPEAR"

static void write_file(const char *path, const char *contents, mode_t mode) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(contents, f);
    fclose(f);
    assert(chmod(path, mode) == 0);
}

static void setup(void) {
    system("rm -rf " ROOT);
    assert(mkdir(ROOT, 0755) == 0);
    assert(mkdir(STATEDIR, 0755) == 0);
    assert(mkdir(WS, 0755) == 0);
    write_file(DB, DB_MARK "\n", 0600);
    write_file(KEYF, KEY_MARK "\n", 0600);
}

/* cat both files from inside the sandbox with the given grant. */
static char *read_state_files(const char *grant_path) {
    const char *reads[] = { grant_path };
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "cat " KEYF " 2>&1; cat " DB " 2>&1";
    r.workspace = WS;
    r.db_path = DB;
    r.net_mode = 1;
    r.read_paths = reads;
    r.read_count = 1;
    return run_tool_shell(&r);
}

/* A directory grant covering the state dir must not expose either file. */
static void test_directory_grant_masks_both(void) {
    setup();
    char *out = read_state_files(STATEDIR);
    printf("  directory grant → %s\n", out);
    assert(strstr(out, KEY_MARK) == NULL);
    assert(strstr(out, DB_MARK) == NULL);
    free(out);
    printf("PASS test_directory_grant_masks_both\n");
}

/* Naming the DB file in a grant exposes nothing either. Two independent
 * reasons, and the test is deliberately agnostic about which one fired:
 * grants bind directories only, so a file path never mounts; and the mask is
 * unconditional regardless. If file-granularity grants ever land, this test
 * is the one that should be revisited alongside the DB rule in sandbox.c. */
static void test_db_file_grant_exposes_nothing(void) {
    setup();
    char *out = read_state_files(DB);
    printf("  db file grant → %s\n", out);
    assert(strstr(out, KEY_MARK) == NULL);
    assert(strstr(out, DB_MARK) == NULL);
    free(out);
    printf("PASS test_db_file_grant_exposes_nothing\n");
}

/* No grant at all: invisible by omission, nothing mounted to mask. */
static void test_no_grant_exposes_nothing(void) {
    setup();
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "cat " KEYF " 2>&1; cat " DB " 2>&1";
    r.workspace = WS;
    r.db_path = DB;
    r.net_mode = 1;
    char *out = run_tool_shell(&r);
    printf("  no grant → %s\n", out);
    assert(strstr(out, KEY_MARK) == NULL);
    assert(strstr(out, DB_MARK) == NULL);
    free(out);
    printf("PASS test_no_grant_exposes_nothing\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_no_grant_exposes_nothing();
    test_directory_grant_masks_both();
    test_db_file_grant_exposes_nothing();
    system("rm -rf " ROOT);
    printf("All key-mask tests passed\n");
    return 0;
}
