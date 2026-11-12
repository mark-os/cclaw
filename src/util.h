#ifndef CCLAW_UTIL_H
#define CCLAW_UTIL_H

/* Small standalone helpers with no home of their own: whole-file read (size
 * capped), mkdir -p, the db-path resolution rule, base64, and locale-
 * independent ASCII case compare.
 */

#include <stddef.h>
#include <sys/types.h>
#include <pthread.h>

/* Read a whole file into a malloc'd, NUL-terminated buffer. out_len (if
 * non-NULL) receives the byte count, excluding the NUL. Returns NULL on
 * any error (missing file, empty file, oversized file, OOM). */
#define UTIL_READ_FILE_MAX (32L * 1024 * 1024)
char *util_read_file(const char *path, size_t *out_len);

/* The one db-path resolution rule: $CCLAW_DB_PATH > $HOME/.cclaw/cclaw.db >
 * ./cclaw.db. Heap-allocated; caller frees. */
char *util_resolve_db_path(void);

/* mkdir -p for path itself (every path component gets created). */
int util_mkdir_p(const char *path);

/* mkdir -p for path's parent directory only; path itself is not created.
 * Best-effort — errors are silently ignored, matching callers that just
 * want the parent to exist before creating path themselves. */
void util_ensure_parent_dir(const char *path);

/* Copy src to dst, creating/truncating dst with the given mode. */
int util_copy_file(const char *src, const char *dst, mode_t mode);
/* Free space (MB) on the filesystem holding `path`; -1 if it cannot be measured. */
long util_free_mb(const char *path);

/* Set O_NONBLOCK on fd. Best-effort — a failed fcntl leaves fd untouched. */
void util_set_nonblock(int fd);

/* Standard base64 (RFC 4648, with padding) into a malloc'd NUL-terminated
 * string. Returns NULL on OOM. len==0 yields an empty string. */
char *base64_encode(const unsigned char *buf, size_t len);

/* ASCII-only case-insensitive compare — unlike strcasecmp/strncasecmp these
 * ignore the process locale, so security checks (host matching, SQL keyword
 * gating) can't be bent by a locale where e.g. 'I'/'i' don't fold (tr_TR). */
int ascii_strcasecmp(const char *a, const char *b);
int ascii_strncasecmp(const char *a, const char *b, size_t n);

/* Split a comma-/space-/tab-separated list in place. Any run of ',', ' ', '\t'
 * is a delimiter, so this is the split and the trim in one pass — a config
 * value like "a, b ,, c" and "a b c" tokenize identically, and stray/leading/
 * trailing delimiters collapse to nothing rather than an empty token. Safe
 * for both content types this is used on (hostnames, chat ids) because
 * neither can legitimately contain internal whitespace, so treating space as
 * a delimiter rather than padding-only loses nothing.
 *
 * out[i] are pointers into s, NUL-terminated in place — s is mutated and must
 * outlive out. Returns the token count, capped at max.
 *
 * One shared implementation for what used to be five hand-rolled copies
 * (channel egress_hosts, admin_ids in three places, and a strtok_r variant)
 * that had drifted on whether space was a delimiter or just padding. */
int split_and_trim(char *s, char **out, int max);

/* True if the *runtime* libcurl can carry our WebSocket transport: it
 * advertises ws/wss AND is >= 8.13.0. The compile-time view lies: libcurl-
 * minimal ships the curl_ws_* declarations and symbols but strips the protocol
 * handlers, so a ws:// transfer fails at perform with "Unsupported protocol".
 * Probing the runtime protocol list is the only source of truth. The version
 * floor is the fragment-reassembly contract — see curl_ws_available() and
 * specs/channel-transports.md.
 *
 * Lives in util (a leaf) rather than channel_runner on purpose: the conn
 * integration test must skip rather than fail on a box without WS, and it
 * deliberately does not link the runner — it forks `cclaw --channel` as a
 * subprocess. Declaring this in channel_runner.h dragged channel.o and
 * dashboard.o into that test and broke the link on main.c's resolve_approval. */
int curl_ws_available(void);

/* Fill buf with len cryptographically random bytes. getrandom() where the
 * kernel has it (3.17+); /dev/urandom on the older kernels a static binary
 * can now reach. Returns 0 on success, -1 on failure — never a short fill. */
int util_random(void *buf, size_t len);

/* The CA bundle to hand libcurl as CURLOPT_CAINFO, or NULL to leave libcurl's
 * own default alone. A system libcurl knows where its distro keeps the bundle;
 * a libcurl linked into a static release binary does not — any path baked in
 * at build time describes the build machine. So: $SSL_CERT_FILE wins when
 * set; otherwise libcurl's compiled-in default is kept if it exists on this
 * host; otherwise the well-known distro locations are probed. *missing (may
 * be NULL) is set when nothing was found — HTTPS will then fail per request,
 * and plain-http providers still work, which is why this is not fatal at
 * startup. Resolved once per process. */
const char *util_ca_bundle(int *missing);

/* The one place threads are created. The stack size is named rather than left
 * to the libc: glibc's default follows RLIMIT_STACK (8M), musl's is ~128K, so
 * code that fits on one overflows on the other with no warning. Pages are
 * committed only when touched, so the reservation is free. detached != 0
 * creates the thread detached. Returns pthread_create's result. */
#ifndef CCLAW_THREAD_STACK
#define CCLAW_THREAD_STACK (1024 * 1024)
#endif
int thread_spawn(pthread_t *t, int detached, void *(*fn)(void *), void *arg);

#endif
