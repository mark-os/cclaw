/* A hook that does not come back (src/settle.c, src/dispatch.c): its own
 * deadline ends it, the daemon ends a broker that cannot end it, and a script
 * killed from outside is a failure like any other. In a suite of its own
 * because these waits are real seconds — the frozen-broker case alone is six —
 * and test_turn_settle.c holds the rest. Same fixture, same `host` profile.
 *
 * main() doubles as the broker: the children re-exec this binary. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define ROOT "/tmp/cclaw_test_hook_deadline"
#include "test_turn_settle.h"

/* A script that outlives its deadline is ended and the turn left untouched —
 * by the broker's own timer at the deadline, and, when the broker itself is
 * stuck, by the loop's sweep a few seconds behind it. The script blocks for
 * real (opening a FIFO nobody writes): a busy loop would trip the engine's
 * instruction limit and prove nothing about the clock. */
static void test_deadline(void) {
    assert(mkfifo(ROOT "/ws/never.fifo", 0600) == 0);
    install("slow", "Slow", "fs.readFile('" ROOT "/ws/never.fifo'); return '{}';");
    assert(config_set(g_db, "hook_timeout", "1") == 0);

    int64_t sid = seed_turn("Slow", 400);
    double t0 = now_s();
    assert(turn_settle_begin(sid, "Slow") == 1);
    assert(is_state(sid, "settling"));
    pump_until_idle(sid);
    double held = now_s() - t0;
    if (held < 0.9 || held > 4.0) {
        fprintf(stderr, "FAIL: held %.2fs, deadline was 1s\n", held);
        abort();
    }
    assert(count(N_EDITS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("slow") == 1);

    /* The backstop: freeze the broker so its timer cannot fire. */
    sid = seed_turn("Slow", 400);
    t0 = now_s();
    assert(turn_settle_begin(sid, "Slow") == 1);
    assert(child_count() == 1);
    assert(kill(child_at(0)->pid, SIGSTOP) == 0);
    pump_until_idle(sid);
    held = now_s() - t0;
    if (held < 5.0 || held > 10.0) {
        fprintf(stderr, "FAIL: frozen broker held %.2fs, kill was due at 6s\n", held);
        abort();
    }
    assert(count(N_EDITS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("slow") == 2);

    /* Killed from outside (the OOM killer, an operator): reaped as a signal
     * death, a failure like any other, and the hold ends at once. */
    sid = seed_turn("Slow", 400);
    t0 = now_s();
    assert(turn_settle_begin(sid, "Slow") == 1);
    assert(child_count() == 1);
    assert(kill(child_at(0)->pid, SIGKILL) == 0);
    pump_until_idle(sid);
    assert(now_s() - t0 < 0.9);
    assert(count(N_EDITS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("slow") == 3);

    /* A hook's own "timeout" overrides the configured default: here the
     * default is long and the hook's own second ends it. */
    assert(config_set(g_db, "hook_timeout", "20") == 0);
    install_hook("slow1", "Slow1", "Stop", ",\"timeout\":1",
                 "fs.readFile('" ROOT "/ws/never.fifo'); return '{}';");
    sid = seed_turn("Slow1", 400);
    t0 = now_s();
    assert(turn_settle_begin(sid, "Slow1") == 1);
    pump_until_idle(sid);
    held = now_s() - t0;
    if (held < 0.9 || held > 4.0) {
        fprintf(stderr, "FAIL: held %.2fs, the hook's own deadline was 1s\n", held);
        abort();
    }
    assert(fail_count("slow1") == 1);
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--run-tool") == 0)
        return run_tool_main();
    TEST_INIT();
    printf("test_hook_deadline:\n");
    fixture_open();
    RUN(test_deadline);
    fixture_close();
    printf("All hook_deadline tests passed.\n");
    return 0;
}
