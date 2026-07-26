/* /tmp in a sandboxed child is a bind of the agent's own scratch directory on
 * the host — not a tmpfs we size ourselves.
 *
 * Two properties have to hold together, and they pull in opposite directions:
 * it must PERSIST across tool calls (./configure in one call, make in the next,
 * and the tool-call boundary is invisible to the agent), while staying PRIVATE
 * (binding the host's shared /tmp would hand the child every other process's
 * scratch — ssh-agent and gpg-agent sockets included). A test that checks only
 * persistence would pass on the shared-/tmp implementation, so both are here.
 *
 * The rest covers scratch_dir_ensure's ownership checks. /tmp is mode 1777, so
 * an existing path proves nothing about who created it. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/config.h"
#include "test_run_tool_shell.h"

#define ROOT "/tmp/cclaw_scratch_test"
#define WS   ROOT "/ws"
/* A file in the REAL /tmp, which must never be visible inside the sandbox. */
#define HOST_TMP_MARKER "/tmp/cclaw_scratch_host_only.txt"

static void setup(void) {
    system("rm -rf " ROOT);
    assert(mkdir(ROOT, 0755) == 0);
    assert(mkdir(WS, 0755) == 0);
    FILE *f = fopen(HOST_TMP_MARKER, "w");
    assert(f);
    fputs("HOST_TMP_LEAKED\n", f);
    fclose(f);
}

static void cleanup(void) {
    system("rm -rf " ROOT);
    unlink(HOST_TMP_MARKER);
}

/* ── scratch_dir_ensure ─────────────────────────────────────────────── */

static void test_creates_owned_0700(void) {
    char out[PATH_MAX];
    const char *p = scratch_dir_ensure("Agent", ROOT, out, sizeof(out));
    assert(p != NULL);

    char want[PATH_MAX];
    snprintf(want, sizeof(want), "%s/cclaw-%u/Agent", ROOT, (unsigned)getuid());
    assert(strcmp(p, want) == 0);

    struct stat st;
    assert(stat(p, &st) == 0 && S_ISDIR(st.st_mode));
    assert((st.st_mode & 07777) == 0700);
    assert(st.st_uid == getuid());

    /* Idempotent — a second call on the existing tree must succeed, or every
     * tool call after the first fails. */
    assert(scratch_dir_ensure("Agent", ROOT, out, sizeof(out)) != NULL);
    printf("  PASS creates_owned_0700\n");
}

static void test_per_agent_isolation(void) {
    char a[PATH_MAX], b[PATH_MAX];
    assert(scratch_dir_ensure("Alpha", ROOT, a, sizeof(a)) != NULL);
    assert(scratch_dir_ensure("Beta", ROOT, b, sizeof(b)) != NULL);
    assert(strcmp(a, b) != 0);
    printf("  PASS per_agent_isolation\n");
}

/* A symlink planted at the per-uid base must be refused, not followed — /tmp is
 * world-writable, so this is the case the ownership check exists for. */
static void test_refuses_symlinked_base(void) {
    char sub[PATH_MAX / 2];
    snprintf(sub, sizeof(sub), "%s/sym", ROOT);
    assert(mkdir(sub, 0755) == 0);

    char base[PATH_MAX], target[PATH_MAX];
    snprintf(target, sizeof(target), "%s/elsewhere", ROOT);
    assert(mkdir(target, 0700) == 0);
    snprintf(base, sizeof(base), "%s/cclaw-%u", sub, (unsigned)getuid());
    assert(symlink(target, base) == 0);

    char out[PATH_MAX];
    assert(scratch_dir_ensure("Agent", sub, out, sizeof(out)) == NULL);
    printf("  PASS refuses_symlinked_base\n");
}

/* A world-writable base is refused too: another user could then drop files into
 * the agent's /tmp. */
static void test_refuses_wrong_mode(void) {
    char sub[PATH_MAX / 2];
    snprintf(sub, sizeof(sub), "%s/mode", ROOT);
    assert(mkdir(sub, 0755) == 0);

    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/cclaw-%u", sub, (unsigned)getuid());
    assert(mkdir(base, 0777) == 0);
    assert(chmod(base, 0777) == 0);

    char out[PATH_MAX];
    assert(scratch_dir_ensure("Agent", sub, out, sizeof(out)) == NULL);
    printf("  PASS refuses_wrong_mode\n");
}

/* Abandoned namespace roots are reaped opportunistically. The old code leaked
 * one empty dir per tool call forever (~3800 on the dev box) because the child
 * could not reach it after pivot_root and the parent never knew its name. */
static void test_reaps_abandoned_ns_roots(void) {
    char out[PATH_MAX];
    assert(scratch_dir_ensure("Agent", ROOT, out, sizeof(out)) != NULL);

    char stale[PATH_MAX];
    snprintf(stale, sizeof(stale), "%s/cclaw-%u/.ns/999999",
             ROOT, (unsigned)getuid());
    assert(mkdir(stale, 0700) == 0);
    struct stat st;
    assert(stat(stale, &st) == 0);          /* precondition: it exists */

    assert(scratch_dir_ensure("Agent", ROOT, out, sizeof(out)) != NULL);
    assert(stat(stale, &st) != 0);          /* and the next call reaped it */
    printf("  PASS reaps_abandoned_ns_roots\n");
}

/* ── end to end, through the real --run-tool broker ──────────────────── */

static void test_tmp_persists_and_is_private(void) {
    if (!run_tool_ns_available(WS)) {
        printf("  SKIP tmp_persists_and_is_private (namespaces unavailable)\n");
        return;
    }
    char scratch[PATH_MAX];
    const char *tmp_dir = scratch_dir_ensure("Agent", ROOT, scratch, sizeof(scratch));
    assert(tmp_dir != NULL);

    /* Call 1: write a marker into /tmp and confirm the host's /tmp is not here. */
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = WS;
    r.tmp_dir = tmp_dir;
    r.net_mode = 1;
    r.command = "echo SURVIVED > /tmp/marker.txt; "
                "test -e " HOST_TMP_MARKER " && echo HOST_TMP_VISIBLE || echo host_tmp_hidden";
    char *out = run_tool_shell(&r);
    assert(out != NULL);
    printf("  call 1 → %s\n", out);
    /* Private: nothing of the host's shared /tmp is reachable. */
    assert(strstr(out, "HOST_TMP_VISIBLE") == NULL);
    assert(strstr(out, "host_tmp_hidden") != NULL);
    free(out);

    /* Call 2: a *separate* child, so a fresh-per-call /tmp would lose this. */
    r.command = "cat /tmp/marker.txt 2>&1";
    out = run_tool_shell(&r);
    assert(out != NULL);
    printf("  call 2 → %s\n", out);
    assert(strstr(out, "SURVIVED") != NULL);
    free(out);

    /* And it really is the host directory we bound, visible from outside. */
    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/marker.txt", tmp_dir);
    struct stat st;
    assert(stat(marker, &st) == 0);
    printf("  PASS tmp_persists_and_is_private\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_sandbox_scratch\n");
    setup();

    test_creates_owned_0700();
    test_per_agent_isolation();
    test_refuses_symlinked_base();
    test_refuses_wrong_mode();
    test_reaps_abandoned_ns_roots();
    test_tmp_persists_and_is_private();

    cleanup();
    printf("All scratch-dir tests passed\n");
    return 0;
}
