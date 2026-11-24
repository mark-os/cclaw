#define _POSIX_C_SOURCE 200809L
#include "inbox_cap.h"
#include "context.h"
#include "run_tool.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Written while the caller holds the write lock, before the entry exists. If
 * the transaction then rolls back, the inbox row stays queued and the next
 * drain rewrites the same file (the name is keyed by the AUTOINCREMENT inbox
 * id, so it is never another item's). O_NOFOLLOW: a symlink planted at the
 * path fails the open rather than redirecting the write. */
static int spill_write(const char *path, const char *text, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (size_t)w;
    }
    int ok = (off == len);
    if (close(fd) != 0) ok = 0;
    return ok ? 0 : -1;
}

char *inbox_cap_text(sqlite3 *db, int64_t session_id, int64_t inbox_id,
                     const char *text) {
    if (!text) return NULL;
    size_t len = strlen(text);
    if (len <= RUNTOOL_RESULT_MAX) return NULL;

    /* Back up to a character's first byte so the kept prefix stays valid
     * UTF-8 (same rule as truncate_and_spill). */
    size_t cut = RUNTOOL_RESULT_MAX;
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--;

    /* The text was already through the secret scanner at inbox insert, so
     * the file holds exactly what the entry would have held uncut. Same home
     * as a tool result's spill: the agent's workspace, which its file tools
     * can read under every containment. */
    char id[64];
    snprintf(id, sizeof(id), "inbox-%lld", (long long)inbox_id);
    char path[PATH_MAX + 64];
    int saved = spill_path_build(db, session_id, id, path, sizeof(path)) == 0 &&
                spill_write(path, text, len) == 0;
    if (!saved)
        LOG_WARN_("inbox: item %lld over %d bytes, full text not saved",
                  (long long)inbox_id, RUNTOOL_RESULT_MAX);

    char note[PATH_MAX + 192];
    if (saved)
        snprintf(note, sizeof(note),
                 "\n[truncated — showing first %zu bytes of %zu. Full message: %s]",
                 cut, len, path);
    else
        snprintf(note, sizeof(note),
                 "\n[truncated — showing first %zu bytes of %zu; the full message could not be saved]",
                 cut, len);

    size_t nlen = strlen(note);
    char *out = malloc(cut + nlen + 1);
    if (!out) return NULL;
    memcpy(out, text, cut);
    memcpy(out + cut, note, nlen + 1);
    return out;
}
