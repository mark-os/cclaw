# Operations — Backup, Restore, Update & Rollback

Durable state has one home: `cclaw.db` (`$CCLAW_DB_PATH`, default
`~/.cclaw/cclaw.db`), with its `-wal`/`-shm` siblings. Backup is copying one
file; restore is swapping it back with the daemon stopped. The one piece of
state *outside* the DB is the secret-encryption key (`<db-dir>/.cclaw_key`).

Implementation: `db_backup_to()` (`src/db.c`) does the snapshot; the
`cclaw backup` verb (`backup_main`, `src/cli_verbs.c`) wraps it; the operator-facing
prompt surface is the shipped `backup-restore` skill
(`templates/docs_backup_restore.md`). Restore is documented here and in the
skill — deliberately not a verb (swapping a live DB would corrupt it). The one
verb that does swap the DB, `cclaw rollback`, exists because it can guarantee
the file is closed when it does (see below).

## Backup — `cclaw backup [dest]`

Writes a consistent single-file snapshot with `VACUUM INTO`:

- **Consistent under a live daemon.** `VACUUM INTO` runs inside a read
  transaction, so it captures committed state including committed WAL content,
  with no torn write. A session caught mid-turn (e.g. `llm_running`) is
  captured as-is; on restore, restart recovery (`db_recover_stale_sessions`,
  see [error-handling.md]) reconciles it — the snapshot only has
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

1. Stop the daemon (`systemctl --user stop cclaw`; for a system install `systemctl stop cclaw` or `/etc/init.d/cclaw stop`; or signal the process).
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

## Update — `cclaw update`

`src/update.c`. Pulls the release asset for this architecture from GitHub
(`update.repo`, `update.asset`), or sideloads one with `--file <path>`; both go
through the same rails:

1. **Integrity.** The download is checked against the release's
   `checksums.b2` (BLAKE2b-512, written by `b2sum` in CI; monocypher has
   BLAKE2b and no SHA-256). Mismatch or an unlisted asset refuses; a release
   with no `checksums.b2` is reported and installed unchecked. Integrity, not
   authenticity.
2. **Vet.** The candidate must run (`--version`).
3. **Schema handshake.** The candidate is asked `--schema-range`; a build that
   could not open this database (below its floor, or a downgrade) is refused.
   Patches are forward-only, so "install and see" is not undoable by swapping
   the binary back.
4. **Disk floor.** Refused when the snapshot or the `.prev` copy would take free
   space under `disk_min_free_mb`.
5. **Snapshot.** `<db>.preupdate` via `VACUUM INTO` — one generation, replaced
   each update, refused if it cannot be written. (The schema migration takes
   its own `<db>.v<N>.bak` at the next daemon start, independently.)
6. **Swap.** The running binary is copied to `<self>.prev`, the candidate
   renamed over `<self>` (atomic, same directory).
7. **Restart.** Default: `SIGUSR2` — the daemon shuts down and `exec`s the
   binary now on disk, keeping its pid, so no supervisor ever sees an exit.
   `update.restart_command` overrides it for deployments whose daemon must
   re-read an env file. With no daemon running, the binary is simply in place.
8. **Await.** 90 s for a *new* `processes` row (fresh `instance_id`, polled on
   a fresh read-only connection each second — a connection from before the
   restart can sit on a WAL snapshot that never sees it). Timeout reverts the
   binary from `.prev` and leaves the database alone.
9. **Crash-loop guard.** `update.verify` is armed; the daemon counts its own
   starts against it *before* running the schema migration (so a crash inside
   a patch is counted while `.prev` can still open the un-migrated file), and
   clears it after 5 minutes of uptime. Three starts inside 5 minutes revert
   the binary to `.prev` — only if `.prev` can open the database as it now is;
   otherwise the guard stays passive and tells the agent to suggest
   `cclaw rollback`. The bad build is kept as `<self>.bad`.

Every exit writes `update.last` (`{tag, at, outcome}`), which `--doctor`
shows: `ok`, `installed_no_daemon`, `installed_restart_pending`,
`reverted_no_restart`, `verified`, `crash_loop_reverted`,
`crash_loop_no_revert`, `rollback_requested`.

## Pre-release channels — `cclaw publish`

Releases are for milestones; most changes ship incrementally. A **channel** is
a directory of builds outside GitHub — local (`file:///var/lib/cclaw/channel`)
or any static https host (an R2 bucket) — and a box follows one by setting
`update.channel`. Empty means GitHub releases via `update.repo`, as before.

    cclaw publish --keygen                 # once: ~/.config/cclaw/publish.key, prints the pubkey
    make && ./build/cclaw publish --notes "what changed"   # every incremental build

Each publish is `pre-<seq>`, seq+1 of the last. The directory holds
`manifest.signed` (line 1: EdDSA signature over the rest; rest: JSON with
seq, version, commit, dirty, branch, notes, and per-asset file/BLAKE2b/size)
and the binaries it names (`cclaw-linux-arm64-<seq>`, last `--keep` kept).
Binary first, manifest last by atomic rename, so a reader sees one whole
release or the previous one. Dirty trees are publishable — `--version` says
`<commit>-dirty` and the agent's notice says so — because the incremental
loop is the point; seq, not the commit, is the identity.

**Trust is the signature, not the transport.** `update.pubkey` is required
for every channel, so a writable bucket or a file planted in the spool offers
nothing the publisher's key did not sign; the manifest's BLAKE2b then binds
the binary to it. The signing key never goes on a box that only follows.

**Notice → approval → install.** A `file://` channel is a `stat` per DB poll
(inode + ns mtime, so two publishes in one second are both seen) and re-read
only on change; an `https://` channel waits out `update.check_interval_minutes`
(or `_hours`). A newer seq than `update.installed_tag` drops one note in the
default agent's inbox; the agent offers `install_update` with `tag: pre-<seq>`;
approval runs `cclaw update --tag pre-<seq>`, which installs only the
channel's *newest* build (a stale tag is refused) through `install_candidate`
— the same schema handshake, snapshot, atomic swap, restart and crash-loop
revert as a release. `cclaw rollback` undoes it the same way.

**Serving over https.** Upload the binary, then `manifest.signed`, to any
static host — `scripts/channel_sync_r2.sh <dir> <bucket>[/prefix]` does it for
R2 with wrangler — and point boxes at its public URL. Only https is accepted.

## Rollback — `cclaw rollback [--yes]`

Undoes the last update *entirely*: `<db>.preupdate` becomes the database
again, `<self>.prev` the binary. This is the way back from the case the
automatic rails cannot handle — a build that migrated the database and then
turned out to be bad, where `.prev` alone would refuse the migrated file.

The verb prints the whole plan before asking — snapshot schema version and
age, how many `entries` written since the snapshot will be discarded, which
build comes back — and confirms on a tty (`--yes` for scripts). It then writes
a marker, `<db>.rollback` (two lines: where the current database goes, which
binary to restore), and the swap itself happens at whichever of three moments
comes first, each one a point where **nothing has the database open**:

- in the verb itself, when no daemon is running;
- in the daemon's shutdown tail, after `db_close()` (the verb signals the
  daemon with `SIGUSR2` or `update.restart_command`, then waits for a
  `processes` row born *after* it asked — the restored database carries an
  older row, possibly with the same pid);
- at daemon startup, before `db_open()` — which is what catches a marker left
  by a daemon that never came back.

Applied by `update_rollback_apply()`, which is only ever run by the build on
disk — the *new* one, the one that knows the protocol — and then `exec`s the
restored build. Order: database first (`<db>` → `<db>.failed-<tag>`, with its
`-wal`/`-shm`; `<db>.preupdate` → `<db>`), binary second (`<self>` copied to
`<self>.bad`, `<self>.prev` renamed over `<self>`). The wrong order would leave
an old binary beside a migrated database — the one state that cannot start;
this order's worst case (old database, new binary) merely migrates again at
the next start. Preconditions are checked before anything moves, the marker is
consumed on every path, and a failure leaves nothing half-done.

Nothing is deleted: `<db>.failed-<tag>` keeps what the new build wrote, and
`--doctor`'s `[update]` section lists every leftover (`.prev`, `.bad`,
`.preupdate`, `.v<N>.bak`, `.failed-*`, a pending `.rollback`) with a plain
answer to "can I delete this?".

[error-handling.md]: error-handling.md

## Shared files — `/share`

`share_file` (an agent tool, not in the default grants) puts one workspace
file behind a link: `<base>/share/<token>/<name>`, valid for the hours asked
(default 24, capped by `share_max_hours`). It exists for what a chat
attachment cannot carry — `channel_send`'s `file` is the direct way to show
someone a picture. State is one row in `shares`
([schema.md](schema.md#shares)); nothing is copied.

- **`<base>` is `web_public_url`** when set, else
  `http://<first non-loopback address>:<web_port>`. A daemon reached through
  a tunnel or a reverse proxy has no way to know that address, so the
  operator states it: `web_public_url` is the base of every link the daemon
  hands out, `/admin` included.
- **The token is the only authentication.** The link is meant to be opened
  by whoever it was given to. Anything that is not a live share — unknown,
  malformed, expired — gets the same 404.
- **The path is re-checked on every request.** The agent can still write its
  workspace, so the server opens the stored path, asks the kernel where the
  descriptor really is, and serves it only if that is still the stored path.
  A file replaced by a symlink, or a directory swapped on the way, is a 404,
  not the new target.
- **An agent's file never runs as a page.** `/admin` is on the same origin.
  Images and PDFs (recognized by their bytes) and text are shown inline,
  everything else is a download; every response carries
  `Content-Security-Policy: sandbox`, `X-Content-Type-Options: nosniff` and
  `Cache-Control: no-store`. An HTML file is served as `text/plain`.
