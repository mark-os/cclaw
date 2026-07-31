/* Channel-runner heap policy, and the global-binding failure path.
 *
 * Two halves of one defect. The reassembled-WebSocket-message cap used to sit
 * ABOVE the runner's JS heap (3MB vs 2MB), and the code that hands a message
 * to JS never checked whether the string had actually been created — so a
 * ~2.2MB frame passed the transport check, failed to materialize, and reached
 * the handler as `undefined` with nothing in the log. Either half alone is
 * survivable; together they are a silent data loss.
 *
 * The live WebSocket path needs a real gateway (test_integration_channel_conn
 * covers it); what is testable here is the seam underneath it — the binding
 * primitive's failure report, and the constant relationship that keeps the
 * failure unreachable in the first place. */
#define _POSIX_C_SOURCE 200809L
#include "channel_runner.h"
#include "qjs_helpers.h"
#include "approval.h"
#include "resolve.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* channel_runner.o pulls in channel.o, which references resolve_approval —
 * defined in main.c, which isn't in libcclaw.a. Same stub as the other
 * channel tests. */
void resolve_approval(int64_t approval_id, ApprovalDecision decision,
                      const char *decided_via, int64_t grant_expires_at) {
    (void)approval_id; (void)decision; (void)decided_via; (void)grant_expires_at;
}

/* The transport refuses anything over CR_CONN_MSG_MAX at receipt, so that is
 * the largest message JS is ever asked to hold — and it has to hold it more
 * than once: QuickJS widens a string to UTF-16 the moment one character falls
 * outside Latin-1 (up to 2x the wire bytes), and the handler then JSON.parses
 * it into an object graph that is a further multiple of the text. A cap at or
 * above the heap reopens the "passes the check, then cannot be materialized"
 * window. Also asserted at compile time in channel_runner.h; kept here so the
 * policy has a named, runnable owner. */
static void test_cap_fits_heap(void) {
    assert(CR_CONN_MSG_MAX > 0);
    assert((long)CR_CONN_MSG_MAX * 4 <= (long)CR_HEAP_SIZE);
    printf("  PASS test_cap_fits_heap (cap=%ld bytes, heap=%d bytes)\n",
           (long)CR_CONN_MSG_MAX, CR_HEAP_SIZE);
}

static void test_set_global_reports_oom(void) {
    QjsRuntime *qrt = qjs_runtime_create(CR_HEAP_SIZE);
    assert(qrt);
    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_CHANNEL);
    assert(ctx);

    /* Normal binding works and is readable back. */
    assert(cr_set_global_int(ctx, "__cr_conn_id", 7) == 0);
    assert(cr_set_global_str(ctx, "__cr_conn_msg", "{\"op\":11}") == 0);
    char *v = qjs_eval_to_string(ctx, "__cr_conn_msg", "<probe>");
    assert(v && strcmp(v, "{\"op\":11}") == 0);
    free(v);
    v = qjs_eval_to_string(ctx, "'' + __cr_conn_id", "<probe>");
    assert(v && strcmp(v, "7") == 0);
    free(v);

    /* A string larger than the whole heap cannot be created. The old code
     * ignored that and dispatched the handler anyway. */
    size_t big = (size_t)CR_HEAP_SIZE + 1024 * 1024;
    char *s = malloc(big + 1);
    assert(s);
    memset(s, 'x', big);
    s[big] = '\0';
    assert(cr_set_global_str(ctx, "__cr_conn_msg", s) == -1);
    free(s);

    /* And the global still holds the PREVIOUS message — which is exactly why
     * a failed bind must drop the event instead of dispatching over it: inside
     * JS a stale payload is indistinguishable from a real re-delivery. */
    v = qjs_eval_to_string(ctx, "__cr_conn_msg", "<probe>");
    assert(v && strcmp(v, "{\"op\":11}") == 0);
    free(v);

    /* Reported, not fatal: the runtime keeps working for the next event. */
    assert(cr_set_global_str(ctx, "__cr_conn_msg", "{\"op\":0}") == 0);
    v = qjs_eval_to_string(ctx, "__cr_conn_msg", "<probe>");
    assert(v && strcmp(v, "{\"op\":0}") == 0);
    free(v);

    /* NULL binds as JS null (the "no body" shape the poll contract uses),
     * never as a missing property. */
    assert(cr_set_global_str(ctx, "__cr_body", NULL) == 0);
    v = qjs_eval_to_string(ctx, "__cr_body === null ? 'null' : 'other'", "<probe>");
    assert(v && strcmp(v, "null") == 0);
    free(v);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
    printf("  PASS test_set_global_reports_oom\n");
}

/* A message right at the cap must still fit — the point of the 4x floor is
 * that the largest message the transport accepts is comfortably bindable. */
static void test_cap_sized_message_binds(void) {
    QjsRuntime *qrt = qjs_runtime_create(CR_HEAP_SIZE);
    assert(qrt);
    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_CHANNEL);
    assert(ctx);

    size_t n = (size_t)CR_CONN_MSG_MAX;
    char *s = malloc(n + 1);
    assert(s);
    memset(s, 'y', n);
    s[n] = '\0';
    assert(cr_set_global_str(ctx, "__cr_conn_msg", s) == 0);
    free(s);
    char *v = qjs_eval_to_string(ctx, "'' + __cr_conn_msg.length", "<probe>");
    assert(v && (size_t)atol(v) == n);
    free(v);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
    printf("  PASS test_cap_sized_message_binds\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_channel_runner_limits:\n");
    test_cap_fits_heap();
    test_set_global_reports_oom();
    test_cap_sized_message_binds();
    printf("all channel_runner limit tests passed\n");
    return 0;
}
