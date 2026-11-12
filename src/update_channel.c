#define _GNU_SOURCE
#include "update_channel.h"

#include "http.h"
#include "monocypher.h"
#include "sqlite3.h"
#include "update.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MANIFEST_NAME   "manifest.signed"
#define MANIFEST_FORMAT 1
#define FETCH_CAP       (64 * 1024 * 1024)
#define DEFAULT_KEEP    5

/* ── small helpers ─────────────────────────────────────────────── */

static void hex_enc(const uint8_t *in, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Exactly 2n hex digits → n bytes. */
static int hex_dec(const char *in, uint8_t *out, size_t n) {
    if (!in || strlen(in) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(in[2 * i]), lo = hex_val(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

/* Names that are joined onto a channel path/URL: no separators, no dotfiles. */
static int safe_name(const char *s) {
    if (!s || !s[0] || s[0] == '.' || strlen(s) > 128) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr(".-_", *p)))
            return 0;
    return 1;
}

static void b2_hex(const unsigned char *data, size_t len, char out[129]) {
    uint8_t h[64];
    crypto_blake2b(h, sizeof(h), data, len);
    hex_enc(h, sizeof(h), out);
}

static char *dupnull(const unsigned char *s) { return s ? strdup((const char *)s) : NULL; }

/* ── public pieces ─────────────────────────────────────────────── */

int uchan_is_local(const char *ch) { return ch && strncmp(ch, "file://", 7) == 0; }

int uchan_is_channel(const char *ch) {
    return uchan_is_local(ch) || (ch && strncmp(ch, "https://", 8) == 0);
}

void uchan_tag(long long seq, char *out, size_t cap) { snprintf(out, cap, "pre-%lld", seq); }

long long uchan_tag_seq(const char *tag) {
    if (!tag || strncmp(tag, "pre-", 4) != 0 || !tag[4]) return -1;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(tag + 4, &end, 10);
    if (errno || *end || v <= 0) return -1;
    return v;
}

int uchan_sig_ok(const unsigned char *msg, size_t len,
                 const char *sig_hex, const char *pub_hex) {
    uint8_t sig[64], pub[32];
    if (hex_dec(sig_hex, sig, sizeof(sig)) != 0) return 0;
    if (hex_dec(pub_hex, pub, sizeof(pub)) != 0) return 0;
    return crypto_eddsa_check(sig, pub, msg, len) == 0;
}

void uchan_release_free(UchanRelease *r) {
    if (!r) return;
    free(r->version); free(r->commit); free(r->branch);
    free(r->notes); free(r->published_at); free(r->file);
    memset(r, 0, sizeof(*r));
}

/* SQLite JSON1 as the parser, like everywhere else a db is in reach — here an
 * in-memory one, since the manifest may be read before any real DB opens. */
int uchan_parse_manifest(const char *json, const char *asset,
                         UchanRelease *out, char *why, size_t cap) {
    memset(out, 0, sizeof(*out));
    if (!safe_name(asset)) { snprintf(why, cap, "bad asset name"); return -1; }

    sqlite3 *m = NULL;
    if (sqlite3_open(":memory:", &m) != SQLITE_OK) {
        sqlite3_close(m);
        snprintf(why, cap, "cannot open sqlite");
        return -1;
    }
    char apath[200];
    snprintf(apath, sizeof(apath), "$.assets.\"%s\"", asset);
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(m,
            "SELECT json_valid(?1),"
            " json_extract(?1,'$.format'), json_extract(?1,'$.seq'),"
            " json_extract(?1,'$.version'), json_extract(?1,'$.commit'),"
            " json_extract(?1,'$.dirty'), json_extract(?1,'$.branch'),"
            " json_extract(?1,'$.notes'), json_extract(?1,'$.published_at'),"
            " json_extract(?1, ?2 || '.file'), json_extract(?1, ?2 || '.b2'),"
            " json_extract(?1, ?2 || '.size')", -1, &st, NULL) != SQLITE_OK) {
        snprintf(why, cap, "manifest query failed");
        goto done;
    }
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, apath, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 1) {
        snprintf(why, cap, "manifest is not valid JSON");
        goto done;
    }
    if (sqlite3_column_int(st, 1) != MANIFEST_FORMAT) {
        snprintf(why, cap, "unsupported manifest format %d", sqlite3_column_int(st, 1));
        goto done;
    }
    out->seq = sqlite3_column_int64(st, 2);
    out->version = dupnull(sqlite3_column_text(st, 3));
    out->commit = dupnull(sqlite3_column_text(st, 4));
    out->dirty = sqlite3_column_int(st, 5);
    out->branch = dupnull(sqlite3_column_text(st, 6));
    out->notes = dupnull(sqlite3_column_text(st, 7));
    out->published_at = dupnull(sqlite3_column_text(st, 8));
    out->file = dupnull(sqlite3_column_text(st, 9));
    const unsigned char *b2 = sqlite3_column_text(st, 10);
    out->size = sqlite3_column_int64(st, 11);
    if (out->seq <= 0) { snprintf(why, cap, "manifest has no seq"); goto done; }
    if (!out->file) {
        snprintf(why, cap, "channel has no %s build in pre-%lld", asset, out->seq);
        goto done;
    }
    if (!safe_name(out->file)) { snprintf(why, cap, "unsafe asset file name"); goto done; }
    if (!b2 || strlen((const char *)b2) != 128) {
        snprintf(why, cap, "asset has no BLAKE2b checksum");
        goto done;
    }
    snprintf(out->b2, sizeof(out->b2), "%s", (const char *)b2);
    rc = 0;
done:
    sqlite3_finalize(st);
    sqlite3_close(m);
    if (rc != 0) uchan_release_free(out);
    return rc;
}

int uchan_fetch(const char *ch, const char *name,
                char **data, size_t *len, char *why, size_t cap) {
    *data = NULL; *len = 0;
    if (!safe_name(name)) { snprintf(why, cap, "unsafe name"); return -1; }
    char loc[4096];
    size_t cl = strlen(ch);
    const char *sep = (cl && ch[cl - 1] == '/') ? "" : "/";

    if (uchan_is_local(ch)) {
        snprintf(loc, sizeof(loc), "%s%s%s", ch + 7, sep, name);
        *data = util_read_file(loc, len);
        if (!*data) {
            snprintf(why, cap, "cannot read %s: %s", loc, strerror(errno));
            return -1;
        }
        return 0;
    }
    if (strncmp(ch, "https://", 8) != 0) {
        snprintf(why, cap, "unsupported channel '%s'", ch);
        return -1;
    }
    snprintf(loc, sizeof(loc), "%s%s%s", ch, sep, name);
    const char *hdrs[] = { "Accept: application/octet-stream",
                           "Cache-Control: no-cache", NULL };
    HttpRequestOpts opts = {
        .url = loc, .method = "GET", .headers = hdrs, .timeout = 300,
        .follow_redirects = 1, .max_redirects = 5,
        .max_response_bytes = FETCH_CAP, .user_agent = "cclaw-update/1.0",
    };
    HttpResponse r = {0};
    int status = http_do(&opts, &r);
    if (status != 200 || !r.data || r.truncated) {
        snprintf(why, cap, "GET %s: HTTP %d%s%s%s", loc, status,
                 r.err_detail[0] ? " " : "", r.err_detail,
                 r.truncated ? " (truncated)" : "");
        http_response_free(&r);
        return -1;
    }
    *data = r.data; *len = r.len;
    r.data = NULL;
    http_response_free(&r);
    return 0;
}

/* Split "sig\njson", verify, parse. */
static int verify_and_parse(char *raw, size_t rawlen, const char *pub_hex,
                            const char *asset, UchanRelease *out,
                            char *why, size_t cap) {
    char *nl = memchr(raw, '\n', rawlen);
    if (!nl) { snprintf(why, cap, "manifest has no signature line"); return -1; }
    *nl = '\0';
    const char *json = nl + 1;
    size_t jlen = rawlen - (size_t)(json - raw);
    if (memchr(json, '\0', jlen)) { snprintf(why, cap, "manifest contains NUL"); return -1; }
    if (!uchan_sig_ok((const unsigned char *)json, jlen, raw, pub_hex)) {
        snprintf(why, cap, "manifest signature does not verify against update.pubkey");
        return -1;
    }
    return uchan_parse_manifest(json, asset, out, why, cap);
}

int uchan_latest(const char *ch, const char *pub_hex, const char *asset,
                 UchanRelease *out, char *why, size_t cap) {
    memset(out, 0, sizeof(*out));
    if (!pub_hex || strlen(pub_hex) != 64) {
        snprintf(why, cap, "update.pubkey is not set — a channel is only "
                           "trusted through its signing key");
        return -1;
    }
    char *raw = NULL; size_t rawlen = 0;
    if (uchan_fetch(ch, MANIFEST_NAME, &raw, &rawlen, why, cap) != 0) return -1;
    /* util_read_file / http bodies are NUL-terminated past len. */
    int rc = verify_and_parse(raw, rawlen, pub_hex, asset, out, why, cap);
    free(raw);
    return rc;
}

int uchan_fetch_asset(const char *ch, const UchanRelease *rel,
                      char **data, size_t *len, char *why, size_t cap) {
    if (uchan_fetch(ch, rel->file, data, len, why, cap) != 0) return -1;
    char h[129];
    b2_hex((const unsigned char *)*data, *len, h);
    if ((rel->size > 0 && (long long)*len != rel->size) || strcmp(h, rel->b2) != 0) {
        snprintf(why, cap, "%s does not match the signed manifest "
                           "(size %zu, expected %lld)", rel->file, *len, rel->size);
        free(*data); *data = NULL; *len = 0;
        return -1;
    }
    return 0;
}

int uchan_manifest_stamp(const char *ch, unsigned long long *out) {
    if (!uchan_is_local(ch)) return -1;
    char p[4096];
    snprintf(p, sizeof(p), "%s/%s", ch + 7, MANIFEST_NAME);
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    *out = ((unsigned long long)st.st_ino << 32) ^
           ((unsigned long long)st.st_mtim.tv_sec * 1000000000ULL +
            (unsigned long long)st.st_mtim.tv_nsec);
    if (*out == 0) *out = 1;   /* 0 means "never seen" to the caller */
    return 0;
}

/* ── cclaw publish ─────────────────────────────────────────────── */

static void default_key_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.config/cclaw/publish.key", home && home[0] ? home : ".");
}

static int write_atomic(const char *path, const void *data, size_t len, mode_t mode) {
    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        p += w; left -= (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int keygen(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        fprintf(stderr, "error: %s already exists — refusing to overwrite a "
                        "signing key\n", path);
        return 1;
    }
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; util_mkdir_p(dir); }

    uint8_t seed[32], sk[64], pk[32];
    if (getrandom(seed, sizeof(seed), 0) != (ssize_t)sizeof(seed)) {
        fprintf(stderr, "error: getrandom failed\n");
        return 1;
    }
    crypto_eddsa_key_pair(sk, pk, seed);
    char skh[129], pkh[65], line[140];
    hex_enc(sk, sizeof(sk), skh);
    hex_enc(pk, sizeof(pk), pkh);
    int n = snprintf(line, sizeof(line), "%s\n", skh);
    int rc = write_atomic(path, line, (size_t)n, 0600);
    crypto_wipe(sk, sizeof(sk)); crypto_wipe(skh, sizeof(skh)); crypto_wipe(line, sizeof(line));
    if (rc != 0) {
        fprintf(stderr, "error: cannot write %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("signing key: %s (keep it private)\n", path);
    printf("public key:  %s\n\n", pkh);
    printf("on each box that follows this channel:\n"
           "  sudo cclaw sqlite3 --write <db> \"INSERT INTO config(key,value)"
           " VALUES('update.pubkey','%s') ON CONFLICT(key) DO UPDATE SET"
           " value=excluded.value\"\n", pkh);
    return 0;
}

static int load_key(const char *path, uint8_t sk[64]) {
    size_t len = 0;
    char *s = util_read_file(path, &len);
    if (!s) {
        fprintf(stderr, "error: cannot read signing key %s: %s\n"
                        "  (create one with: cclaw publish --keygen)\n",
                path, strerror(errno));
        return -1;
    }
    char *nl = strchr(s, '\n');
    if (nl) *nl = '\0';
    int rc = hex_dec(s, sk, 64);
    crypto_wipe(s, len);
    free(s);
    if (rc != 0) fprintf(stderr, "error: %s is not a cclaw signing key\n", path);
    return rc;
}

/* Current seq in the directory, read unverified: we are the publisher. */
static long long current_seq(const char *dir) {
    char p[4096];
    snprintf(p, sizeof(p), "%s/%s", dir, MANIFEST_NAME);
    size_t len = 0;
    char *raw = util_read_file(p, &len);
    if (!raw) return 0;
    long long seq = 0;
    char *nl = strchr(raw, '\n');
    sqlite3 *m = NULL;
    if (nl && sqlite3_open(":memory:", &m) == SQLITE_OK) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(m, "SELECT json_extract(?1,'$.seq')", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, nl + 1, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) seq = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(m);
    free(raw);
    return seq;
}

static char *git_branch(void) {
    FILE *p = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
    if (!p) return NULL;
    char buf[256] = "";
    char *ok = fgets(buf, sizeof(buf), p);
    pclose(p);
    if (!ok) return NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf[0] ? strdup(buf) : NULL;
}

/* Remove <asset>-<n> files older than keep publishes. */
static void prune(const char *dir, const char *asset, long long seq, int keep) {
    DIR *d = opendir(dir);
    if (!d) return;
    size_t al = strlen(asset);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, asset, al) != 0 || e->d_name[al] != '-') continue;
        char *end = NULL;
        long long n = strtoll(e->d_name + al + 1, &end, 10);
        if (!end || *end || n <= 0 || n > seq - keep) continue;
        char p[4096];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
}

static int show(const char *dir) {
    char p[4096];
    snprintf(p, sizeof(p), "%s/%s", dir, MANIFEST_NAME);
    size_t len = 0;
    char *raw = util_read_file(p, &len);
    if (!raw) { fprintf(stderr, "no manifest at %s\n", p); return 1; }
    char *nl = strchr(raw, '\n');
    printf("%s\n", nl ? nl + 1 : raw);
    free(raw);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: cclaw publish [--dir DIR] [--binary PATH] [--asset NAME]\n"
        "                     [--notes TEXT] [--key PATH] [--keep N]\n"
        "       cclaw publish --keygen [--key PATH]\n"
        "       cclaw publish --show [--dir DIR]\n"
        "Defaults: --dir /var/lib/cclaw/channel, --binary this executable,\n"
        "          --asset this build's architecture, --key ~/.config/cclaw/publish.key\n");
}

int publish_main(int argc, char *argv[]) {
    const char *dir = "/var/lib/cclaw/channel", *binary = NULL, *asset = NULL;
    const char *notes = "", *keyarg = NULL;
    int keep = DEFAULT_KEEP, do_keygen = 0, do_show = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() do { if (++i >= argc) { usage(); return 2; } } while (0)
        if (!strcmp(a, "--dir")) { NEXT(); dir = argv[i]; }
        else if (!strcmp(a, "--binary")) { NEXT(); binary = argv[i]; }
        else if (!strcmp(a, "--asset")) { NEXT(); asset = argv[i]; }
        else if (!strcmp(a, "--notes")) { NEXT(); notes = argv[i]; }
        else if (!strcmp(a, "--key")) { NEXT(); keyarg = argv[i]; }
        else if (!strcmp(a, "--keep")) { NEXT(); keep = atoi(argv[i]); }
        else if (!strcmp(a, "--keygen")) do_keygen = 1;
        else if (!strcmp(a, "--show")) do_show = 1;
        else { usage(); return 2; }
#undef NEXT
    }
    char keypath[4096];
    if (keyarg) snprintf(keypath, sizeof(keypath), "%s", keyarg);
    else default_key_path(keypath, sizeof(keypath));

    if (do_keygen) return keygen(keypath);
    if (do_show) return show(dir);
    if (keep < 1) keep = 1;

    if (!asset) asset = update_default_asset();
    if (!asset || !safe_name(asset)) {
        fprintf(stderr, "error: no asset name for this architecture — pass --asset\n");
        return 2;
    }

    char selfbuf[4096];
    if (!binary) {
        ssize_t n = readlink("/proc/self/exe", selfbuf, sizeof(selfbuf) - 1);
        if (n <= 0) { fprintf(stderr, "error: cannot resolve own path\n"); return 1; }
        selfbuf[n] = '\0';
        binary = selfbuf;
    }

    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "error: channel directory %s does not exist\n", dir);
        return 1;
    }

    uint8_t sk[64];
    if (load_key(keypath, sk) != 0) return 1;

    /* The version string is what the installer's vetting step will see too. */
    char *version = update_run_capture(binary, "--version");
    if (!version) {
        crypto_wipe(sk, sizeof(sk));
        fprintf(stderr, "error: %s does not run here (--version failed)\n", binary);
        return 1;
    }
    char commit[64] = "";
    sscanf(version, "%*s %63s", commit);
    int dirty = strstr(commit, "-dirty") != NULL;

    size_t blen = 0;
    char *bin = util_read_file(binary, &blen);
    if (!bin || blen == 0) {
        crypto_wipe(sk, sizeof(sk));
        fprintf(stderr, "error: cannot read %s\n", binary);
        free(version); free(bin);
        return 1;
    }
    char b2[129];
    b2_hex((unsigned char *)bin, blen, b2);

    long long seq = current_seq(dir) + 1;
    char fname[200], fpath[4300];
    snprintf(fname, sizeof(fname), "%s-%lld", asset, seq);
    snprintf(fpath, sizeof(fpath), "%s/%s", dir, fname);
    int rc = 1;
    char *branch = git_branch();
    char *json = NULL;
    sqlite3 *m = NULL;

    if (write_atomic(fpath, bin, blen, 0644) != 0) {
        fprintf(stderr, "error: cannot write %s: %s\n", fpath, strerror(errno));
        goto out;
    }

    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    if (sqlite3_open(":memory:", &m) == SQLITE_OK) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(m,
                "SELECT json_object('format', ?1, 'seq', ?2, 'version', ?3,"
                " 'commit', ?4, 'dirty', json(?5), 'branch', ?6, 'notes', ?7,"
                " 'published_at', ?8, 'assets', json_object(?9,"
                " json_object('file', ?10, 'b2', ?11, 'size', ?12)))",
                -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int(q, 1, MANIFEST_FORMAT);
            sqlite3_bind_int64(q, 2, seq);
            sqlite3_bind_text(q, 3, version, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 4, commit, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 5, dirty ? "true" : "false", -1, SQLITE_STATIC);
            if (branch) sqlite3_bind_text(q, 6, branch, -1, SQLITE_STATIC);
            else sqlite3_bind_null(q, 6);
            sqlite3_bind_text(q, 7, notes, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 8, when, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 9, asset, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 10, fname, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 11, b2, -1, SQLITE_STATIC);
            sqlite3_bind_int64(q, 12, (long long)blen);
            if (sqlite3_step(q) == SQLITE_ROW)
                json = dupnull(sqlite3_column_text(q, 0));
        }
        sqlite3_finalize(q);
    }
    if (!json) { fprintf(stderr, "error: cannot build manifest\n"); unlink(fpath); goto out; }

    {
        uint8_t sig[64];
        crypto_eddsa_sign(sig, sk, (const uint8_t *)json, strlen(json));
        char sigh[129];
        hex_enc(sig, sizeof(sig), sigh);
        size_t mlen = 128 + 1 + strlen(json);
        char *man = malloc(mlen + 1);
        if (!man) { unlink(fpath); goto out; }
        snprintf(man, mlen + 1, "%s\n%s", sigh, json);
        char mpath[4300];
        snprintf(mpath, sizeof(mpath), "%s/%s", dir, MANIFEST_NAME);
        /* The manifest goes last and atomically: a reader sees the old
         * release or the new one, never a manifest naming a missing file. */
        if (write_atomic(mpath, man, mlen, 0644) != 0) {
            fprintf(stderr, "error: cannot write %s: %s\n", mpath, strerror(errno));
            free(man); unlink(fpath);
            goto out;
        }
        free(man);
    }
    prune(dir, asset, seq, keep);

    printf("published pre-%lld: %s%s\n", seq, version, dirty ? "  [dirty tree]" : "");
    printf("  %s/%s (%zu bytes)\n", dir, fname, blen);
    rc = 0;
out:
    crypto_wipe(sk, sizeof(sk));
    sqlite3_close(m);
    free(json); free(branch); free(bin); free(version);
    return rc;
}
