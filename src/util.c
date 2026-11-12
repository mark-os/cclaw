#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <curl/curl.h>
#include "log.h"

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

/* 64-bit math so the block-count product doesn't overflow on 32-bit targets
   (ARMv5TE). */
long util_free_mb(const char *path) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return -1;
    return (long)((uint64_t)vfs.f_bavail * vfs.f_frsize / (1024 * 1024));
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
#if LIBCURL_VERSION_NUM < 0x075600
    /* Built against pre-WebSocket headers: channel_runner compiled its
     * stand-ins, so a newer runtime libcurl does not make the transport real. */
    return 0;
#endif
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

int thread_spawn(pthread_t *t, int detached, void *(*fn)(void *), void *arg) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) return rc;
    rc = pthread_attr_setstacksize(&attr, CCLAW_THREAD_STACK);
    if (rc == 0 && detached)
        rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (rc == 0) rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

static const char *g_ca_path;
static int g_ca_missing;
static pthread_once_t g_ca_once = PTHREAD_ONCE_INIT;

static void ca_bundle_resolve(void) {
    const char *env = getenv("SSL_CERT_FILE");
    if (env && env[0]) {
        /* An explicit choice is never second-guessed: an unreadable file is
         * still handed to libcurl so the request fails rather than quietly
         * verifying against some other bundle. */
        g_ca_path = env;
        if (access(env, R_OK) != 0) {
            g_ca_missing = 1;
            LOG_ERROR_("SSL_CERT_FILE=%s is not readable: HTTPS requests will fail", env);
        }
        return;
    }

#if LIBCURL_VERSION_NUM >= 0x075400   /* cainfo/capath reported since 7.84.0 */
    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    if (v && v->age >= CURLVERSION_TENTH &&
        ((v->cainfo && access(v->cainfo, R_OK) == 0) ||
         (v->capath && access(v->capath, R_OK) == 0)))
        return;
#else
    /* Older headers cannot ask; such a libcurl is always the system's own. */
    return;
#endif

    static const char *const known[] = {
        "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Alpine, OpenWrt */
        "/etc/pki/tls/certs/ca-bundle.crt",                  /* RHEL, Fedora, Amazon Linux */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* RHEL 7+ */
        "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
        "/etc/ssl/cert.pem",                                 /* Alpine, BSDs */
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (access(known[i], R_OK) == 0) { g_ca_path = known[i]; return; }

    g_ca_missing = 1;
    LOG_ERROR_("no CA certificate bundle found: HTTPS requests will fail. "
               "Install ca-certificates or set SSL_CERT_FILE");
}

const char *util_ca_bundle(int *missing) {
    pthread_once(&g_ca_once, ca_bundle_resolve);
    if (missing) *missing = g_ca_missing;
    return g_ca_path;
}

int util_random(void *buf, size_t len) {
    unsigned char *p = buf;
    size_t got = 0;
    int fd = -1;
    while (got < len) {
        ssize_t n = fd < 0 ? getrandom(p + got, len - got, 0)
                           : read(fd, p + got, len - got);
        if (n > 0) { got += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (fd < 0 && n < 0 && errno == ENOSYS) {
            fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (fd >= 0) continue;
        }
        break;
    }
    if (fd >= 0) close(fd);
    return got == len ? 0 : -1;
}
