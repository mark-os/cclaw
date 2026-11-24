/* The events end to end (src/settle.c): real hook scripts, forked through
 * the real --run-tool broker, against a session in a temp DB. What is proven
 * here is the hold — a session enters 'settling', nothing consumes its inbox
 * meanwhile, and every way the script can end returns the session to where
 * it goes next with the right thing done to it — and the order at the end of
 * an iteration: Stop / SubagentStop, then ContextThreshold while over the
 * threshold, then C's trim, then the next request or idle.
 *
 * The scripts run under containment `none` so the test does not depend on
 * the machine having unprivileged user namespaces; the sandboxed path is the
 * same code and is exercised on a real instance (specs/hooks.md).
 *
 * main() doubles as the broker: the children re-exec this binary. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define ROOT "/tmp/cclaw_test_turn_settle"
#include "test_turn_settle.h"

/* The good path: held, inbox left alone, reply applied, released. Also what
 * the script is handed: the turn as `args`, of any size (this one is far past
 * the 32KB request blob), and its own extension's settings. */
static void test_reviewed(void) {
    install("rv", "Rv",
        "if (args.event !== 'Stop' || args.iterations[0].calls[0].call_id !== 'c1')"
        "  throw new Error('unexpected input');"
        "if (args.iterations[0].calls[0].result.text.split('\\n').length !== 4001)"
        "  throw new Error('truncated input');"
        "if (getConfig('greeting') !== 'hello') throw new Error('no config');"
        DROP_C1);
    int64_t sid = seed_turn("Rv", 4000);
    assert(count(N_CALLS, sid) == 1);

    assert(turn_settle_begin(sid, "Rv") == 1);
    assert(is_state(sid, "settling"));
    assert(child_hook_holds(sid));

    /* A message arriving now queues; advancing the session decides nothing. */
    assert(inbox_insert(g_db, sid, "cli", NULL, "are you there") > 0);
    AdvanceOutput out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_WAITING);
    assert(is_state(sid, "settling"));
    assert(count("SELECT COUNT(*) FROM inbox WHERE session_id=?1 AND consumed=0", sid) == 1);
    /* The sweep leaves a hold with a live child alone. */
    turn_settle_sweep();
    assert(is_state(sid, "settling"));

    /* No LLM worker in this process, so the turn the queued message opens
     * cannot be dispatched; that it was opened is the point. */
    for (int i = 0; i < 240 && child_count() > 0; i++) {
        reap_children();
        usleep(50 * 1000);
    }
    assert(child_count() == 0);
    assert(!is_state(sid, "settling"));
    assert(count(N_EDITS, sid) == 1);
    assert(count(N_NOTES, sid) == 1);
    assert(count(N_CALLS, sid) == 0);
    /* The record names the extension and carries the script's own part. */
    assert(count("SELECT COUNT(*) FROM entries WHERE session_id=?1 AND json_valid(data)"
                 " AND json_extract(data,'$.turn_edits[0].by')='hook:rv'"
                 " AND json_extract(data,'$.turn_edits[0].record.by_script')=1", sid) == 1);
    assert(count("SELECT COUNT(*) FROM inbox WHERE session_id=?1 AND consumed=0", sid) == 0);
    /* The queued message became the next turn's user entry, after the
     * reviewed turn's answer. */
    assert(count("SELECT type='user_message' AND content='are you there' FROM entries"
                 " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1)", sid) == 1);
    assert(fail_count("rv") == 0);
}

/* Garbage, an exception, and a clean "nothing to do": the turn is untouched
 * each time; the first two count as failures, the third as a success. */
static void test_outcomes(void) {
    install("junk", "Junk", "return 'not json {';");
    install("boom", "Boom", "throw new Error('reviewer blew up');");
    install("nil", "Nil", "return '{}';");
    static const struct { const char *agent, *ext; int fails; } cases[] = {
        { "Junk", "junk", 1 }, { "Boom", "boom", 1 }, { "Nil", "nil", 0 } };
    for (size_t i = 0; i < 3; i++) {
        int64_t sid = seed_turn(cases[i].agent, 400);
        assert(turn_settle_begin(sid, cases[i].agent) == 1);
        assert(is_state(sid, "settling"));
        pump_until_idle(sid);
        assert(count(N_EDITS, sid) == 0 && count(N_NOTES, sid) == 0);
        assert(count(N_CALLS, sid) == 1);
        printf("    %s\n", cases[i].ext);
        assert(fail_count(cases[i].ext) == cases[i].fails);
    }
    /* A run that works in between starts the count over (nil above); three
     * in a row disable the hook (test_disabled). */
    int64_t sid = seed_turn("Junk", 400);
    assert(turn_settle_begin(sid, "Junk") == 1);
    pump_until_idle(sid);
    assert(fail_count("junk") == 2);
}

/* Two awaited hooks on one agent run one at a time in extension-name order,
 * each on the turn as the one before left it: the second sees the first's
 * drop and note, and edits the same turn again. A reply the runtime refuses
 * is charged to its script, and the session is not stuck. */
static void test_chain(void) {
    install("ch_a", "Chain", DROP_C1);
    install("ch_b", "Chain",
            "if (args.iterations.length !== 0) throw new Error('saw the unedited turn');"
            "if (args.notes.length !== 1 || args.notes[0].text !== 'dropped the lookup')"
            "  throw new Error('did not see the first note');"
            "return JSON.stringify({edits:[{op:'note',text:'second opinion'}]});");
    install("ch_c", "Chain",
            "return JSON.stringify({edits:[{op:'drop',call_id:'c1'}]});");
    int64_t sid = seed_turn("Chain", 400);
    assert(turn_settle_begin(sid, "Chain") == 1);
    assert(child_count() == 1);                 /* one at a time */
    pump_until_idle(sid);
    assert(count(N_EDITS, sid) == 2 && count(N_CALLS, sid) == 0);
    assert(count("SELECT group_concat(content || '@' || json_extract(data,'$.source'), '|')"
                 " = 'dropped the lookup@hook:ch_a|second opinion@hook:ch_b' FROM ("
                 " SELECT content, data FROM entries WHERE session_id=?1 AND type='system'"
                 " ORDER BY id)", sid) == 1);
    assert(fail_count("ch_a") == 0);
    assert(fail_count("ch_b") == 0);
    assert(fail_count("ch_c") == 1);            /* c1 is gone: rejected, not thrown */
}

/* An awaited hook that cannot be started is passed over, not the end of
 * the chain: the first here is refused the turn (over its max_bytes), the
 * second has lost its script, and the third still runs and edits. */
static void test_passed_over(void) {
    install("po_a", "Pass", DROP_C1);
    run("UPDATE hooks SET input='{\"max_bytes\":2000}' WHERE extension_name='po_a';");
    install("po_b", "Pass", DROP_C1);
    assert(unlink(ROOT "/extensions/po_b/h.qjs") == 0);
    install("po_c", "Pass", "return JSON.stringify({edits:[{op:'note',text:'still here'}]});");
    int64_t sid = seed_turn("Pass", 400);
    assert(turn_settle_begin(sid, "Pass") == 1);
    assert(is_state(sid, "settling") && child_count() == 1);
    pump_until_idle(sid);
    assert(count(N_EDITS, sid) == 1 && count(N_CALLS, sid) == 1);
    assert(count("SELECT json_extract(data,'$.source') = 'hook:po_c' FROM entries"
                 " WHERE session_id=?1 AND type='system'", sid) == 1);
    assert(fail_count("po_a") == 0 && fail_count("po_b") == 0 && fail_count("po_c") == 0);
}

/* async — started, never held for, and its reply goes nowhere. */
static void test_not_awaited(void) {
    install_as("obs", "Obs", 0, DROP_C1);
    assert(count("SELECT async FROM hooks WHERE extension_name='obs' AND ?1 = 1", 1) == 1);
    int64_t sid = seed_turn("Obs", 400);
    assert(turn_settle_begin(sid, "Obs") == 0);
    assert(is_state(sid, "idle"));
    assert(child_count() == 1 && !child_hook_holds(sid));
    pump_until_idle(sid);
    assert(count(N_EDITS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("obs") == 0);
}

/* Nothing to run, nothing held: a sub-agent's session, an agent without a
 * hook. And a hook whose script is gone starts nothing. */
static void test_not_started(void) {
    int64_t parent = seed_turn("Rv", 2);
    int64_t sub = seed_turn("Rv", 400);
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE sessions SET parent_session_id=%lld, depth=1"
             " WHERE id=%lld;", (long long)parent, (long long)sub);
    run(sql);
    assert(turn_settle_begin(sub, "Rv") == 0);
    assert(is_state(sub, "idle") && child_count() == 0);

    db_agent_upsert(g_db, "Plain", NULL, NULL);
    int64_t plain = seed_turn("Plain", 400);
    assert(turn_settle_begin(plain, "Plain") == 0);
    assert(is_state(plain, "idle") && child_count() == 0);

    install("gone", "Gone", DROP_C1);
    assert(unlink(ROOT "/extensions/gone/h.qjs") == 0);
    int64_t sid = seed_turn("Gone", 400);
    assert(turn_settle_begin(sid, "Gone") == 0);
    assert(is_state(sid, "idle") && child_count() == 0);
    assert(fail_count("gone") == 0);            /* not a failure of the script */

    /* A finished turn larger than the hook's max_bytes is not handed over:
     * not started, the session never held, not a failure of the script. */
    install("big", "Big", DROP_C1);
    run("UPDATE hooks SET input='{\"max_bytes\":2000}' WHERE extension_name='big';");
    sid = seed_turn("Big", 400);
    assert(turn_settle_begin(sid, "Big") == 0);
    assert(is_state(sid, "idle") && child_count() == 0);
    assert(count(N_EDITS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("big") == 0);
}

/* The script is sent what its hooks row asks for: here the conversation
 * before the turn, the earlier results without their text. */
static void test_declared_input(void) {
    install("hist", "Hist",
        "var c = args.history[0].iterations[0].calls[0];"
        "return JSON.stringify({edits:[{op:'note',text:'history=' + args.history.length"
        " + ' asked=' + args.input.history + ' omitted=' + args.history_omitted"
        " + ' text=' + (c.result.text === undefined ? 'none' : 'some')"
        " + ' tokens=' + (c.result.tokens > 0)}]});");
    run("UPDATE hooks SET input='{\"history\":\"all\",\"history_results\":false}'"
        " WHERE extension_name='hist';");
    /* (A Stop script is sent no "context": that is ContextThreshold's.) */
    int64_t sid = seed_turn("Hist", 40);
    Message u = {.role = ROLE_USER, .content = "and then?"};
    assert(entry_append_with_iteration(g_db, sid, &u, 0) > 0);
    assert(entry_append_typed(g_db, sid, db_next_iteration_id(g_db, sid), "assistant_message",
                              0, "nothing more", NULL, NULL, 0, STOP_REASON_NONE, NULL,
                              0, 0, 0) > 0);
    assert(turn_settle_begin(sid, "Hist") == 1);
    pump_until_idle(sid);
    assert(count("SELECT COUNT(*) FROM entries WHERE session_id=?1 AND type='system'"
                 " AND content='history=1 asked=all omitted=0 text=none tokens=true'", sid) == 1);
    assert(fail_count("hist") == 0);
}

/* The lost-wake backstop: a session this process holds with no hook child
 * behind it is released by the sweep. */
static void test_sweep(void) {
    int64_t sid = seed_turn("Rv", 400);
    assert(session_set_settling(g_db, sid, "Stop") == 0);
    assert(count("SELECT settle_event='Stop' FROM sessions WHERE id=?1", sid) == 1);
    assert(!child_hook_holds(sid));
    turn_settle_sweep();
    assert(is_state(sid, "idle"));
    assert(count(N_EDITS, sid) == 0 && count(N_CALLS, sid) == 1);
}

/* Routing: a sub-agent's turn fires SubagentStop and a top-level one Stop,
 * with the same reply. A hook for one is not a hook for the other. */
static void test_subagent_stop(void) {
    install_hook("subh", "Sub", "SubagentStop", "",
        "if (args.event !== 'SubagentStop') throw new Error('wrong event');" DROP_C1);
    int64_t parent = seed_turn("Sub", 400);
    int64_t sub = seed_turn("Sub", 400);
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE sessions SET parent_session_id=%lld, depth=1"
             " WHERE id=%lld;", (long long)parent, (long long)sub);
    run(sql);
    assert(turn_settle_begin(parent, "Sub") == 0);
    assert(is_state(parent, "idle") && child_count() == 0);
    assert(turn_settle_begin(sub, "Sub") == 1);
    assert(is_state(sub, "settling"));
    pump_until_idle(sub);
    assert(count(N_EDITS, sub) == 1 && count(N_CALLS, sub) == 0);
    assert(count(N_CALLS, parent) == 1);
    assert(fail_count("subh") == 0);
}

/* The governing routing entry's limits for `agent`. */
static void set_limits(const char *agent, int threshold, int target) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "DELETE FROM agent_models WHERE agent_name='%s';"
             "INSERT INTO agent_models(agent_name, model_id, pos, trim_threshold, prune_target)"
             " VALUES('%s','p/m',0,%d,%d);", agent, agent, threshold, target);
    run(sql);
}

/* The session's size as the C check sees it. */
static int measure(int64_t sid, const char *agent) {
    int total = turn_edit_measure(g_db, sid, agent, NULL);
    assert(total > 0);
    return total;
}

/* A trim note C left (turn_edit_trim), and how many whole turns it dropped. */
#define N_TRIMS "SELECT COUNT(*) FROM entries WHERE session_id=?1 AND type='system'" \
                " AND json_extract(data,'$.source')='" TRIM_SOURCE "'"
#define TRIMMED_TURNS "SELECT COALESCE(SUM(json_extract(data,'$.turn_edits[0].dropped.turns')),0)" \
                " FROM entries WHERE session_id=?1 AND type='system'" \
                " AND json_extract(data,'$.source')='" TRIM_SOURCE "'"

/* Two turns, the first large; the second (the one that just ended) makes a
 * call the Stop hook drops. */
static int64_t seed_long(const char *agent) {
    int64_t sid = session_create(g_db, "t", agent, -1, 0);
    assert(sid > 0);
    add_turn(sid, 2000, 0);
    add_turn(sid, 400, 0);
    return sid;
}

/* The order at the end of a turn, one hold for all of it: the Stop hook
 * edits first; the ContextThreshold hook, the session still over its
 * threshold, sees that edit and the numbers, before C has trimmed anything;
 * then C trims to prune_target; then idle. */
static void test_turn_end_order(void) {
    install_hook("ord_a", "Ord", "Stop", "",
        "return JSON.stringify({edits:[{op:'drop',call_id:'c1'}]});");
    install_hook("ord_b", "Ord", "ContextThreshold", ",\"input\":{\"history\":\"all\"}",
        "var c = args.context;"
        "if (args.event !== 'ContextThreshold' || args.iterations.length !== 0"
        "    || args.final.text !== 'done' || args.history.length !== 1"
        "    || !(c.tokens > c.trim_threshold) || !(c.prune_target < c.trim_threshold)"
        "    || !(c.window >= c.trim_threshold)"
        "    || !(args.history[0].iterations[0].calls[0].result.tokens > 0))"
        "  throw new Error('unexpected input ' + JSON.stringify(c));"
        "return JSON.stringify({edits:[{op:'note',text:'ct saw ' + c.tokens}]});");
    int64_t sid = seed_long("Ord");
    int total = measure(sid, "Ord");
    set_limits("Ord", total / 4, total / 8);

    assert(turn_settle_begin(sid, "Ord") == 1);
    assert(is_state(sid, "settling") && child_count() == 1);
    pump_until_idle(sid);
    assert(fail_count("ord_a") == 0 && fail_count("ord_b") == 0);
    /* The second turn's call left with the Stop hook's edit, the first
     * turn's with C's trim. */
    assert(count(N_CALLS, sid) == 0);
    assert(count("SELECT COUNT(*) FROM entries WHERE session_id=?1 AND type='tool_call'", sid) == 2);
    assert(count("SELECT COUNT(*) FROM entries WHERE session_id=?1 AND type='system'"
                 " AND content LIKE 'ct saw %' AND json_extract(data,'$.source')='hook:ord_b'",
                 sid) == 1);
    assert(count(N_TRIMS, sid) == 1 && count(TRIMMED_TURNS, sid) == 1);
    assert(measure(sid, "Ord") < total / 2);

    /* The same with the ContextThreshold hook crashing: nothing it did,
     * counted against it, and C's trim still runs. */
    install_hook("ordx", "OrdX", "ContextThreshold", "", "throw new Error('compactor died');");
    sid = seed_long("OrdX");
    total = measure(sid, "OrdX");
    set_limits("OrdX", total / 4, total / 8);
    assert(turn_settle_begin(sid, "OrdX") == 1);
    pump_until_idle(sid);
    assert(fail_count("ordx") == 1);
    assert(count(N_TRIMS, sid) == 1 && count(TRIMMED_TURNS, sid) == 1);
    assert(count(N_NOTES, sid) == 0);

    /* Under the threshold: no ContextThreshold hook runs, nothing is held. */
    sid = seed_long("OrdX");
    total = measure(sid, "OrdX");
    set_limits("OrdX", total + 1000, total / 2);
    assert(turn_settle_begin(sid, "OrdX") == 0);
    assert(is_state(sid, "idle") && child_count() == 0);
    assert(fail_count("ordx") == 1);

    /* A Stop hook that gets the session under the threshold ends it there:
     * the ContextThreshold hook does not fire, C has nothing to do. */
    install_hook("ord_c", "OrdS", "Stop", "",
        "return JSON.stringify({edits:[{op:'drop',call_id:'c1'}]});");
    install_hook("ord_d", "OrdS", "ContextThreshold", "", "throw new Error('must not run');");
    sid = session_create(g_db, "t", "OrdS", -1, 0);
    add_turn(sid, 4000, 0);
    total = measure(sid, "OrdS");
    set_limits("OrdS", total - 1000, (total - 1000) / 2);
    assert(turn_settle_begin(sid, "OrdS") == 1);
    pump_until_idle(sid);
    assert(fail_count("ord_c") == 0 && fail_count("ord_d") == 0);
    assert(count(N_TRIMS, sid) == 0 && count(N_CALLS, sid) == 0);
}

/* Between iterations: advance_session counts the iteration once and hands
 * over to the hooks; the session is held (messages queue) while the
 * ContextThreshold script looks at the turn still running; then C's trim and
 * the next request. A reply that gets the session under the threshold —
 * here by dropping the earlier turn and leaving a note at the end of the
 * running one — leaves C nothing to do. */
static int64_t seed_mid_turn(const char *agent) {
    int64_t sid = session_create(g_db, "t", agent, -1, 0);
    assert(sid > 0);
    add_turn(sid, 2000, 0);
    add_turn(sid, 10, 1);       /* running: its call answered, no request yet */
    assert(session_set_state(g_db, sid, "llm_running") == 0);
    assert(session_set_state(g_db, sid, "tool_running") == 0);
    int total = measure(sid, agent);
    set_limits(agent, total - 1000, (total - 1000) / 2);
    return sid;
}

static int iteration_of(int64_t sid) {
    return (int)count("SELECT turn_iteration FROM sessions WHERE id=?1", sid);
}

static void pump_until_released(int64_t sid) {
    for (int i = 0; i < 240; i++) {
        reap_children();
        child_sweep_deadlines();
        if (!is_state(sid, "settling") && child_count() == 0) return;
        usleep(50 * 1000);
    }
    fprintf(stderr, "FAIL: session %lld still held after 12s\n", (long long)sid);
    abort();
}

static void test_iteration_hold(void) {
    install_hook("mid", "Mid", "ContextThreshold", "",
        "if (args.final !== null || args.iterations.length !== 1"
        "    || args.iterations[0].calls[0].result.text.indexOf('result line') !== 0"
        "    || !(args.context.tokens > args.context.trim_threshold))"
        "  throw new Error('unexpected input');"
        "return '{}';");
    int64_t sid = seed_mid_turn("Mid");

    AdvanceOutput out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_RUN_HOOKS && out.iteration == 1);
    assert(is_state(sid, "tool_running") && iteration_of(sid) == 1);
    assert(iteration_settle_begin(sid, "Mid") == 1);
    assert(is_state(sid, "settling") && child_hook_holds(sid));
    /* Held: a message queues, advancing decides nothing, the iteration is
     * not counted again. */
    assert(inbox_insert(g_db, sid, "cli", NULL, "are you done") > 0);
    out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_WAITING && iteration_of(sid) == 1);
    pump_until_released(sid);
    /* Released into the next request (no LLM worker here: it stays
     * llm_running), C having trimmed the earlier turn first. */
    assert(is_state(sid, "llm_running") && iteration_of(sid) == 1);
    assert(fail_count("mid") == 0);
    assert(count(N_TRIMS, sid) == 1 && count(TRIMMED_TURNS, sid) == 1);
    assert(count("SELECT COUNT(*) FROM inbox WHERE session_id=?1 AND consumed=0", sid) == 1);

    /* An edit between iterations is applied: the session is under the
     * threshold after it, so C's check trims nothing. */
    install_hook("midedit", "MidEdit", "ContextThreshold", "",
        "return JSON.stringify({edits:[{op:'drop_turns',from_turn:1,to_turn:1},"
        "                              {op:'note',text:'shorter now'}]});");
    sid = seed_mid_turn("MidEdit");
    out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_RUN_HOOKS);
    assert(iteration_settle_begin(sid, "MidEdit") == 1);
    pump_until_released(sid);
    assert(is_state(sid, "llm_running") && fail_count("midedit") == 0);
    assert(count(N_NOTES, sid) == 1 && count(N_TRIMS, sid) == 0);
    assert(count("SELECT type='system' FROM entries WHERE id=(SELECT leaf_id FROM sessions"
                 " WHERE id=?1)", sid) == 1);

    /* A hook that times out: the same. */
    install_hook("midslow", "MidSlow", "ContextThreshold", ",\"timeout\":1",
                 "fs.readFile('" ROOT "/ws/never.fifo'); return '{}';");
    sid = seed_mid_turn("MidSlow");
    out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_RUN_HOOKS);
    assert(iteration_settle_begin(sid, "MidSlow") == 1);
    pump_until_released(sid);
    assert(is_state(sid, "llm_running") && fail_count("midslow") == 1);
    assert(count(N_TRIMS, sid) == 1);

    /* Under the threshold the hook is not run and nothing is held: the
     * caller goes straight on to the C check and the next request. */
    sid = seed_mid_turn("Mid");
    set_limits("Mid", measure(sid, "Mid") + 1000, 100);
    out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_RUN_HOOKS);
    assert(iteration_settle_begin(sid, "Mid") == 0);
    assert(is_state(sid, "tool_running") && child_count() == 0);
    run_iteration_end(sid);
    assert(is_state(sid, "llm_running") && iteration_of(sid) == 1);
    assert(count(N_TRIMS, sid) == 0);

    /* The sweep's backstop between iterations: a hold whose child is gone
     * unheard-of ends where a reaped one would — the next request. */
    sid = seed_mid_turn("Mid");
    out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_RUN_HOOKS);
    assert(session_set_state(g_db, sid, "settling") == 0);
    turn_settle_sweep();
    assert(is_state(sid, "llm_running") && iteration_of(sid) == 1);
    assert(count(N_TRIMS, sid) == 1);

    /* An agent with no ContextThreshold hook is never handed over. */
    db_agent_upsert(g_db, "NoCt", NULL, NULL);
    sid = seed_mid_turn("NoCt");
    out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_DISPATCH_LLM && iteration_of(sid) == 1);
    assert(count(N_TRIMS, sid) == 1);
}

/* A session held for a hook holds a slot of session_max_concurrent: another
 * session's autonomous turn defers while it lasts. */
static void test_hold_is_a_resource(void) {
    install("res", "Res", "fs.readFile('" ROOT "/ws/never.fifo'); return '{}';");
    int64_t held = seed_turn("Res", 10);
    int64_t other = seed_turn("Res", 10);
    assert(config_set(g_db, "session_max_concurrent", "1") == 0);
    assert(inbox_insert(g_db, other, "cron", "job", "tick") > 0);
    assert(turn_settle_begin(held, "Res") == 1);
    AdvanceOutput out = advance_session(g_db, other, 0);
    assert(out.action == ADVANCE_NOOP && out.deferred == 1);
    assert(is_state(other, "idle"));
    assert(kill(child_at(0)->pid, SIGKILL) == 0);
    pump_until_idle(held);
    assert(config_set(g_db, "session_max_concurrent", "0") == 0);
}

/* CacheExpiring, fired from the timer (cache_expiring_tick at the real
 * clock: the arm is written into the past, so nothing sleeps). The session is
 * idle and over even its trim threshold, which proves the point of decision
 * 23: whatever the script does, C never trims an idle session after it. */
static void arm_due(int64_t sid) {
    char sql[160];
    snprintf(sql, sizeof(sql), "UPDATE sessions SET cache_armed_at=unixepoch()-3300"
             " WHERE id=%lld;", (long long)sid);
    run(sql);
}

static int64_t seed_cache(const char *agent) {
    int64_t sid = seed_long(agent);
    int total = measure(sid, agent);
    char sql[512];
    snprintf(sql, sizeof(sql),
             "DELETE FROM agent_models WHERE agent_name='%s';"
             "INSERT INTO agent_models(agent_name, model_id, pos, trim_threshold,"
             " prune_target, cache_lifetime) VALUES('%s','p/m',0,%d,%d,3600);",
             agent, agent, total / 2, total / 4);
    run(sql);
    arm_due(sid);
    return sid;
}

#define ARMED "SELECT cache_armed_at IS NOT NULL FROM sessions WHERE id=?1"

static void test_cache_expiring(void) {
    /* No script: nothing happens, and the period is spent. */
    int64_t sid = seed_cache("CeNone");
    int before = measure(sid, "CeNone");
    cache_expiring_tick();
    assert(is_state(sid, "idle") && child_count() == 0);
    assert(count(ARMED, sid) == 0);
    assert(measure(sid, "CeNone") == before && count(N_TRIMS, sid) == 0);

    /* A failed script: nothing it did, no trim, counted against it. Fired
     * once: the next tick finds nothing armed. */
    install_hook("ceboom", "CeBoom", "CacheExpiring", "", "throw new Error('summary died');");
    sid = seed_cache("CeBoom");
    before = measure(sid, "CeBoom");
    cache_expiring_tick();
    assert(is_state(sid, "settling") && child_count() == 1);
    assert(count("SELECT settle_event='CacheExpiring' FROM sessions WHERE id=?1", sid) == 1);
    pump_until_idle(sid);
    assert(count("SELECT settle_event IS NULL FROM sessions WHERE id=?1", sid) == 1);
    assert(fail_count("ceboom") == 1);
    assert(count(N_EDITS, sid) == 0 && count(N_TRIMS, sid) == 0);
    assert(measure(sid, "CeBoom") == before);
    cache_expiring_tick();
    assert(is_state(sid, "idle") && child_count() == 0 && fail_count("ceboom") == 1);

    /* A reply with no edits: nothing, and a success. */
    install_hook("cenil", "CeNil", "CacheExpiring", "", "return '{}';");
    sid = seed_cache("CeNil");
    before = measure(sid, "CeNil");
    cache_expiring_tick();
    pump_until_idle(sid);
    assert(fail_count("cenil") == 0 && count(N_EDITS, sid) == 0 && count(N_TRIMS, sid) == 0);
    assert(measure(sid, "CeNil") == before);

    /* What the script is sent, and an edit applied while a message waits:
     * the hold is a hold like any other — the message queues, the session
     * counts as holding a resource — and the message opens the next turn
     * when it ends. */
    install_hook("ceok", "CeOk", "CacheExpiring", ",\"input\":{\"history\":\"all\"}",
        "var c = args.context;"
        "if (args.event !== 'CacheExpiring' || c.depth !== 0 || c.cache_lifetime !== 3600"
        "    || !(c.tokens > c.prune_target) || !(c.trim_threshold > c.prune_target)"
        "    || !(c.window >= c.trim_threshold) || c.cache_read_tokens !== 1800"
        "    || c.cache_write_tokens !== 248 || !(c.idle_seconds >= 0)"
        "    || args.final.text !== 'done' || args.history.length !== 1)"
        "  throw new Error('unexpected input ' + JSON.stringify(c));"
        "return JSON.stringify({edits:[{op:'note',text:'ce saw ' + c.tokens}]});");
    sid = seed_cache("CeOk");
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO llm_responses(session_id, iteration_id, model, status,"
             " cached_tokens, cache_write_tokens) VALUES (%lld, 1, 'p/m', 'ok', 1800, 248);",
             (long long)sid);
    run(sql);
    cache_expiring_tick();
    assert(is_state(sid, "settling") && child_hook_holds(sid));
    assert(session_count_resource_holders(g_db, 0) >= 1);
    assert(inbox_insert(g_db, sid, "cli", NULL, "still there?") > 0);
    AdvanceOutput out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_WAITING && is_state(sid, "settling"));
    for (int i = 0; i < 240 && child_count() > 0; i++) {
        reap_children();
        usleep(50 * 1000);
    }
    assert(child_count() == 0 && !is_state(sid, "settling"));
    assert(fail_count("ceok") == 0);
    assert(count("SELECT COUNT(*) FROM entries WHERE session_id=?1 AND type='system'"
                 " AND content LIKE 'ce saw %' AND json_extract(data,'$.source')='hook:ceok'",
                 sid) == 1);
    assert(count(N_TRIMS, sid) == 0);
    assert(count("SELECT type='user_message' AND content='still there?' FROM entries"
                 " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1)", sid) == 1);

    /* The sweep ends a CacheExpiring hold whose child went unheard-of as the
     * reap would: idle, and no trim — the hold says what it was for. */
    sid = seed_cache("CeNone");
    before = measure(sid, "CeNone");
    assert(session_set_settling(g_db, sid, "CacheExpiring") == 0);
    turn_settle_sweep();
    assert(is_state(sid, "idle") && count(N_TRIMS, sid) == 0);
    assert(measure(sid, "CeNone") == before);

    /* At or under the prune target: no hook, nothing held. */
    sid = seed_cache("CeOk");
    snprintf(sql, sizeof(sql), "UPDATE agent_models SET trim_threshold=NULL, prune_target=%d"
             " WHERE agent_name='CeOk';", measure(sid, "CeOk") + 10);
    run(sql);
    cache_expiring_tick();
    assert(is_state(sid, "idle") && child_count() == 0 && count(ARMED, sid) == 0);
    assert(count(N_NOTES, sid) == 0);

    /* A sub-agent's session fires too: the depth is the script's business. */
    install_hook("cesub", "CeSub", "CacheExpiring", "",
        "if (args.context.depth !== 1) throw new Error('depth ' + args.context.depth);"
        "return '{}';");
    int64_t parent = seed_turn("CeSub", 10);
    sid = seed_cache("CeSub");
    snprintf(sql, sizeof(sql), "UPDATE sessions SET parent_session_id=%lld, depth=1"
             " WHERE id=%lld;", (long long)parent, (long long)sid);
    run(sql);
    cache_expiring_tick();
    assert(is_state(sid, "settling"));
    pump_until_idle(sid);
    assert(fail_count("cesub") == 0);
}

/* Three failures in a row disable the hook: one note to the default agent,
 * the hook passed over from then on, until the extension is promoted
 * again. */
static void test_disabled(void) {
    char *def = db_default_agent_name(g_db);
    assert(def);
    int64_t dsid = db_recent_session(g_db, def);
    if (dsid <= 0) {
        db_agent_upsert(g_db, def, NULL, NULL);
        dsid = session_create(g_db, "d", def, -1, 0);
    }
    assert(dsid > 0);
    #define N_TOLD "SELECT COUNT(*) FROM inbox WHERE session_id=?1 AND source='system'" \
                   " AND source_ref='hook' AND payload LIKE '%''trip''%disabled%'"
    install("trip", "Trip", "return 'not json';");
    for (int n = 1; n <= 3; n++) {
        int64_t sid = seed_turn("Trip", 10);
        assert(turn_settle_begin(sid, "Trip") == 1);
        pump_until_idle(sid);
        assert(fail_count("trip") == n);
        assert(count(N_TOLD, dsid) == (n == 3));
    }
    assert(count("SELECT disabled FROM hooks WHERE extension_name='trip' AND ?1", 1) == 1);
    /* The note names the one hook: extension, event and script. */
    assert(count("SELECT COUNT(*) FROM inbox WHERE session_id=?1 AND source_ref='hook'"
                 " AND payload LIKE '%trip%' AND payload LIKE '%Stop%'"
                 " AND payload LIKE '%h.qjs%'", dsid) == 1);
    /* Disabled: not run, not held, nobody told again. */
    int64_t sid = seed_turn("Trip", 10);
    assert(turn_settle_begin(sid, "Trip") == 0);
    assert(is_state(sid, "idle") && child_count() == 0);
    assert(count(N_TOLD, dsid) == 1);
    /* Promoting the extension again starts it over. */
    install("trip", "Trip", "return '{}';");
    assert(count("SELECT disabled*10 + fail_count FROM hooks WHERE extension_name='trip'"
                 " AND ?1", 1) == 0);
    assert(turn_settle_begin(sid, "Trip") == 1);
    pump_until_idle(sid);
    assert(fail_count("trip") == 0);
    #undef N_TOLD
    free(def);
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--run-tool") == 0)
        return run_tool_main();
    TEST_INIT();
    printf("test_turn_settle:\n");

    fixture_open();

    RUN(test_reviewed);
    RUN(test_outcomes);
    RUN(test_chain);
    RUN(test_not_awaited);
    RUN(test_not_started);
    RUN(test_passed_over);
    RUN(test_declared_input);
    RUN(test_sweep);
    RUN(test_subagent_stop);
    RUN(test_turn_end_order);
    RUN(test_iteration_hold);
    RUN(test_hold_is_a_resource);
    RUN(test_cache_expiring);
    RUN(test_disabled);

    fixture_close();
    printf("All turn_settle tests passed.\n");
    return 0;
}
