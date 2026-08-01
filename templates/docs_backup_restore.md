---
name: backup-restore
description: How to back up and restore CClaw — the `cclaw backup` snapshot verb, a rotating scheduled-backup pattern, off-box copies, and the stop-swap-restart restore procedure (including the separate key-file rule).
---

# Backing up and restoring CClaw

All durable state lives in one file, `cclaw.db` (`$CCLAW_DB_PATH`, or
`~/.cclaw/cclaw.db`). Backup is therefore one file to copy; the only thing that
lives *outside* it is the secret-encryption key (see "The key file" below).

## Taking a snapshot

`cclaw backup [dest]` writes a consistent single-file snapshot with SQLite's
`VACUUM INTO`. It:

- defaults `dest` to `<db>.backup.<yyyymmdd-HHMMSS>` when omitted;
- is **safe against a running daemon** — `VACUUM INTO` takes a read snapshot,
  so a mid-turn session is captured consistently (recovery reconciles it on the
  next start), and a busy writer is never locked out;
- **refuses to overwrite** an existing file and unlinks a partial one on error;
- respects the disk floor (`disk_min_free_mb`) — it won't be the write that
  fills the card.

```
cclaw backup                       # ~/.cclaw/cclaw.db.backup.20260717-184441
cclaw backup /mnt/usb/cclaw.db     # explicit destination
```

## Scheduling rotating backups

Policy — how often, how many to keep, where to copy — lives here, not in the
binary. The pattern: a cron job runs `cclaw backup`, then prunes to the newest
N snapshots, then (optionally) copies the freshest one off-box.

A cron payload is a `prompt` or a workspace `.qjs` `script` — there is no shell
payload — so set up a daily `cron_set` job whose `prompt` tells you to run
roughly this shell:

```sh
set -e
DB="${CCLAW_DB_PATH:-$HOME/.cclaw/cclaw.db}"
cclaw backup                                    # timestamped snapshot next to the DB
# keep the newest 7, delete older ones:
ls -1t "$DB".backup.* 2>/dev/null | tail -n +8 | xargs -r rm -f
# optional off-box copy of the freshest snapshot (needs an scp/rsync target the
# operator has granted — keep the destination in config, not hard-coded):
# scp "$(ls -1t "$DB".backup.* | head -1)" "$BACKUP_SCP_TARGET"
```

Choose the retention count and cadence for the deployment: an SD-card box wants
a small N (disk is the constraint); a server with an off-box target can keep
more. If you copy off-box, prefer the *encrypted* DB as-is — see the key file.

## Restoring

Restore is a deliberate manual procedure, not a verb — swapping the live DB
under a running daemon would corrupt it. Steps:

1. **Stop the daemon** (`systemctl --user stop cclaw`, or signal the process).
2. **Move the snapshot over the DB**: `mv <snapshot> <db>`.
3. **Delete the WAL siblings**: `rm -f <db>-wal <db>-shm`. This is not optional —
   a stale `-wal` from the old DB would be replayed onto the restored file and
   corrupt it.
4. **Restart the daemon.**

Owner staleness self-heals: the previous instance's `processes` row ages out and
its sessions are reclaimed automatically on start. State restored from an older
snapshot may reference workspace files that have since changed — that is
expected; the snapshot restores conversation and config state, not the
filesystem around it.

## The key file (back this up separately, once)

Secrets in the DB are encrypted with the key at `<db-dir>/.cclaw_key`. A DB
snapshot **without** that key leaves the `secrets` table as unreadable
ciphertext — which is a *feature* for off-box copies: your credentials do not
travel with your conversation data.

The key never changes after install, so the rule is simple: **back up
`.cclaw_key` once, at install time, and store it separately** from the routine
DB snapshots. Restoring DB + original key on the same box needs nothing extra.
The scheduled-backup job should *not* copy the key alongside every DB snapshot
by default — remind the operator to save it once instead.
