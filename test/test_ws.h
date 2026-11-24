#ifndef CCLAW_TEST_WS_H
#define CCLAW_TEST_WS_H

/* Server side of a loopback WebSocket mock for the channel.conn.* suites:
 * the RFC 6455 handshake (SHA-1 for the accept key), unmasked text frames
 * out, masked client frames in. Needs _GNU_SOURCE for strcasestr. */

#include "util.h"          /* base64_encode */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ── Minimal SHA-1 (public domain, for the handshake accept key) ── */
typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; size_t n; } Sha1;

static inline uint32_t sha1_rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static inline void sha1_block(Sha1 *s, const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = sha1_rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = sha1_rol(a,5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rol(b,30); b = a; a = t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}

static inline void sha1_init(Sha1 *s) {
    s->h[0]=0x67452301; s->h[1]=0xEFCDAB89; s->h[2]=0x98BADCFE;
    s->h[3]=0x10325476; s->h[4]=0xC3D2E1F0; s->len=0; s->n=0;
}

static inline void sha1_update(Sha1 *s, const void *data, size_t len) {
    const unsigned char *p = data;
    s->len += len;
    while (len) {
        size_t take = 64 - s->n; if (take > len) take = len;
        memcpy(s->buf + s->n, p, take); s->n += take; p += take; len -= take;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static inline void sha1_final(Sha1 *s, unsigned char out[20]) {
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

/* ── Socket I/O ── */

static inline int ws_read_n(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static inline int ws_write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

/* A listening socket on 127.0.0.1 with an ephemeral port. accept() times
 * out after `timeout_s` so a mock thread never blocks forever. */
static inline int ws_listen_loopback(int backlog, int timeout_s, int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof(sa);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, backlog) != 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(sa.sin_port);
    struct timeval tv = {timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* ── WS framing (server side) ── */

static inline int ws_extract_key(const char *req, char *out, size_t cap) {
    const char *h = strcasestr(req, "Sec-WebSocket-Key:");
    if (!h) return -1;
    h += strlen("Sec-WebSocket-Key:");
    while (*h == ' ' || *h == '\t') h++;
    size_t n = 0;
    while (*h && *h != '\r' && *h != '\n' && n < cap - 1) out[n++] = *h++;
    out[n] = '\0';
    return 0;
}

/* Accept one client on `lfd` and complete the upgrade: read the request,
 * answer 101 with accept = base64(sha1(key + GUID)). Returns the connected
 * fd (10 s read timeout), or -1. */
static inline int ws_accept_upgrade(int lfd) {
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) return -1;
    struct timeval tv = {10, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[2048]; size_t rn = 0;
    while (rn < sizeof(req) - 1) {
        ssize_t r = read(cfd, req + rn, sizeof(req) - 1 - rn);
        if (r <= 0) { close(cfd); return -1; }
        rn += (size_t)r; req[rn] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    char key[128];
    if (ws_extract_key(req, key, sizeof(key)) != 0) { close(cfd); return -1; }

    char concat[256];
    snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    Sha1 s; sha1_init(&s); sha1_update(&s, concat, strlen(concat));
    unsigned char digest[20]; sha1_final(&s, digest);
    char *accept = base64_encode(digest, 20);
    if (!accept) { close(cfd); return -1; }

    char resp[256];
    int rl = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    free(accept);
    if (ws_write_all(cfd, resp, (size_t)rl) != 0) { close(cfd); return -1; }
    return cfd;
}

/* Send an unmasked text frame (server->client). */
static inline int ws_send_text(int fd, const char *s) {
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
    if (ws_write_all(fd, hdr, hn) != 0) return -1;
    return ws_write_all(fd, s, len);
}

/* Read one client frame (masked). Returns the opcode and fills payload
 * (NUL-terminated); -1 on a socket error or close. */
static inline int ws_read_frame(int fd, char *payload, size_t cap, size_t *out_len) {
    unsigned char b[2];
    if (ws_read_n(fd, b, 2) != 0) return -1;
    int opcode = b[0] & 0x0f;
    int masked = b[1] & 0x80;
    uint64_t len = b[1] & 0x7f;
    if (len == 126) {
        unsigned char e[2]; if (ws_read_n(fd, e, 2) != 0) return -1;
        len = (uint64_t)e[0] << 8 | e[1];
    } else if (len == 127) {
        unsigned char e[8]; if (ws_read_n(fd, e, 8) != 0) return -1;
        len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    unsigned char mask[4] = {0};
    if (masked && ws_read_n(fd, mask, 4) != 0) return -1;
    if (len >= cap) return -1;              /* test frames are small */
    if (len && ws_read_n(fd, payload, (size_t)len) != 0) return -1;
    if (masked) for (uint64_t i = 0; i < len; i++)
        payload[i] = (char)((unsigned char)payload[i] ^ mask[i % 4]);
    payload[len] = '\0';
    if (out_len) *out_len = (size_t)len;
    return opcode;
}

#endif
