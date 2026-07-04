#ifndef CCLAW_CHANNEL_RUNNER_H
#define CCLAW_CHANNEL_RUNNER_H

#include "channel_api.h"
#include "qjs_helpers.h"
#include <stdint.h>

/* Shared with channel_harness.c (the --harness fixture-replay path): the
 * queue cclaw.send() fills and the live runner's curl loop drains. The
 * harness drains it too, but matches fixtures instead of hitting real
 * network. */
typedef struct SendReq {
    char *method;
    char *url;
    char *body;
    char *tag;
    char **headers;
    int n_headers;
    int64_t outbox_id;   /* 0 = not outbox-bound */
    int is_final;        /* last send for this outbox row */
    long timeout;
    struct SendReq *next;
} SendReq;

void send_queue_push(SendReq *r);
SendReq *send_queue_pop(void);
void send_req_free(SendReq *r);

/* JS-eval + callback-invocation helpers shared with the harness. */
JSValue eval_js(JSContext *ctx, const char *code, const char *tag);
void set_global_str(JSContext *ctx, const char *name, const char *val);
void set_global_int(JSContext *ctx, const char *name, int val);
void call_on_outbox(JSContext *ctx, ChannelOutboxRow *row);
void call_on_result(JSContext *ctx, const char *tag, int status,
                    const char *body, const char *error);

/* Run the universal JS channel loop in-process — this is the `cclaw --channel
 * <name>` mode. The daemon fork+execs `cclaw --channel <name>` (do_fork in
 * channel.c) to get a clean process image, then main() calls this directly; the
 * process shows as `cclaw --channel <name>` in ps. There is no separate runner
 * binary. Opens its own DB ctx from db_path, loads the channel's extension JS,
 * and runs the poll / outbox / request event loop until SIGTERM. Returns the
 * process exit code. */
int channel_runner_main(const char *db_path, const char *channel_name);

/* `cclaw --channel <name> --check`: static validation gate — manifest check +
 * JS load + onInit(), no event loop, nothing queued during onInit actually
 * goes out. Returns 0 on pass, -1 on fail (and sets *err_out if given; caller
 * frees). Does not touch channels.status — the caller transitions on 0. */
int channel_runner_check(const char *db_path, const char *channel_name, char **err_out);

/* Resolve a channel's channel.qjs path from db (joins channels -> extensions).
 * Shared by the live runner, --check, and --harness. Returns 0 and fills out,
 * or -1 if the channel/extension isn't registered. */
int resolve_js_path(sqlite3 *db, const char *channel_name, char *out, size_t outlen);

#endif
