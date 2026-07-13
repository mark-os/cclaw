#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *util_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

int util_mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

void util_ensure_parent_dir(const char *path) {
    char *dup = strdup(path);
    if (!dup) return;
    char *slash = strrchr(dup, '/');
    if (slash && slash != dup) {
        *slash = '\0';
        util_mkdir_p(dup);
    }
    free(dup);
}

void util_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int util_copy_file(const char *src, const char *dst, mode_t mode) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (write(fd, buf, n) != (ssize_t)n) { rc = -1; break; }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    close(fd);
    return rc;
}

char *base64_encode(const unsigned char *buf, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = ((len + 2) / 3) * 4;
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    char *p = out;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        unsigned v = (unsigned)buf[i] << 16 | (unsigned)buf[i + 1] << 8 | buf[i + 2];
        *p++ = tbl[v >> 18];
        *p++ = tbl[(v >> 12) & 63];
        *p++ = tbl[(v >> 6) & 63];
        *p++ = tbl[v & 63];
    }
    if (i < len) {
        unsigned v = (unsigned)buf[i] << 16;
        if (i + 1 < len) v |= (unsigned)buf[i + 1] << 8;
        *p++ = tbl[v >> 18];
        *p++ = tbl[(v >> 12) & 63];
        *p++ = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        *p++ = '=';
    }
    *p = '\0';
    return out;
}

static int ascii_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int ascii_strcasecmp(const char *a, const char *b) {
    while (*a && ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b)) {
        a++; b++;
    }
    return ascii_lower((unsigned char)*a) - ascii_lower((unsigned char)*b);
}

int ascii_strncasecmp(const char *a, const char *b, size_t n) {
    for (; n > 0; n--, a++, b++) {
        int d = ascii_lower((unsigned char)*a) - ascii_lower((unsigned char)*b);
        if (d != 0 || *a == '\0') return d;
    }
    return 0;
}
