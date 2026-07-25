#ifndef CCLAW_UTIL_H
#define CCLAW_UTIL_H

/* Small standalone helpers with no home of their own: whole-file read (size
 * capped), mkdir -p, the db-path resolution rule, base64, and locale-
 * independent ASCII case compare.
 */

#include <stddef.h>
#include <sys/types.h>

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

/* True if the *runtime* libcurl advertises the ws/wss protocols. The
 * compile-time view lies: libcurl-minimal ships the curl_ws_* declarations and
 * symbols but strips the protocol handlers, so a ws:// transfer fails at
 * perform with "Unsupported protocol". Probing the runtime protocol list is
 * the only source of truth (see specs/channel-transports.md).
 *
 * Lives in util (a leaf) rather than channel_runner on purpose: the conn
 * integration test must skip rather than fail on a box without WS, and it
 * deliberately does not link the runner — it forks `cclaw --channel` as a
 * subprocess. Declaring this in channel_runner.h dragged channel.o and
 * dashboard.o into that test and broke the link on main.c's resolve_approval. */
int curl_ws_available(void);

#endif
