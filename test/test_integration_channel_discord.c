/* Integration test: the real templates/channel_discord.qjs Discord Gateway
 * handler, driven against a loopback WS mock speaking the gateway protocol.
 * Unlike test_integration_channel_conn (which uses a stripped inline handler to
 * prove the transport), this loads the SHIPPED handler and exercises its
 * protocol + policy logic:
 *   HELLO -> handler IDENTIFY (op 2) + heartbeat (op 1) -> ACK (op 11)
 *   -> READY (learn bot id) -> three MESSAGE_CREATEs:
 *       guild, no mention   -> DROPPED by the require_mention gate
 *       DM                  -> emitted (DMs bypass the gate)
 *       guild, @mention     -> emitted
 * Proves: IDENTIFY, heartbeat liveness, mention gating, DM bypass, and that the
 * envelope reaches channel_events. No real network: everything is 127.0.0.1.
 * Skips cleanly where libcurl lacks ws/wss (libcurl-minimal). */
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
#include "util.h"          /* base64_encode, curl_ws_available */
#include "test_util.h"

#define DB_PATH  "/tmp/test_integ_discord.db"
#define EXT_DIR  "/tmp/test_integ_discord_ext"
#define JS_PATH  EXT_DIR "/channel.qjs"
#define BOT_ID   "999000"
#define FAIL(m)  do { fprintf(stderr, "FAIL: %s\n", m); goto fail; } while (0)

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
    if (len >= cap) return -1;
    if (len && read_n(fd, payload, (size_t)len) != 0) return -1;
    if (masked) for (uint64_t i = 0; i < len; i++)
        payload[i] = (char)((unsigned char)payload[i] ^ mask[i % 4]);
    payload[len] = '\0';
    if (out_len) *out_len = (size_t)len;
    return opcode;
}

/* ── Mock gateway state ────────────────────────────────────────────── */
static int g_listen_fd = -1;
static volatile int g_identify_seen = 0;
static volatile int g_heartbeat_seen = 0;

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

static void *mock_gateway(void *arg) {
    (void)arg;
    int cfd = accept(g_listen_fd, NULL, NULL);
    if (cfd < 0) return NULL;
    struct timeval tv = {10, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[2048]; size_t rn = 0;
    while (rn < sizeof(req) - 1) {
        ssize_t r = read(cfd, req + rn, sizeof(req) - 1 - rn);
        if (r <= 0) { close(cfd); return NULL; }
        rn += (size_t)r; req[rn] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    char key[128];
    if (extract_ws_key(req, key, sizeof(key)) != 0) { close(cfd); return NULL; }
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

    /* Short heartbeat interval so the timer fires within the test window. */
    ws_send_text(cfd, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}");

    int dispatched = 0;
    for (;;) {
        char pl[4096]; size_t len = 0;
        int op = ws_read_frame(cfd, pl, sizeof(pl), &len);
        if (op < 0) break;
        if (op == 0x8) break;               /* client close */
        if (op != 0x1) continue;            /* only text */
        if (strstr(pl, "\"op\":2")) g_identify_seen = 1;
        if (strstr(pl, "\"op\":1")) {
            g_heartbeat_seen = 1;
            ws_send_text(cfd, "{\"op\":11}");           /* heartbeat ACK */
        }
        /* Once IDENTIFY lands: READY (teaches the handler its bot id), then
         * three messages exercising the mention gate. */
        if (g_identify_seen && !dispatched) {
            dispatched = 1;
            ws_send_text(cfd,
                "{\"op\":0,\"s\":1,\"t\":\"READY\",\"d\":{\"session_id\":\"sess1\","
                "\"resume_gateway_url\":\"ws://127.0.0.1\",\"user\":{\"id\":\"" BOT_ID
                "\",\"username\":\"cclawbot\"}}}");
            /* guild, no mention -> dropped by require_mention */
            ws_send_text(cfd,
                "{\"op\":0,\"s\":2,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m1\","
                "\"channel_id\":\"chan1\",\"guild_id\":\"g1\",\"content\":\"hello world\","
                "\"author\":{\"id\":\"u1\",\"username\":\"alice\"},\"mentions\":[]}}");
            /* DM -> emitted (bypasses the gate) */
            ws_send_text(cfd,
                "{\"op\":0,\"s\":3,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m2\","
                "\"channel_id\":\"dm1\",\"content\":\"hi in dm\","
                "\"author\":{\"id\":\"u1\",\"username\":\"alice\"},\"mentions\":[]}}");
            /* guild, @mention -> emitted */
            ws_send_text(cfd,
                "{\"op\":0,\"s\":4,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m3\","
                "\"channel_id\":\"chan1\",\"guild_id\":\"g1\",\"content\":\"<@" BOT_ID
                "> help me\",\"author\":{\"id\":\"u1\",\"username\":\"alice\"},"
                "\"mentions\":[{\"id\":\"" BOT_ID "\"}]}}");
        }
    }
    close(cfd);
    return NULL;
}

static void seed_db(int port) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    char base_url[64], gw_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    snprintf(gw_url, sizeof(gw_url), "ws://127.0.0.1:%d/", port);

    sqlite3_exec(db, "INSERT OR REPLACE INTO extensions(name,path)"
        " VALUES('discord','" EXT_DIR "');", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO channels(name,extension_name)"
        " VALUES('discord','discord');", NULL, NULL, NULL);

    const char *sql = "INSERT OR REPLACE INTO config(key,value,description) VALUES(?,?,'test');";
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, sql, -1, &s, NULL);
    struct { const char *k, *v; } kv[] = {
        { "discord.bot_token",      "test-token" },
        { "discord.base_url",       base_url },
        { "discord.gateway_url",    gw_url },
        { "discord.require_mention","1" },
        { "discord.admin_ids",      "" },
    };
    for (size_t i = 0; i < sizeof(kv)/sizeof(*kv); i++) {
        sqlite3_bind_text(s, 1, kv[i].k, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, kv[i].v, -1, SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    db_close(db);
}

/* Count channel_events whose payload contains `needle`. */
static int event_count(sqlite3 *db, const char *needle) {
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM channel_events WHERE channel_name='discord'"
            " AND payload LIKE '%' || ?1 || '%';", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, needle, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

int main(void) {
    TEST_INIT();
    printf("test_integration_channel_discord:\n");

    if (!curl_ws_available()) {
        printf("  SKIP: runtime libcurl cannot carry the WS transport "
               "(no ws/wss, or older than 8.13.0) — Discord gateway untestable here\n");
        return 0;
    }

    (void)system("rm -f /tmp/test_integ_discord.db* /tmp/test_integ_discord.discord.*");

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
    if (pthread_create(&th, NULL, mock_gateway, NULL) != 0) { fprintf(stderr, "FAIL: thread\n"); return 1; }

    /* Stage the SHIPPED handler (copied from templates/) as the extension. */
    mkdir(EXT_DIR, 0755);
    char *js = util_read_file("templates/channel_discord.qjs", NULL);
    if (!js) { fprintf(stderr, "FAIL: cannot read templates/channel_discord.qjs\n"); return 1; }
    FILE *jf = fopen(JS_PATH, "w");
    if (!jf) { fprintf(stderr, "FAIL: cannot write %s\n", JS_PATH); return 1; }
    fputs(js, jf); fclose(jf); free(js);
    seed_db(port);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        execl("./build/cclaw", "cclaw", "--channel", "discord", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { fprintf(stderr, "FAIL: fork\n"); return 1; }

    sqlite3 *db = test_db_open(DB_PATH);
    int rc = 1;

    /* Poll up to ~8s for the @mention message to be emitted — it is sent last,
     * so once it lands the guild-no-mention drop has already been decided. */
    int got_mention = 0;
    for (int i = 0; i < 80 && !got_mention; i++) {
        usleep(100000);
        if (event_count(db, "help me") > 0) got_mention = 1;
    }
    if (!got_mention) FAIL("@mention guild message never reached channel_events");
    printf("  PASS: IDENTIFY -> READY -> @mention MESSAGE_CREATE -> emit\n");

    if (!g_identify_seen) FAIL("server never received IDENTIFY");
    printf("  PASS: handler sent IDENTIFY\n");

    if (event_count(db, "hi in dm") < 1) FAIL("DM message was not emitted");
    printf("  PASS: DM bypasses the mention gate\n");

    if (event_count(db, "hello world") != 0) FAIL("unmentioned guild message was NOT dropped");
    printf("  PASS: require_mention drops unmentioned guild chatter\n");

    /* Heartbeats are timer-driven (first at ~interval/2, tick ~1s), so they
     * can lag the message burst — poll rather than check instantly. */
    for (int i = 0; i < 50 && !g_heartbeat_seen; i++) usleep(100000);
    if (!g_heartbeat_seen) FAIL("server never received a heartbeat");
    printf("  PASS: heartbeat liveness\n");

    rc = 0;
fail:
    if (rc != 0 && db) {
        fprintf(stderr, "  -- diagnostics --\n");
        fprintf(stderr, "  identify=%d heartbeat=%d\n", g_identify_seen, g_heartbeat_seen);
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT payload FROM channel_events WHERE channel_name='discord';",
                               -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                fprintf(stderr, "  event: %s\n", sqlite3_column_text(st, 0));
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
    if (rc == 0) printf("all channel_discord integration tests passed\n");
    return rc;
}
