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
#include <curl/curl.h>

char *util_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    /* Cap bounds daemon OOM from an oversized workspace file (manifest, hook,
     * skill, channel JS) or media spool entry — the largest legitimate load
     * is a media file, well under this. */
    if (len <= 0 || len > UTIL_READ_FILE_MAX) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

char *util_resolve_db_path(void) {
    const char *env = getenv("CCLAW_DB_PATH");
    if (env) return strdup(env);
    const char *home = getenv("HOME");
    if (home) {
        size_t len = strlen(home);
        char *p = malloc(len + sizeof("/.cclaw/cclaw.db"));
        if (p) { sprintf(p, "%s/.cclaw/cclaw.db", home); return p; }
    }
    return strdup("cclaw.db");
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

int split_and_trim(char *s, char **out, int max) {
    int n = 0;
    if (!s) return 0;
    char *p = s;
    while (*p && n < max) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *end = p;
        while (*end && *end != ',' && *end != ' ' && *end != '\t') end++;
        char more = *end;
        *end = '\0';
        out[n++] = p;
        if (!more) break;
        p = end + 1;
    }
    return n;
}

int curl_ws_available(void) {
    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    if (!v || !v->protocols) return 0;
    /* Before 8.13.0 libcurl derived the frame flags from the opcode alone and
     * never surfaced the FIN bit: the opening fragment of a fragmented message
     * arrived as plain TEXT (indistinguishable from a whole message) and every
     * continuation as bare CONT with no type and no end marker. Reassembly is
     * not merely awkward there, it is impossible — nothing in the API says
     * where a message ends. 8.13.0 made the type ride every fragment and CONT
     * mean "more to come", which is what conn_recv_drain reads. Refuse the
     * older contract rather than silently truncate half the traffic. */
    if (v->version_num < 0x080D00) return 0;
    for (const char *const *p = v->protocols; *p; p++)
        if (ascii_strcasecmp(*p, "ws") == 0 || ascii_strcasecmp(*p, "wss") == 0) return 1;
    return 0;
}
