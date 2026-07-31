#ifndef CCLAW_CHANNEL_RUNNER_H
#define CCLAW_CHANNEL_RUNNER_H

#include "channel_api.h"
#include "qjs_helpers.h"
#include <stdint.h>

/* ── Heap / message-size policy ───────────────────────────────────
 * These two live together because they are one decision, not two, and they
 * live in the header because the relationship between them is asserted below
 * and exercised by test_channel_runner_limits.
 *
 * CR_HEAP_SIZE is a ceiling, not a reservation: QuickJS allocates on demand,
 * so raising it costs a runaway handler's worst case, not steady-state RSS on
 * a 128MB box (an idle channel context is well under 1MB).
 *
 * CR_CONN_MSG_MAX caps a *reassembled* WebSocket message. Anything larger is
 * refused at receipt (conn_recv_drain drops it and drains the rest without
 * buffering) — the cap exists because the WS length field is 64-bit and
 * libcurl imposes no message ceiling, so an allowlisted-but-hostile peer could
 * otherwise stream us out of RAM.
 *
 * The cap must sit well BELOW the heap, not merely below it, because a message
 * is charged to the JS heap more than once:
 *   - JS_NewString copies it in, and QuickJS widens a string to UTF-16 as soon
 *     as one character falls outside Latin-1 — an N-byte message can cost 2N;
 *   - the handler then JSON.parses it, building an object graph that is again
 *     a multiple of the source text.
 * A cap above that budget produces the worst failure mode there is: a message
 * that passes the transport check and then cannot be materialized, reaching
 * the handler as `undefined`. Hence the 4x floor (8x as configured). */
#define CR_HEAP_SIZE (8 * 1024 * 1024)
#define CR_CONN_MSG_MAX (1024L * 1024L)

_Static_assert(CR_CONN_MSG_MAX > 0 && CR_CONN_MSG_MAX * 4 <= CR_HEAP_SIZE,
               "WS message cap must leave the JS heap room for the message "
               "widened to UTF-16 plus its parsed form");

/* Shared with channel_harness.c (the --harness fixture-replay path): the
 * queue cclaw.send() fills and the live runner's curl loop drains. The
 * harness drains it too, but matches fixtures instead of hitting real
 * network. */
typedef struct SendReq {
    char *method;
    char *url;
    char *body;
    char **headers;
    int n_headers;
    int64_t outbox_id;   /* 0 = not outbox-bound */
    int is_final;        /* last send for this outbox row */
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
void call_on_outbox(JSContext *ctx, ChannelOutboxRow *row);

/* Bind a global for the next handler eval. Return 0, or -1 if the value could
 * not be created or stored — JS_NewString answers with an OOM exception, not a
 * value, when the string does not fit CR_HEAP_SIZE. Callers MUST drop the event
 * on -1 rather than dispatch: the property is then either missing (the handler
 * sees `undefined`) or still holding the *previous* event's value, and neither
 * is distinguishable from a real message inside JS. The pending exception is
 * consumed and logged here. */
int cr_set_global_str(JSContext *ctx, const char *name, const char *val);
int cr_set_global_int(JSContext *ctx, const char *name, int val);

/* Settle a promise-backed request (channel.http): resolve with
 * {status, body, path, bytes, error} and drain the microtask queue so
 * awaiting continuations run to their next suspension point. body and path
 * are mutually exclusive (path = save_to download). Never rejects — transport
 * failures resolve with status 0 + error, so channel JS needs no try/catch
 * around plain calls. */
void send_req_settle(JSContext *ctx, SendReq *r, int status, const char *body,
                     const char *path, long bytes, const char *error);

/* Persistent-connection primitive (channel.conn.* host functions). C owns the
 * transport — WebSocket via libcurl CONNECT_ONLY; the .qjs handler owns the
 * protocol. The CONNECT_ONLY easy handles are driven MANUALLY from the runner
 * event loop (their CURLINFO_ACTIVESOCKET fd is polled in extra[]), never added
 * to curl_multi. See specs/channel-transports.md. Called from qjs_host_channel.c.
 *   open: framing defaults to "ws" ("raw" is planned, not yet implemented);
 *         synchronous handshake; returns id >= 1 or -1.
 *   send: queues a text frame (flushed opportunistically / on the tick);
 *         returns 0 if id unknown or closed.
 *   close: graceful close with an optional WebSocket status code (<= 0 sends a
 *          payload-less CLOSE); onConnClose fires from the loop, not
 *          re-entrantly. */
int cr_conn_open(const char *url, const char *framing,
                 char **headers, int n_headers, long timeout);
int cr_conn_send(int id, const char *text);
void cr_conn_close(int id, int code);

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
