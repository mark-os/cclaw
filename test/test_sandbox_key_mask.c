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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/run_tool.h"
#include "../src/tool_args.h"
#include "test_run_tool_shell.h"

#define ROOT     "/tmp/cclaw_keymask"
#define STATEDIR ROOT "/state"
#define DB       STATEDIR "/cclaw.db"
#define KEYF     STATEDIR "/.cclaw_key"
#define DECOY    STATEDIR "/notes.txt"
#define WS       ROOT "/ws"

/* Distinct markers so a leak names which file leaked. */
#define KEY_MARK "KEYBYTES_SHOULD_NEVER_APPEAR"
#define DB_MARK  "DBBYTES_SHOULD_NEVER_APPEAR"
/* Positive control: an ordinary file living beside them. It must come back —
 * otherwise an empty key/db read proves nothing (a masked file, an unmounted
 * grant and a broken request all look identical). */
#define DECOY_MARK "DECOYBYTES_MUST_APPEAR"

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
    write_file(DECOY, DECOY_MARK "\n", 0644);
}

/* cat the key, the db and the decoy from inside the sandbox, under the given
 * grant (plus an always-present directory grant so the decoy is reachable and
 * the positive control means something). */
static char *read_state_files(const char *grant_path) {
    const char *reads[] = { grant_path, STATEDIR };
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "cat " KEYF " 2>&1; cat " DB " 2>&1; cat " DECOY " 2>&1";
    r.workspace = WS;
    r.db_path = DB;
    r.net_mode = 1;
    r.read_paths = reads;
    r.read_count = 2;
    return run_tool_shell(&r);
}

/* A directory grant covering the state dir must not expose the key or the DB —
 * while the decoy beside them reads fine, which is what makes the two empty
 * reads mean "masked" rather than "nothing mounted". */
static void test_directory_grant_masks_both(void) {
    setup();
    char *out = read_state_files(STATEDIR);
    printf("  directory grant → %s\n", out);
    assert(strstr(out, DECOY_MARK) != NULL);   /* positive control */
    assert(strstr(out, KEY_MARK) == NULL);
    assert(strstr(out, DB_MARK) == NULL);
    free(out);
    printf("PASS test_directory_grant_masks_both\n");
}

/* Naming the DB file exactly does NOT unmask it. Such a grant is refused at
 * grant time (grant_path_hits_db), so it should never reach a mount plan at
 * all — this asserts the second layer: even if one is fabricated, the child
 * still sees nothing. The DB is the policy store for the containment
 * mechanism; reading it is reconnaissance and writing it is escalation. */
static void test_exact_db_grant_still_masked(void) {
    setup();
    char *out = read_state_files(DB);
    printf("  exact db grant → %s\n", out);
    assert(strstr(out, DECOY_MARK) != NULL);   /* positive control */
    assert(strstr(out, KEY_MARK) == NULL);
    assert(strstr(out, DB_MARK) == NULL);
    free(out);
    printf("PASS test_exact_db_grant_still_masked\n");
}

/* A grant naming the key file itself must still not expose it — the key is
 * masked unconditionally, and no grant may unmask it. */
static void test_exact_key_grant_still_masked(void) {
    setup();
    char *out = read_state_files(KEYF);
    printf("  exact key grant → %s\n", out);
    assert(strstr(out, DECOY_MARK) != NULL);   /* positive control */
    assert(strstr(out, KEY_MARK) == NULL);
    free(out);
    printf("PASS test_exact_key_grant_still_masked\n");
}

/* The shell tier is not the only way to read a file. file_read reaches the
 * same bytes with no shell, no network and no command string, and it is
 * granted far more widely — so the mask has to hold there too. Nothing
 * covered it: every assertion above drives the shell tier.
 *
 * Scope, honestly: this builds the request itself, so it proves the *broker*
 * masks on the file tier. It cannot catch a dispatch in main.c that fails to
 * put db_path on the wire — which is exactly how the file tier shipped
 * unmasked. That gap is closed by making db_path a required parameter of
 * run_tool_req_init() rather than a field a caller may forget, so the
 * compiler now rejects the omission; a runtime test would need a full agent
 * turn to reach that code.
 *
 * Drives the real --run-tool broker (not tool_file_tier_run in-process) —
 * masking happens in the child's sandbox setup, so an in-process call cannot
 * see it. */
static char *file_read_via_broker(const char *path, const char *grant_path) {
    ToolWireArg arg = { .key = (char *)"path", .kind = TOOL_ARG_TEXT,
                        .value = (char *)path };
    const char *reads[] = { grant_path };
    RunToolReq req = {
        .tier = RUNTOOL_TIER_FILE, .tool_name = "file_read",
        .env_mode = 1, .nproc = 64, .as_mb = 0, .cpu_sec = 60,
        .sandbox = 1, .net_mode = 1,
        .workspace = WS, .db_path = DB, .agent_dir = WS,
        .read_paths = reads, .read_count = 1,
        .params = &arg, .param_count = 1,
        .timeout = 5,
    };
    size_t blob_len = 0;
    char *blob = run_tool_serialize_request(&req, &blob_len);
    assert(blob);

    int sp[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sp[0]);
        if (sp[1] != 3) { dup2(sp[1], 3); close(sp[1]); }
        char *const argv[] = {"cclaw", "--run-tool", NULL};
        char *const envp[] = {NULL};
        execve(SHELL_BINARY, argv, envp);
        _exit(127);
    }
    close(sp[1]);
    size_t w = 0;
    while (w < blob_len) {
        ssize_t k = write(sp[0], blob + w, blob_len - w);
        if (k <= 0) break;
        w += (size_t)k;
    }
    free(blob);
    shutdown(sp[0], SHUT_WR);

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    assert(buf);
    for (;;) {
        if (len + 1024 > cap) { cap *= 2; buf = realloc(buf, cap); assert(buf); }
        ssize_t n = read(sp[0], buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sp[0]);
    buf[len] = '\0';
    int status;
    waitpid(pid, &status, 0);

    assert(len >= 4);
    size_t meta_len = ((size_t)(unsigned char)buf[0] << 24) |
                      ((size_t)(unsigned char)buf[1] << 16) |
                      ((size_t)(unsigned char)buf[2] << 8) |
                       (size_t)(unsigned char)buf[3];
    assert(4 + meta_len <= len);
    char *result = strdup(buf + 4 + meta_len);
    free(buf);
    return result;
}

static void test_file_tier_grant_masks_key_and_db(void) {
    setup();

    /* Positive control first, or the assertions below are worthless: an empty
     * read is what a masked file AND a broken request both look like. The
     * decoy sits in the same granted directory, so its content coming back
     * proves the mount landed and file_read works — an empty key or DB read
     * afterwards means masked, not merely absent. */
    char *out = file_read_via_broker(DECOY, STATEDIR);
    printf("  file_read, decoy under directory grant → %s\n", out);
    assert(strstr(out, DECOY_MARK) != NULL);
    free(out);

    out = file_read_via_broker(DB, STATEDIR);
    printf("  file_read, db under directory grant → %s\n", out);
    assert(strstr(out, DB_MARK) == NULL);
    free(out);

    out = file_read_via_broker(DB, DB);
    printf("  file_read, exact db grant → %s\n", out);
    assert(strstr(out, DB_MARK) == NULL);
    free(out);

    out = file_read_via_broker(KEYF, STATEDIR);
    printf("  file_read, directory grant → %s\n", out);
    assert(strstr(out, KEY_MARK) == NULL);
    free(out);

    /* Naming the key file exactly must not unmask it either. */
    out = file_read_via_broker(KEYF, KEYF);
    printf("  file_read, exact key grant → %s\n", out);
    assert(strstr(out, KEY_MARK) == NULL);
    free(out);
    printf("PASS test_file_tier_grant_masks_key_and_db\n");
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
    test_exact_db_grant_still_masked();
    test_exact_key_grant_still_masked();
    test_file_tier_grant_masks_key_and_db();
    system("rm -rf " ROOT);
    printf("All key-mask tests passed\n");
    return 0;
}
