#ifndef CCLAW_H
#define CCLAW_H

#define CCLAW_VERSION "0.1.0"

/* DB schema generation. Bumped whenever templates/schema.sql changes shape.
 * Existing DBs are patched forward at startup (schema_patches[] in db.c);
 * fresh DBs get the full schema at the current version. */
#define CCLAW_SCHEMA_VERSION 51

/* Oldest DB generation this build can patch forward from. Together with
 * CCLAW_SCHEMA_VERSION this is the range of databases the binary accepts, which
 * `cclaw --schema-range` prints so an update can be refused *before* it is
 * installed — patches are forward-only, so a binary that strands the database
 * cannot simply be swapped back out. */
#define CCLAW_SCHEMA_MIN 40   /* schema freeze 2026-07-31 — no patches below this */

#endif
