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
    int base64;          /* deliver the response body base64-encoded (binary
                            downloads — a raw body would truncate at the first
                            NUL crossing the C-string JS bridge) */
    long timeout;
    /* channel.http(): promise-backed request. js_ctx non-NULL marks it; the
       resolve/reject pair is owned by the request and freed in send_req_free.
       save_to (absolute, pre-validated) streams the response body to a file —
       the payload never enters the JS heap; JS gets {status, path, bytes}. */
    JSContext *js_ctx;
    JSValue p_resolve, p_reject;
    char *save_to;
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

/* Settle a promise-backed request (channel.http): resolve with
 * {status, body, path, bytes, error} and drain the microtask queue so
 * awaiting continuations run to their next suspension point. body and path
 * are mutually exclusive (path = save_to download). Never rejects — transport
 * failures resolve with status 0 + error, so channel JS needs no try/catch
 * around plain calls. */
void send_req_settle(JSContext *ctx, SendReq *r, int status, const char *body,
                     const char *path, long bytes, const char *error);

/* Resolve a save_to name to an absolute path in this channel's media spool
 * (<db_dir>/media/<channel>/<name>), creating the directory. Rejects names
 * with '/' or leading '.'. Returns malloc'd path or NULL. */
char *channel_save_path(const char *name);

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
