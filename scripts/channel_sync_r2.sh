#!/bin/bash
# Mirror a local `cclaw publish` channel to an R2 bucket for https followers.
#   scripts/channel_sync_r2.sh /var/lib/cclaw/channel my-bucket[/prefix]
# Uploads the binary the manifest names first, then manifest.signed, so a
# follower never sees a manifest pointing at a missing object. Followers set
# update.channel to the bucket's public https URL (custom domain or r2.dev).
set -euo pipefail
dir=${1:?channel dir}; dest=${2:?bucket[/prefix]}
manifest="$dir/manifest.signed"
[ -f "$manifest" ] || { echo "no $manifest" >&2; exit 1; }
files=$(tail -n +2 "$manifest" | grep -o '"file":"[^"]*"' | cut -d'"' -f4)
for f in $files; do
  npx --yes wrangler r2 object put "$dest/$f" --file "$dir/$f" --remote \
    --content-type application/octet-stream
done
npx --yes wrangler r2 object put "$dest/manifest.signed" --file "$manifest" --remote \
  --content-type text/plain --cache-control no-cache
echo "synced $(echo $files | wc -w) asset(s) + manifest to r2://$dest"
