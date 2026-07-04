/* Unit tests for sandbox_policy_from_profile / sandbox_profile_resolve: the
 * profile-name → containment-bundle mapping (specs/sandbox-profiles.md).
 * Pure functions, no namespaces required. Locks in the fall-through rule:
 * unknown/NULL profiles get the standard bundle, never something looser. */
#include <stdio.h>
#include <string.h>
#include "sandbox.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static SandboxConfig cfg_for(const char *profile) {
    SandboxConfig c; memset(&c, 0xAA, sizeof(c)); /* poison: catch unset fields */
    sandbox_policy_from_profile(profile, &c);
    return c;
}

static void test_host(void) {
    TEST(host_no_sandbox_no_limits);
    SandboxConfig c = cfg_for("host");
    if (c.sandbox == 0 && c.env_mode == 0 && c.net_mode == 0 &&
        c.mount_cwd == 1 && c.workspace_ro == 0 &&
        c.rlimits.nproc == 0 && c.rlimits.as_mb == 0 && c.rlimits.cpu_sec == 0)
        PASS();
    else FAIL("host bundle mismatch");
}

static void test_trusted(void) {
    TEST(trusted_sandboxed_loose);
    SandboxConfig c = cfg_for("trusted");
    if (c.sandbox == 1 && c.env_mode == 0 && c.net_mode == 0 &&
        c.mount_cwd == 1 && c.workspace_ro == 0 &&
        c.rlimits.nproc == 0 && c.rlimits.as_mb == 0 && c.rlimits.cpu_sec == 0)
        PASS();
    else FAIL("trusted bundle mismatch");
}

static void test_standard(void) {
    TEST(standard_clean_env_proxy);
    SandboxConfig c = cfg_for("standard");
    if (c.sandbox == 1 && c.env_mode == 1 && c.net_mode == 0 &&
        c.mount_cwd == 0 && c.workspace_ro == 0 &&
        c.rlimits.nproc == 64 && c.rlimits.as_mb == 512 && c.rlimits.cpu_sec == 60)
        PASS();
    else FAIL("standard bundle mismatch");
}

static void test_restricted(void) {
    TEST(restricted_no_net_ro_tight);
    SandboxConfig c = cfg_for("restricted");
    if (c.sandbox == 1 && c.env_mode == 1 && c.net_mode == 1 &&
        c.mount_cwd == 0 && c.workspace_ro == 1 &&
        c.rlimits.nproc == 8 && c.rlimits.as_mb == 128 && c.rlimits.cpu_sec == 10)
        PASS();
    else FAIL("restricted bundle mismatch");
}

static void test_fallthrough(void) {
    /* Unknown values must land on standard — a typo'd profile (or the old
     * 'bootstrap' string) must never yield a looser bundle than standard. */
    const char *unknowns[] = {NULL, "", "bootstrap", "TRUSTED", "hosT", "yolo"};
    SandboxConfig std = cfg_for("standard");
    TEST(unknown_and_null_fall_through_to_standard);
    for (size_t i = 0; i < sizeof(unknowns)/sizeof(unknowns[0]); i++) {
        SandboxConfig c = cfg_for(unknowns[i]);
        if (memcmp(&c, &std, sizeof(c)) != 0) {
            printf("FAIL: '%s' != standard bundle\n",
                   unknowns[i] ? unknowns[i] : "(null)");
            return;
        }
    }
    PASS();
}

static void test_profile_resolve_matches_policy(void) {
    /* sandbox_profile_resolve copies the policy half field-by-field (the
     * structs differ, so no memcmp across them) — pin each field instead. */
    static const char *profiles[] = {"host", "trusted", "standard", "restricted"};
    TEST(profile_resolve_matches_policy_fields);
    for (size_t i = 0; i < sizeof(profiles)/sizeof(profiles[0]); i++) {
        SandboxConfig c = cfg_for(profiles[i]);
        SandboxProfile p; memset(&p, 0, sizeof(p));
        sandbox_profile_resolve(profiles[i], &p);
        if (p.sandbox != c.sandbox || p.env_mode != c.env_mode ||
            p.net_mode != c.net_mode || p.mount_cwd != c.mount_cwd ||
            p.workspace_ro != c.workspace_ro ||
            p.rlimits.nproc != c.rlimits.nproc ||
            p.rlimits.as_mb != c.rlimits.as_mb ||
            p.rlimits.cpu_sec != c.rlimits.cpu_sec) {
            printf("FAIL: %s: profile/policy field drift\n", profiles[i]);
            return;
        }
        if (p.read_paths != NULL || p.write_paths != NULL ||
            p.read_path_count != 0 || p.write_path_count != 0) {
            printf("FAIL: %s: resolve touched grant-path fields\n", profiles[i]);
            return;
        }
    }
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_sandbox_profile:\n");
    test_host();
    test_trusted();
    test_standard();
    test_restricted();
    test_fallthrough();
    test_profile_resolve_matches_policy();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
