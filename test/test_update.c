/* `cclaw update` — the compatibility handshake.
 *
 * This is the part that has to be right. Schema patches are forward-only, so
 * installing a binary that cannot open the live database is not a mistake you
 * undo by swapping the binary back: the old build then refuses a database
 * stamped newer than itself. Every case below is therefore a refusal, except
 * the one that genuinely fits. */

#include <stdio.h>
#include <string.h>
#include "update.h"
#include "cclaw.h"
#include "test_util.h"

static int tests_run = 0, tests_passed = 0;

static void expect(const char *label, const char *range, int db_version,
                   int want_ok, const char *want_reason_substr) {
    tests_run++;
    printf("  %s... ", label);
    char why[256] = "";
    int ok = update_schema_ok(range, db_version, why, sizeof(why));
    if (ok != want_ok) {
        printf("FAIL: expected %s, got %s (%s)\n",
               want_ok ? "accept" : "refuse", ok ? "accept" : "refuse", why);
        return;
    }
    if (!want_ok && want_reason_substr && !strstr(why, want_reason_substr)) {
        printf("FAIL: reason %s did not mention %s\n", why, want_reason_substr);
        return;
    }
    tests_passed++;
    printf("PASS\n");
}

int main(void) {
    TEST_INIT();
    printf("test_update:\n");

    /* The ordinary case: our own range, our own database. */
    char ours[64];
    snprintf(ours, sizeof(ours), "min=%d current=%d",
             CCLAW_SCHEMA_MIN, CCLAW_SCHEMA_VERSION);
    expect("same_build_accepts", ours, CCLAW_SCHEMA_VERSION, 1, NULL);

    /* A newer build that still patches from our floor is the whole point. */
    expect("newer_build_accepts", "min=40 current=99", 51, 1, NULL);

    /* The case this feature exists for: the database predates the candidate's
     * floor, so the candidate would refuse to open it. Exactly the v30-vs-v40
     * wall the Pogoplug was behind. */
    expect("db_below_candidate_floor_refuses", "min=40 current=51", 30, 0,
           "patches forward from");

    /* Downgrade: the candidate does not know this database's shape. */
    expect("db_newer_than_candidate_refuses", "min=40 current=45", 51, 0,
           "downgrade");

    /* A binary that cannot say what it accepts is not one we can reason about,
     * which also covers every build older than --schema-range itself. */
    expect("no_answer_refuses", NULL, 51, 0, "does not answer");
    expect("empty_answer_refuses", "", 51, 0, "does not answer");
    expect("garbage_refuses", "cclaw 1307fc3 (2026-08-28)", 51, 0, "unparseable");
    expect("partial_refuses", "min=40", 51, 0, "unparseable");
    expect("nonsense_range_refuses", "min=99 current=40", 51, 0, "unparseable");
    expect("zero_min_refuses", "min=0 current=0", 51, 0, "unparseable");

    /* Boundary: a database sitting exactly on either end is fine. */
    expect("db_at_floor_accepts", "min=40 current=51", 40, 1, NULL);
    expect("db_at_ceiling_accepts", "min=40 current=51", 51, 1, NULL);

    /* Loader noise before the marker must not break the handshake — a custom
     * libcurl prints "no version information available" to stderr on every
     * exec, and the capture merges stderr (the CI websocket target found
     * this; a real deployment with a hand-built library would hit it too). */
    expect("loader_noise_prefix_accepts",
           "/opt/lib/libcurl.so.4: no version information available "
           "min=40 current=51", 51, 1, NULL);

    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
