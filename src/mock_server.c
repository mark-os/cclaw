#define _DEFAULT_SOURCE
#include "mock_server.h"
#include "civetweb.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* FIFO queue of canned responses */
typedef struct MockResponse {
    int http_status;
    char *body;
    struct MockResponse *next;
} MockResponse;

static struct mg_context *s_ctx;
static MockResponse *s_queue_head;
static MockResponse *s_queue_tail;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_request_count;
static char *s_last_body;

void mock_server_enqueue(int http_status, const char *body) {
    MockResponse *r = calloc(1, sizeof(MockResponse));
    r->http_status = http_status;
    r->body = strdup(body);
    pthread_mutex_lock(&s_mutex);
    if (s_queue_tail) {
        s_queue_tail->next = r;
    } else {
        s_queue_head = r;
    }
    s_queue_tail = r;
    pthread_mutex_unlock(&s_mutex);
}

static int handle_completions(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    /* Read request body */
    int content_len = (int)(ri->content_length > 0 ? ri->content_length : 0);
    char *req_body = NULL;
    if (content_len > 0) {
        req_body = malloc((size_t)content_len + 1);
        int nread = mg_read(conn, req_body, (size_t)content_len);
        req_body[nread > 0 ? nread : 0] = '\0';
    }

    pthread_mutex_lock(&s_mutex);
    s_request_count++;
    free(s_last_body);
    s_last_body = req_body; /* take ownership */

    /* Pop next canned response */
    MockResponse *r = s_queue_head;
    if (r) {
        s_queue_head = r->next;
        if (!s_queue_head) s_queue_tail = NULL;
    }
    pthread_mutex_unlock(&s_mutex);

    int status;
    const char *body;
    char fallback[] = "{\"error\":\"no mock responses queued\"}";
    if (r) {
        status = r->http_status;
        body = r->body;
    } else {
        status = 500;
        body = fallback;
    }

    int len = (int)strlen(body);
    mg_printf(conn,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        status, status == 200 ? "OK" : "Error",
        len, body);

    if (r) {
        free(r->body);
        free(r);
    }
    return status;
}

int mock_server_start(void) {
    mg_init_library(0);

    const char *options[] = {
        "listening_ports", "0",
        "num_threads", "2",
        NULL
    };

    s_ctx = mg_start(NULL, NULL, options);
    if (!s_ctx) return -1;

    mg_set_request_handler(s_ctx, "/v1/chat/completions", handle_completions, NULL);

    /* Get assigned port */
    struct mg_server_port ports[1];
    int n = mg_get_server_ports(s_ctx, 1, ports);
    if (n < 1) {
        mg_stop(s_ctx);
        s_ctx = NULL;
        return -1;
    }
    return ports[0].port;
}

void mock_server_stop(void) {
    if (s_ctx) {
        mg_stop(s_ctx);
        s_ctx = NULL;
    }
    mg_exit_library();

    pthread_mutex_lock(&s_mutex);
    while (s_queue_head) {
        MockResponse *r = s_queue_head;
        s_queue_head = r->next;
        free(r->body);
        free(r);
    }
    s_queue_tail = NULL;
    s_request_count = 0;
    free(s_last_body);
    s_last_body = NULL;
    pthread_mutex_unlock(&s_mutex);
}

int mock_server_request_count(void) {
    pthread_mutex_lock(&s_mutex);
    int c = s_request_count;
    pthread_mutex_unlock(&s_mutex);
    return c;
}

const char *mock_server_last_request_body(void) {
    return s_last_body;
}
