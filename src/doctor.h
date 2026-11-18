#ifndef CCLAW_DOCTOR_H
#define CCLAW_DOCTOR_H

#include "sqlite3.h"
#include <stdio.h>

/* One-shot diagnostic bundle. Prints redacted system info to stdout.
 * Runs before normal DB/config path — a broken DB is itself a finding.
 * Always returns 0 (the report is the product). */
int doctor_main(void);

/* The [compaction] section: per agent, what keeps its sessions under the
 * trim threshold (the bundled script, another script, or the fallback trim
 * only) and the newest compaction on its sessions; then every hook turned
 * off after three failures, with how to turn it back on (db_path names the
 * database in the command it prints). Plain SELECTs on db, which may be
 * beside a live daemon. */
void doctor_check_compaction(sqlite3 *db, const char *db_path, FILE *out);

#endif
