#ifndef CCLAW_H
#define CCLAW_H

#define CCLAW_VERSION "0.1.0"

/* DB schema generation. Bumped whenever templates/schema.sql changes shape.
 * Existing DBs are patched forward at startup (schema_patches[] in db.c);
 * fresh DBs get the full schema at the current version. */
#define CCLAW_SCHEMA_VERSION 36

#endif
