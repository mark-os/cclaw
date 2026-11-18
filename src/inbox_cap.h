#ifndef CCLAW_INBOX_CAP_H
#define CCLAW_INBOX_CAP_H

#include "sqlite3.h"
#include <stdint.h>

/* Cap one inbox item's text as it becomes a user entry. Over
 * RUNTOOL_RESULT_MAX the text is cut at a UTF-8 boundary, the full text is
 * written to <session spill dir>/inbox-<inbox_id>.out, and a note naming that
 * file is appended. A failed write still cuts, with a note that the full text
 * was not saved. Returns a malloc'd replacement, or NULL to keep `text`. */
char *inbox_cap_text(sqlite3 *db, int64_t session_id, int64_t inbox_id,
                     const char *text);

#endif
