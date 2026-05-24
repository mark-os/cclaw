#ifndef CCLAW_LANDLOCK_H
#define CCLAW_LANDLOCK_H

/* V22: Apply landlock filesystem restrictions to current process.
 * workspace_path: writable directory for agent.
 * db_path: SQLite DB file (needs read+write for WAL).
 * Returns 0 on success, -1 on failure (logged, non-fatal). */
int landlock_apply(const char *workspace_path, const char *db_path);

#endif
