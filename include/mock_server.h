#ifndef CCLAW_MOCK_SERVER_H
#define CCLAW_MOCK_SERVER_H

/* Mock LLM server for integration tests.
 * Uses civetweb on port 0 (OS-assigned) in-process.
 * Serves /v1/chat/completions with configurable canned responses. */

/* Queue a canned response (FIFO). Each call to the endpoint pops one.
 * If queue empty, returns 500. http_status is the HTTP code to return.
 * body is the raw JSON response body (caller owns, copied internally). */
void mock_server_enqueue(int http_status, const char *body);

/* Start mock server. Returns the assigned port, or -1 on failure. */
int mock_server_start(void);

/* Stop mock server and free all queued responses. */
void mock_server_stop(void);

/* Return number of requests received since start (or last reset). */
int mock_server_request_count(void);

/* Return the body of the last request received (NULL if none). Caller must NOT free. */
const char *mock_server_last_request_body(void);

#endif
