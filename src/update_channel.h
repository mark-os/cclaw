#ifndef CCLAW_UPDATE_CHANNEL_H
#define CCLAW_UPDATE_CHANNEL_H

/* Update channels — incremental, pre-release builds outside GitHub releases.
 *
 * A channel is a directory, local (file:///var/lib/cclaw/channel) or served
 * over https (an R2 bucket, any static host), holding one signed manifest and
 * the binaries it names:
 *
 *   manifest.signed        line 1: EdDSA signature (hex) over the rest
 *                          rest:   JSON {format, seq, version, commit, dirty,
 *                                  branch, notes, published_at,
 *                                  assets:{<asset>:{file,b2,size}}}
 *   cclaw-linux-arm64-42   the binary for seq 42 (older seqs pruned)
 *
 * `cclaw publish` appends: seq+1, copy binary, sign, atomic rename of the
 * manifest. Installs go through the same rails as a release (`cclaw update`
 * → install_candidate); a channel build's tag is "pre-<seq>".
 *
 * Trust lives in the signature, not the transport: update.pubkey is required
 * for every channel, so a writable bucket or a planted file in the spool can
 * offer nothing the publisher's key did not sign. The BLAKE2b in the manifest
 * then binds the binary to that signature. */

#include <stddef.h>
#include <time.h>

typedef struct {
    long long seq;
    int dirty;
    char *version;       /* `<binary> --version` at publish time */
    char *commit;
    char *branch;
    char *notes;
    char *published_at;
    char *file;          /* asset file name inside the channel */
    char b2[129];        /* BLAKE2b-512 hex of the asset */
    long long size;
} UchanRelease;

/* 1 for a file:// or https:// channel; 0 for empty or github: (the
 * release path). */
int uchan_is_channel(const char *channel);
int uchan_is_local(const char *channel);

/* "pre-<seq>" ↔ seq. uchan_tag_seq returns -1 for anything else. */
void uchan_tag(long long seq, char *out, size_t cap);
long long uchan_tag_seq(const char *tag);

/* Signature check of msg against a hex signature and hex public key. */
int uchan_sig_ok(const unsigned char *msg, size_t len,
                 const char *sig_hex, const char *pub_hex);

/* Parse manifest JSON (already verified) and select `asset`. 0 ok. */
int uchan_parse_manifest(const char *json, const char *asset,
                         UchanRelease *out, char *why, size_t cap);

/* Read `<channel>/<name>` (file:// or https://). Caller frees *data. */
int uchan_fetch(const char *channel, const char *name,
                char **data, size_t *len, char *why, size_t cap);

/* Fetch + verify + parse the channel's manifest. 0 ok. */
int uchan_latest(const char *channel, const char *pub_hex, const char *asset,
                 UchanRelease *out, char *why, size_t cap);

/* Fetch the release's asset and check size + BLAKE2b. 0 ok. */
int uchan_fetch_asset(const char *channel, const UchanRelease *rel,
                      char **data, size_t *len, char *why, size_t cap);

/* Change stamp of a local channel's manifest (inode + ns mtime: publish
 * renames a fresh file over it, so every publish changes it even within one
 * second). -1 if not local or absent. */
int uchan_manifest_stamp(const char *channel, unsigned long long *out);

void uchan_release_free(UchanRelease *r);

/* `cclaw publish` — keygen, publish, show. */
int publish_main(int argc, char *argv[]);

#endif
