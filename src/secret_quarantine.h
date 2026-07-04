#ifndef CCLAW_SECRET_QUARANTINE_H
#define CCLAW_SECRET_QUARANTINE_H

#include "tool_shell.h"   /* ShellSecret */
#include <sqlite3.h>
#include <stddef.h>

/* DLP quarantine (specs/security.md): a drop-in replacement for
 * tool_result_postprocess that CAPTURES scanner-detected credentials instead
 * of shredding them. Deinterpolates known {{SECRET:name}} values same as
 * before, then for every remaining secret_scan() finding: mints (or reuses,
 * on a value match) a DB-backed secret named PENDING_<RULEID>_<n>,
 * source='quarantine' status='pending', and replaces the match with its
 * {{SECRET:name}} placeholder — so the plaintext leaves the context window
 * exactly like any other secret, and composes with the existing fail-closed
 * binding rule for free (born unbound -> first use parks).
 *
 * Spam guard: once the DB holds >= 32 pending secrets, falls back to the
 * plain secret_scan_redact shred for further findings (a hostile page can't
 * fill the store with junk rows). Caller owns the returned string; NULL
 * result in, NULL out. */
char *tool_result_postprocess_q(sqlite3 *db, const char *result,
                                const ShellSecret *secrets, size_t count);

#endif
