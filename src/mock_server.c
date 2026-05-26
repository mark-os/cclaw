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
    char *extra_headers;  /* pre-formatted header lines (e.g. "Retry-After: 2\r\n"), or NULL */
    struct MockResponse *next;
} MockResponse;

static struct mg_context *s_ctx;
static MockResponse *s_queue_head;
static MockResponse *s_queue_tail;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_request_count;
static char *s_last_body;

/* T129: Telegram mock state */
static MockResponse *s_tg_updates_head;
static MockResponse *s_tg_updates_tail;
static MockResponse *s_tg_send_head;
static MockResponse *s_tg_send_tail;
static int s_tg_send_count;
static char *s_tg_last_send_body;

void mock_server_enqueue(int http_status, const char *body) {
    mock_server_enqueue_with_headers(http_status, body, NULL);
}

void mock_server_enqueue_with_headers(int http_status, const char *body, const char **headers) {
    MockResponse *r = calloc(1, sizeof(MockResponse));
    r->http_status = http_status;
    r->body = strdup(body);
    /* Build extra headers string from array */
    if (headers) {
        size_t total = 0;
        for (int i = 0; headers[i]; i++)
            total += strlen(headers[i]) + 2; /* \r\n */
        r->extra_headers = malloc(total + 1);
        r->extra_headers[0] = '\0';
        for (int i = 0; headers[i]; i++) {
            strcat(r->extra_headers, headers[i]);
            strcat(r->extra_headers, "\r\n");
        }
    }
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

    /* Read request body (supports both Content-Length and chunked) */
    char *req_body = NULL;
    if (ri->content_length > 0) {
        size_t content_len = (size_t)ri->content_length;
        req_body = malloc(content_len + 1);
        int nread = mg_read(conn, req_body, content_len);
        req_body[nread > 0 ? nread : 0] = '\0';
    } else {
        /* Chunked or unknown length — read until EOF */
        size_t cap = 4096, len = 0;
        req_body = malloc(cap);
        while (1) {
            if (len + 1024 > cap) { cap *= 2; req_body = realloc(req_body, cap); }
            int n = mg_read(conn, req_body + len, 1024);
            if (n <= 0) break;
            len += (size_t)n;
        }
        req_body[len] = '\0';
        if (len == 0) { free(req_body); req_body = NULL; }
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
    const char *extra_hdrs = NULL;
    char fallback[] = "{\"error\":\"no mock responses queued\"}";
    if (r) {
        status = r->http_status;
        body = r->body;
        extra_hdrs = r->extra_headers;
    } else {
        status = 500;
        body = fallback;
    }

    int len = (int)strlen(body);
    mg_printf(conn,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "%s"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        status, status == 200 ? "OK" : "Error",
        extra_hdrs ? extra_hdrs : "",
        len, body);

    if (r) {
        free(r->body);
        free(r->extra_headers);
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
        free(r->extra_headers);
        free(r);
    }
    s_queue_tail = NULL;
    s_request_count = 0;
    free(s_last_body);
    s_last_body = NULL;

    /* Clean Telegram queues */
    while (s_tg_updates_head) {
        MockResponse *r = s_tg_updates_head;
        s_tg_updates_head = r->next;
        free(r->body);
        free(r);
    }
    s_tg_updates_tail = NULL;
    while (s_tg_send_head) {
        MockResponse *r = s_tg_send_head;
        s_tg_send_head = r->next;
        free(r->body);
        free(r);
    }
    s_tg_send_tail = NULL;
    s_tg_send_count = 0;
    free(s_tg_last_send_body);
    s_tg_last_send_body = NULL;

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

/* ── T129: Telegram mock implementation ────────────────────────── */

void mock_tg_enqueue_updates(const char *body) {
    MockResponse *r = calloc(1, sizeof(MockResponse));
    r->http_status = 200;
    r->body = strdup(body);
    pthread_mutex_lock(&s_mutex);
    if (s_tg_updates_tail) s_tg_updates_tail->next = r;
    else s_tg_updates_head = r;
    s_tg_updates_tail = r;
    pthread_mutex_unlock(&s_mutex);
}

void mock_tg_enqueue_send(const char *body) {
    MockResponse *r = calloc(1, sizeof(MockResponse));
    r->http_status = 200;
    r->body = strdup(body);
    pthread_mutex_lock(&s_mutex);
    if (s_tg_send_tail) s_tg_send_tail->next = r;
    else s_tg_send_head = r;
    s_tg_send_tail = r;
    pthread_mutex_unlock(&s_mutex);
}

int mock_tg_send_count(void) {
    pthread_mutex_lock(&s_mutex);
    int c = s_tg_send_count;
    pthread_mutex_unlock(&s_mutex);
    return c;
}

const char *mock_tg_last_send_body(void) {
    return s_tg_last_send_body;
}

static int handle_tg_get_updates(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    /* Read and discard request body */
    if (ri->content_length > 0) {
        char tmp[4096];
        mg_read(conn, tmp, sizeof(tmp));
    }

    pthread_mutex_lock(&s_mutex);
    MockResponse *r = s_tg_updates_head;
    if (r) {
        s_tg_updates_head = r->next;
        if (!s_tg_updates_head) s_tg_updates_tail = NULL;
    }
    pthread_mutex_unlock(&s_mutex);

    const char *body;
    char empty[] = "{\"ok\":true,\"result\":[]}";
    if (r) {
        body = r->body;
    } else {
        body = empty;
    }

    int len = (int)strlen(body);
    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n\r\n%s", len, body);

    if (r) { free(r->body); free(r); }
    return 200;
}

static int handle_tg_send_message(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    char *req_body = NULL;
    int content_len = (int)(ri->content_length > 0 ? ri->content_length : 0);
    if (content_len > 0) {
        req_body = malloc((size_t)content_len + 1);
        int nread = mg_read(conn, req_body, (size_t)content_len);
        req_body[nread > 0 ? nread : 0] = '\0';
    }

    pthread_mutex_lock(&s_mutex);
    s_tg_send_count++;
    free(s_tg_last_send_body);
    s_tg_last_send_body = req_body;

    MockResponse *r = s_tg_send_head;
    if (r) {
        s_tg_send_head = r->next;
        if (!s_tg_send_head) s_tg_send_tail = NULL;
    }
    pthread_mutex_unlock(&s_mutex);

    const char *body;
    char ok[] = "{\"ok\":true,\"result\":{\"message_id\":1}}";
    if (r) { body = r->body; } else { body = ok; }

    int len = (int)strlen(body);
    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n\r\n%s", len, body);

    if (r) { free(r->body); free(r); }
    return 200;
}

void mock_tg_enable(const char *token) {
    if (!s_ctx) return;
    char route[128];
    snprintf(route, sizeof(route), "/bot%s/getUpdates", token);
    mg_set_request_handler(s_ctx, route, handle_tg_get_updates, NULL);
    snprintf(route, sizeof(route), "/bot%s/sendMessage", token);
    mg_set_request_handler(s_ctx, route, handle_tg_send_message, NULL);
}
