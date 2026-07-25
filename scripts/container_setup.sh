#!/bin/sh
# Install the two build deps a bare cloud container is missing, then verify.
#
# CClaw vendors everything except libcurl, so a fresh Debian/Ubuntu image needs
# exactly two packages:
#   libcurl4-openssl-dev  — curl/curl.h (src/channel_runner.c and friends)
#   xxd                   — Makefile:143/154 embed the preload + net_shim blobs
#
# Idempotent: re-running is a no-op once both are present. Safe to source into
# a session-start script or run by hand.

set -e

cd "$(dirname "$0")/.."

need=""
# The header lands in /usr/include/<triplet>/curl on multiarch, so ask the
# compiler rather than guessing the path.
echo '#include <curl/curl.h>' | cc -fsyntax-only -xc - 2>/dev/null \
    || need="$need libcurl4-openssl-dev"
command -v xxd >/dev/null 2>&1 || need="$need xxd"

if [ -n "$need" ]; then
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "container_setup: missing:$need — install them with your package manager" >&2
        exit 1
    fi
    echo "container_setup: installing$need"
    # A stale index 404s on the current package version, so always refresh.
    apt-get update -qq
    # shellcheck disable=SC2086
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $need
else
    echo "container_setup: deps already present"
fi

echo "container_setup: building"
make -s -j"$(nproc)"

echo "container_setup: smoke test"
make -s smoke

cat <<'EOF'

container_setup: ready.
  make test              unit suite
  make test-integration  mock-server suite

Live LLM calls need a key and a reachable provider. In a locked-down
container most provider hosts are blocked at the egress proxy; check with
  curl -o /dev/null -w '%{http_code}\n' https://<provider-host>/
A 000 means the proxy refused CONNECT, so `make test-e2e` cannot run there.
EOF
