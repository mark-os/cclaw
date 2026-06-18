#ifndef CCLAW_APPROVAL_H
#define CCLAW_APPROVAL_H

#include <stdint.h>
#include <sqlite3.h>

typedef struct {
    int64_t id;
    int64_t session_id;
    char *tool_call_id;
    char *tool_name;
    char *action;
    char *scope;
    char *args_json;
    char *args_hash;
    char *state;
    char *decided_via;
    int64_t requested_at;
    int64_t expires_at;
} Approval;

/* Insert a pending approval. Computes args_hash from args_json (FNV-1a hex).
 * Returns the new row id (>0) or -1 on error. */
int64_t approval_create(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                        const char *tool_name, const char *action,
                        const char *scope, const char *args_json);

/* Get the pending approval for a session. Returns heap-allocated Approval or NULL. */
Approval *approval_get_pending(sqlite3 *db, int64_t session_id);

/* Resolve an approval (approve or deny). Returns heap-allocated resolved Approval or NULL. */
Approval *approval_resolve(sqlite3 *db, int64_t id, int approved, const char *decided_via);

/* Expire scope='once' approvals for a session. Returns count expired. */
int approval_expire_once(sqlite3 *db, int64_t session_id);

/* Free an Approval struct. */
void approval_free(Approval *a);

#endif
