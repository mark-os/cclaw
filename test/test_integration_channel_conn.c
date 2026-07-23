/* Integration test: `cclaw --channel` runner + channel.conn.* WebSocket
 * primitive against a loopback WS mock. Drives the Discord-shaped handshake
 * end to end:
 *   server HELLO (op 10) -> handler IDENTIFY (op 2) + heartbeat (op 1)
 *   -> server heartbeat ACK (op 11) -> server MESSAGE_CREATE (op 0)
 *   -> handler channel.emit -> channel_events row.
 * Proves: conn.open handshake, onConnOpen, bidirectional send/recv, control
 * frames consumed by C, onConnMessage reassembly, onTimer, egress refusal of
 * an off-host conn.open, and clean SIGTERM shutdown.
 *
 * The mock WS server runs in-process (a thread), so IDENTIFY / heartbeat
 * receipt is asserted directly on server-side flags; the inbound path is
 * asserted via the DB. No real network: everything is 127.0.0.1. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>
#include "db.h"
#include "util.h"          /* base64_encode */
#include "test_util.h"

/* The channel.conn.* WS transport needs a libcurl whose runtime protocol set
 * includes ws/wss. Amazon Linux's default `libcurl-minimal` strips it (the
 * curl_ws_* symbols still link, but ws:// perform returns "Unsupported
 * protocol"). Skip cleanly there — the test exercises the real transport as
 * soon as a WS-capable libcurl is present. */
static int curl_has_ws(void) {
    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    if (!v || !v->protocols) return 0;
    for (const char *const *p = v->protocols; *p; p++)
        if (strcasecmp(*p, "ws") == 0 || strcasecmp(*p, "wss") == 0) return 1;
    return 0;
}

#define DB_PATH "/tmp/test_integ_conn.db"
#define EXT_DIR "/tmp/test_integ_conn_ext"
#define JS_PATH EXT_DIR "/channel.qjs"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); goto fail; } while (0)

/* ── Minimal SHA-1 (public domain, for the WS handshake accept key) ── */
typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; size_t n; } Sha1;

static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(Sha1 *s, const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = rol(a,5) + f + e + k + w[i];
        e = d; d = c; c = rol(b,30); b = a; a = t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}

static void sha1_init(Sha1 *s) {
    s->h[0]=0x67452301; s->h[1]=0xEFCDAB89; s->h[2]=0x98BADCFE;
    s->h[3]=0x10325476; s->h[4]=0xC3D2E1F0; s->len=0; s->n=0;
}

static void sha1_update(Sha1 *s, const void *data, size_t len) {
    const unsigned char *p = data;
    s->len += len;
    while (len) {
        size_t take = 64 - s->n; if (take > len) take = len;
        memcpy(s->buf + s->n, p, take); s->n += take; p += take; len -= take;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void sha1_final(Sha1 *s, unsigned char out[20]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    sha1_update(s, &pad, 1);
    unsigned char zero = 0;
    while (s->n != 56) sha1_update(s, &zero, 1);
    unsigned char lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (unsigned char)(bits >> (56 - i*8));
    sha1_update(s, lenbuf, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(s->h[i] >> 24);
        out[i*4+1] = (unsigned char)(s->h[i] >> 16);
        out[i*4+2] = (unsigned char)(s->h[i] >> 8);
        out[i*4+3] = (unsigned char)(s->h[i]);
    }
}

/* ── WS framing helpers (server side) ─────────────────────────────── */

static int read_n(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

/* Send an unmasked text frame (server->client). */
static int ws_send_text(int fd, const char *s) {
    size_t len = strlen(s);
    unsigned char hdr[10];
    size_t hn = 0;
    hdr[hn++] = 0x81;                       /* FIN + text */
    if (len < 126) {
        hdr[hn++] = (unsigned char)len;
    } else if (len < 65536) {
        hdr[hn++] = 126;
        hdr[hn++] = (unsigned char)(len >> 8);
        hdr[hn++] = (unsigned char)(len & 0xff);
    } else {
        hdr[hn++] = 127;
        for (int i = 7; i >= 0; i--) hdr[hn++] = (unsigned char)((uint64_t)len >> (i*8));
    }
    if (write_all(fd, hdr, hn) != 0) return -1;
    return write_all(fd, s, len);
}

/* Read one client frame (masked). Returns opcode, fills payload (NUL-term).
 * -1 on error/close of the socket. */
static int ws_read_frame(int fd, char *payload, size_t cap, size_t *out_len) {
    unsigned char b[2];
    if (read_n(fd, b, 2) != 0) return -1;
    int opcode = b[0] & 0x0f;
    int masked = b[1] & 0x80;
    uint64_t len = b[1] & 0x7f;
    if (len == 126) {
        unsigned char e[2]; if (read_n(fd, e, 2) != 0) return -1;
        len = (uint64_t)e[0] << 8 | e[1];
    } else if (len == 127) {
        unsigned char e[8]; if (read_n(fd, e, 8) != 0) return -1;
        len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    unsigned char mask[4] = {0};
    if (masked && read_n(fd, mask, 4) != 0) return -1;
    if (len >= cap) return -1;              /* test frames are small */
    if (len && read_n(fd, payload, (size_t)len) != 0) return -1;
    if (masked) for (uint64_t i = 0; i < len; i++)
        payload[i] = (char)((unsigned char)payload[i] ^ mask[i % 4]);
    payload[len] = '\0';
    if (out_len) *out_len = (size_t)len;
    return opcode;
}

/* ── Mock server state (asserted directly by main) ────────────────── */
static int g_listen_fd = -1;
static volatile int g_identify_seen = 0;
static volatile int g_heartbeat_seen = 0;
static volatile int g_handshaked = 0;

static int extract_ws_key(const char *req, char *out, size_t cap) {
    const char *h = strcasestr(req, "Sec-WebSocket-Key:");
    if (!h) return -1;
    h += strlen("Sec-WebSocket-Key:");
    while (*h == ' ' || *h == '\t') h++;
    size_t n = 0;
    while (*h && *h != '\r' && *h != '\n' && n < cap - 1) out[n++] = *h++;
    out[n] = '\0';
    return 0;
}

static void *mock_ws_server(void *arg) {
    (void)arg;
    int cfd = accept(g_listen_fd, NULL, NULL);
    if (cfd < 0) return NULL;
    struct timeval tv = {10, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Read the HTTP upgrade request (until CRLFCRLF). */
    char req[2048]; size_t rn = 0;
    while (rn < sizeof(req) - 1) {
        ssize_t r = read(cfd, req + rn, sizeof(req) - 1 - rn);
        if (r <= 0) { close(cfd); return NULL; }
        rn += (size_t)r; req[rn] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    char key[128];
    if (extract_ws_key(req, key, sizeof(key)) != 0) { close(cfd); return NULL; }

    /* accept = base64(sha1(key + magic-GUID)) */
    char concat[256];
    snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    Sha1 s; sha1_init(&s); sha1_update(&s, concat, strlen(concat));
    unsigned char digest[20]; sha1_final(&s, digest);
    char *accept = base64_encode(digest, 20);
    if (!accept) { close(cfd); return NULL; }

    char resp[256];
    int rl = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    free(accept);
    if (write_all(cfd, resp, (size_t)rl) != 0) { close(cfd); return NULL; }
    g_handshaked = 1;

    /* HELLO */
    ws_send_text(cfd, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");

    /* Read client frames; drive IDENTIFY -> heartbeat -> ACK -> MESSAGE_CREATE. */
    int sent_message = 0;
    for (;;) {
        char pl[4096]; size_t len = 0;
        int op = ws_read_frame(cfd, pl, sizeof(pl), &len);
        if (op < 0) break;
        if (op == 0x8) break;               /* client close */
        if (op != 0x1) continue;            /* only text matters here */
        /* Gateway op lives in the JSON: crude but sufficient for the fixture. */
        if (strstr(pl, "\"op\":2")) g_identify_seen = 1;
        if (strstr(pl, "\"op\":1")) {
            g_heartbeat_seen = 1;
            ws_send_text(cfd, "{\"op\":11}");           /* heartbeat ACK */
        }
        if (g_identify_seen && g_heartbeat_seen && !sent_message) {
            sent_message = 1;
            ws_send_text(cfd, "{\"op\":0,\"t\":\"MESSAGE_CREATE\","
                              "\"d\":{\"channel_id\":\"999\",\"content\":\"hi from gateway\"}}");
        }
    }
    close(cfd);
    return NULL;
}

/* ── Test channel JS: a stripped Discord-shaped gateway handler ────── */
static void write_test_js(void) {
    FILE *f = fopen(JS_PATH, "w");
    fprintf(f,
        "var CID = 0;\n"
        "function onInit() {\n"
        "  CID = channel.conn.open({url: channel.getConfig('gw_url')});\n"
        "  channel.setState('opened', '' + CID);\n"
        "  try {\n"
        "    channel.conn.open({url: 'wss://evil.invalid/gw'});\n"   /* off-host: must throw */
        "    channel.setState('egress', 'ALLOWED');\n"
        "  } catch (e) { channel.setState('egress', 'BLOCKED'); }\n"
        "  return {};\n"
        "}\n"
        "function onConnOpen(id) { channel.setState('onopen', '' + id); }\n"
        "function onConnMessage(id, text) {\n"
        "  var m = JSON.parse(text);\n"
        "  if (m.op === 10) {\n"
        "    channel.conn.send(id, JSON.stringify({op: 2, d: {token: 'x', intents: 33280}}));\n"
        "    channel.conn.send(id, JSON.stringify({op: 1, d: null}));\n"
        "  } else if (m.op === 11) {\n"
        "    channel.setState('acked', '1');\n"
        "  } else if (m.op === 0 && m.t === 'MESSAGE_CREATE') {\n"
        "    channel.emit('message', JSON.stringify({chat_id: '' + m.d.channel_id, text: m.d.content}));\n"
        "  }\n"
        "}\n"
        "function onConnClose(id, code) { channel.setState('closed', '' + code); }\n"
        "function onTimer() { channel.setState('ticked', '1'); }\n");
    fclose(f);
}

static void seed_db(int port) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    char base_url[64], gw_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    snprintf(gw_url, sizeof(gw_url), "ws://127.0.0.1:%d/", port);

    sqlite3_exec(db, "INSERT OR REPLACE INTO extensions(name,path)"
        " VALUES('discx','" EXT_DIR "');", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO channels(name,extension_name)"
        " VALUES('discx','discx');", NULL, NULL, NULL);

    const char *sql = "INSERT OR REPLACE INTO config(key,value,description) VALUES(?,?,'test');";
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, sql, -1, &s, NULL);
    sqlite3_bind_text(s, 1, "discx.base_url", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, base_url, -1, SQLITE_TRANSIENT);
    sqlite3_step(s); sqlite3_reset(s);
    sqlite3_bind_text(s, 1, "discx.gw_url", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, gw_url, -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
    db_close(db);
}

static char *state_value(sqlite3 *db, const char *key) {
    sqlite3_stmt *stmt;
    char *val = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM channel_state WHERE channel_name='discx' AND key=?;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (v) val = strdup(v);
        }
        sqlite3_finalize(stmt);
    }
    return val;
}

/* Poll up to ~6s for channel_state key == want. */
static int wait_state(sqlite3 *db, const char *key, const char *want) {
    for (int i = 0; i < 60; i++) {
        usleep(100000);
        char *v = state_value(db, key);
        int hit = v && strcmp(v, want) == 0;
        free(v);
        if (hit) return 1;
    }
    return 0;
}

int main(void) {
    TEST_INIT();
    printf("test_integration_channel_conn:\n");

    if (!curl_has_ws()) {
        printf("  SKIP: runtime libcurl has no ws/wss protocol "
               "(libcurl-minimal?) — conn.* transport untestable here\n");
        return 0;
    }

    (void)system("rm -f /tmp/test_integ_conn.db* /tmp/test_integ_conn.discx.*");

    /* Loopback listener on an OS-assigned port. */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { fprintf(stderr, "FAIL: socket\n"); return 1; }
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { fprintf(stderr, "FAIL: bind\n"); return 1; }
    if (listen(g_listen_fd, 4) != 0) { fprintf(stderr, "FAIL: listen\n"); return 1; }
    socklen_t sl = sizeof(sa);
    getsockname(g_listen_fd, (struct sockaddr *)&sa, &sl);
    int port = ntohs(sa.sin_port);
    struct timeval atv = {10, 0};
    setsockopt(g_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &atv, sizeof(atv));

    pthread_t th;
    if (pthread_create(&th, NULL, mock_ws_server, NULL) != 0) { fprintf(stderr, "FAIL: thread\n"); return 1; }

    mkdir(EXT_DIR, 0755);
    write_test_js();
    seed_db(port);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        execl("./build/cclaw", "cclaw", "--channel", "discx", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { fprintf(stderr, "FAIL: fork\n"); return 1; }

    sqlite3 *db = test_db_open(DB_PATH);
    int rc = 1;

    /* 1. onConnOpen fired with the id conn.open returned. */
    if (!wait_state(db, "onopen", "1")) FAIL("onConnOpen(1) did not fire");
    printf("  PASS: conn.open handshake -> onConnOpen\n");

    /* 2. Egress: off-host conn.open threw (refused before any connect). */
    if (!wait_state(db, "egress", "BLOCKED")) FAIL("off-host conn.open was not refused");
    printf("  PASS: egress pin refuses off-host conn.open\n");

    /* 3. Inbound MESSAGE_CREATE -> channel.emit -> channel_events. */
    int found = 0;
    for (int i = 0; i < 60 && !found; i++) {
        usleep(100000);
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT payload FROM channel_events WHERE channel_name='discx' LIMIT 1;",
                -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(st, 0);
                if (p && strstr(p, "hi from gateway")) found = 1;
            }
            sqlite3_finalize(st);
        }
    }
    if (!found) FAIL("MESSAGE_CREATE did not reach channel_events");
    printf("  PASS: HELLO->IDENTIFY->ACK->MESSAGE_CREATE->emit\n");

    /* 4. Bidirectional proof: server saw IDENTIFY + heartbeat; handler saw ACK. */
    if (!g_identify_seen) FAIL("server never received IDENTIFY");
    if (!g_heartbeat_seen) FAIL("server never received heartbeat");
    if (!wait_state(db, "acked", "1")) FAIL("handler never received heartbeat ACK");
    printf("  PASS: bidirectional send/recv + control frames\n");

    /* 5. onTimer ticked. */
    if (!wait_state(db, "ticked", "1")) FAIL("onTimer never fired");
    printf("  PASS: onTimer tick\n");

    rc = 0;
fail:
    if (rc != 0 && db) {
        fprintf(stderr, "  -- diagnostics --\n");
        fprintf(stderr, "  handshaked=%d identify=%d heartbeat=%d\n",
                g_handshaked, g_identify_seen, g_heartbeat_seen);
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT key,value FROM channel_state WHERE channel_name='discx';",
                               -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                fprintf(stderr, "  state[%s] = %s\n",
                        sqlite3_column_text(st, 0), sqlite3_column_text(st, 1));
            sqlite3_finalize(st);
        }
    }
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    if (db) db_close(db);
    close(g_listen_fd);
    pthread_join(th, NULL);

    if (rc == 0) {
        if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
            if (WIFSIGNALED(wstatus))
                fprintf(stderr, "  runner killed by signal %d\n", WTERMSIG(wstatus));
            else
                fprintf(stderr, "  runner exited with status %d\n", WEXITSTATUS(wstatus));
            rc = 1;
        } else {
            printf("  PASS: clean shutdown after SIGTERM (exit 0)\n");
        }
    }

    test_db_clean(DB_PATH);
    unlink(JS_PATH);
    rmdir(EXT_DIR);
    if (rc == 0) printf("all channel_conn integration tests passed\n");
    return rc;
}
