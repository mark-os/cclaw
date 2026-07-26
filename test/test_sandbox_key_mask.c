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

/* Naming the DB file exactly is the sanctioned way to reach it — grants bind
 * files, not just directories, so this mounts. It must not drag the key along:
 * this is the half of the rule a mask-everything implementation would fail. */
static void test_exact_db_grant_exposes_db_but_never_key(void) {
    setup();
    char *out = read_state_files(DB);
    printf("  exact db grant → %s\n", out);
    assert(strstr(out, KEY_MARK) == NULL);
    assert(strstr(out, DB_MARK) != NULL);
    free(out);
    printf("PASS test_exact_db_grant_exposes_db_but_never_key\n");
}

/* A grant naming the key file itself must still not expose it — the key is
 * masked unconditionally, and no grant may unmask it. */
static void test_exact_key_grant_still_masked(void) {
    setup();
    char *out = read_state_files(KEYF);
    printf("  exact key grant → %s\n", out);
    assert(strstr(out, KEY_MARK) == NULL);
    free(out);
    printf("PASS test_exact_key_grant_still_masked\n");
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
    test_exact_db_grant_exposes_db_but_never_key();
    test_exact_key_grant_still_masked();
    system("rm -rf " ROOT);
    printf("All key-mask tests passed\n");
    return 0;
}
