#ifndef CCLAW_MOCK_SERVER_H
#define CCLAW_MOCK_SERVER_H

/* Mock LLM server for integration tests.
 * Uses civetweb on port 0 (OS-assigned) in-process.
 * Serves /v1/chat/completions with configurable canned responses. */

/* Queue a canned response (FIFO). Each call to the endpoint pops one.
 * If queue empty, returns 500. http_status is the HTTP code to return.
 * body is the raw JSON response body (caller owns, copied internally). */
void mock_server_enqueue(int http_status, const char *body);

/* Queue a canned response with extra headers (NULL-terminated "Key: Value" array, or NULL). */
void mock_server_enqueue_with_headers(int http_status, const char *body, const char **headers);

/* Start mock server. Returns the assigned port, or -1 on failure. */
int mock_server_start(void);

/* Load responses from a JSON template file. Format: array of objects:
 * [{"status": 200, "body": {...}}, {"status": 429, "body": {...}}]
 * Each object is enqueued in order. "body" is re-serialized as JSON string.
 * Returns number of responses loaded, or -1 on error. */
int mock_server_load(const char *path);

/* Stop mock server and free all queued responses. */
void mock_server_stop(void);

/* Reset state without stopping server — clear queue, zero request count. */
void mock_server_reset(void);

/* Return number of requests received since start (or last reset). */
int mock_server_request_count(void);

/* Return the body of the last request received (NULL if none). Caller must NOT free. */
const char *mock_server_last_request_body(void);

/* ── Telegram mock endpoints ────────────────────────────────────── */

/* Queue a canned response for Telegram getUpdates (FIFO, separate from LLM queue). */
void mock_tg_enqueue_updates(const char *body);

/* Queue a canned response for Telegram sendMessage. The HTTP status is
 * honored (a 400 drives the runner's format-degradation re-delivery). */
void mock_tg_enqueue_send(int http_status, const char *body);

/* Return number of sendMessage calls received. */
int mock_tg_send_count(void);

/* Return body of last sendMessage request (NULL if none). Caller must NOT free. */
const char *mock_tg_last_send_body(void);

/* Enable Telegram route handlers (call after mock_server_start).
 * token is the bot token used in URL matching (e.g. "test-token"). */
void mock_tg_enable(const char *token);

#endif
