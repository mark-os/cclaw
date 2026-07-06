#ifndef CCLAW_H
#define CCLAW_H

#define CCLAW_VERSION "0.1.0"

/* DB schema generation. No migrations yet (no users) — bump this whenever
 * templates/schema.sql changes shape, and stale DBs are refused at startup
 * with a "delete the DB" message rather than silently misbehaving. */
#define CCLAW_SCHEMA_VERSION 11

#endif
