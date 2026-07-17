# Operations — Backup & Restore

Durable state has one home: `cclaw.db` (`$CCLAW_DB_PATH`, default
`~/.cclaw/cclaw.db`), with its `-wal`/`-shm` siblings. Backup is copying one
file; restore is swapping it back with the daemon stopped. The one piece of
state *outside* the DB is the secret-encryption key (`<db-dir>/.cclaw_key`).

Implementation: `db_backup_to()` (`src/db.c`) does the snapshot; the
`cclaw backup` verb (`src/main.c`, `backup_main`) wraps it; the operator-facing
prompt surface is the shipped `backup-restore` skill
(`templates/docs_backup_restore.md`). Restore is documented here and in the
skill — deliberately not a verb (swapping a live DB would corrupt it).

## Backup — `cclaw backup [dest]`

Writes a consistent single-file snapshot with `VACUUM INTO`:

- **Consistent under a live daemon.** `VACUUM INTO` runs inside a read
  transaction, so it captures committed state including committed WAL content,
  with no torn write. A session caught mid-turn (e.g. `llm_running`) is
  captured as-is; on restore, restart recovery (`db_recover_stale_sessions`,
  see [error-handling.md] / review-5 F8) reconciles it — the snapshot only has
  to be internally consistent, which it is.
- **No writer lockout.** WAL lets the snapshot's read transaction coexist with
  a busy writer; a concurrent turn keeps writing while the backup runs.
- **Fail-closed.** Refuses to overwrite an existing destination and unlinks a
  partial snapshot on error.
- **Disk-floor aware.** Refuses when free space is under `disk_min_free_mb`, so
  a backup is never the write that fills the disk.

`dest` defaults to `<db>.backup.<yyyymmdd-HHMMSS>`. Rotation, cadence, and
off-box copying are **policy**, and live in the `backup-restore` skill (a cron
that runs the verb, prunes to N snapshots, optionally copies off-box) — not in
the binary.

## Restore

Manual procedure, daemon stopped:

1. Stop the daemon (`systemctl --user stop cclaw`, or signal the process).
2. Move the snapshot over the DB: `mv <snapshot> <db>`.
3. **Delete the WAL siblings**: `rm -f <db>-wal <db>-shm`. Mandatory — a stale
   `-wal` from the old DB would be replayed onto the restored file and corrupt
   it.
4. Restart the daemon.

The dead instance's `processes` row ages out on its TTL and its sessions are
reclaimed by `db_recover_stale_sessions` — owner staleness self-heals, no manual
step. State restored from an older snapshot may reference workspace files that
have since changed; that is expected and out of scope (the snapshot restores DB
state, not the surrounding filesystem).

## The key file

`secrets` rows are ChaCha20-Poly1305 ciphertext under `<db-dir>/.cclaw_key`. A
snapshot without that key is unreadable ciphertext — a feature for off-box
copies (credentials don't travel with conversation data). The key is fixed at
install and never rotates, so the rule is: **back up `.cclaw_key` once, at
install, stored separately** from routine DB snapshots. DB + original key on the
same box restores with nothing extra. The scheduled-backup skill reminds the
operator to save the key once rather than copying it beside every snapshot.

[error-handling.md]: error-handling.md
