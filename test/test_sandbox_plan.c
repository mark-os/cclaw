/* Unit tests for sandbox_plan_mounts: canonicalize + dedup (rw wins) + sort
 * shallow→deep. Uses real temp dirs + a symlink so realpath() has something to
 * resolve; no namespaces required (the planner is pure). */
#define _DEFAULT_SOURCE   /* mkdtemp, symlink, realpath */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sandbox.h"
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* Resolved temp layout, shared by tests. base stays small so derived paths
 * provably fit PATH_MAX (keeps -Wformat-truncation happy). */
static char base[64];
static char parent[PATH_MAX], child[PATH_MAX], link_[PATH_MAX], missing[PATH_MAX];
static char parent_real[PATH_MAX], child_real[PATH_MAX];

static int setup_tree(void) {
    snprintf(base, sizeof(base), "/tmp/cclaw_smt_XXXXXX");
    if (!mkdtemp(base)) return -1;
    snprintf(parent, sizeof(parent), "%s/parent", base);
    snprintf(child, sizeof(child), "%s/parent/child", base);
    snprintf(link_, sizeof(link_), "%s/link", base);
    snprintf(missing, sizeof(missing), "%s/does_not_exist", base);
    if (mkdir(parent, 0755) != 0) return -1;
    if (mkdir(child, 0755) != 0) return -1;
    if (symlink(parent, link_) != 0) return -1;
    /* Canonical truth to compare against (base itself may sit under a symlink). */
    if (!realpath(parent, parent_real)) return -1;
    if (!realpath(child, child_real)) return -1;
    return 0;
}

static const SandboxMount *find(const SandboxMount *m, size_t n, const char *path) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(m[i].path, path) == 0) return &m[i];
    return NULL;
}

/* rw wins over ro; symlink + duplicate paths collapse; missing path dropped. */
static void test_dedup_and_canon(void) {
    TEST(dedup_and_canon);
    SandboxMountReq in[] = {
        { child,   1 },  /* ro child */
        { parent,  0 },  /* rw parent */
        { parent,  1 },  /* ro dup — must NOT downgrade the rw above */
        { missing, 1 },  /* unresolvable — dropped */
        { link_,   1 },  /* symlink → parent — collapses into parent */
    };
    SandboxMount out[8];
    size_t n = sandbox_plan_mounts(in, sizeof(in)/sizeof(in[0]), out);
    if (n != 2) { FAIL("expected 2 deduped mounts"); return; }
    const SandboxMount *p = find(out, n, parent_real);
    const SandboxMount *c = find(out, n, child_real);
    if (!p || !c) { FAIL("missing parent/child in plan"); return; }
    if (p->ro != 0) { FAIL("rw parent downgraded to ro"); return; }
    if (c->ro != 1) { FAIL("child should stay ro"); return; }
    PASS();
}

/* Output is sorted shallow→deep regardless of input order. */
static void test_sort_order_independent(void) {
    TEST(sort_order_independent);
    /* Child listed before parent on purpose. */
    SandboxMountReq in[] = { { child, 0 }, { parent, 0 } };
    SandboxMount out[4];
    size_t n = sandbox_plan_mounts(in, 2, out);
    if (n != 2) { FAIL("expected 2 mounts"); return; }
    if (strcmp(out[0].path, parent_real) != 0) { FAIL("parent must sort first"); return; }
    if (strcmp(out[1].path, child_real) != 0) { FAIL("child must sort after parent"); return; }
    PASS();
}

/* Empty / NULL paths are skipped without corrupting the plan. */
static void test_skips_empty(void) {
    TEST(skips_empty);
    SandboxMountReq in[] = { { "", 1 }, { NULL, 0 }, { parent, 1 } };
    SandboxMount out[4];
    size_t n = sandbox_plan_mounts(in, 3, out);
    if (n != 1) { FAIL("expected 1 mount"); return; }
    if (strcmp(out[0].path, parent_real) != 0 || out[0].ro != 1) { FAIL("bad surviving entry"); return; }
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("--- test_sandbox_plan ---\n");
    if (setup_tree() != 0) { printf("FAIL: could not set up temp tree\n"); return 1; }
    test_dedup_and_canon();
    test_sort_order_independent();
    test_skips_empty();
    printf("%d/%d passed\n", tests_passed, tests_run);
    /* Best-effort cleanup (leave on failure for inspection). */
    if (tests_passed == tests_run) {
        rmdir(child); rmdir(parent); unlink(link_); rmdir(base);
    }
    return tests_passed == tests_run ? 0 : 1;
}
