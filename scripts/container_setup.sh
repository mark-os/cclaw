#!/bin/sh
# Install the two build deps a bare cloud container is missing, then verify.
#
# CClaw vendors everything except libcurl, so a fresh Debian/Ubuntu image needs
# exactly two packages:
#   libcurl4-openssl-dev  — curl/curl.h (src/channel_runner.c and friends)
#   xxd                   — Makefile:143/154 embed the preload + net_shim blobs
#
# Those two get you a full build and every suite except the WebSocket ones.
# Set CCLAW_SETUP_WS=1 to also build a WS-capable libcurl (see below); either
# way the closing summary says which of the two the container ended up with,
# because a suite that skips still reports green.
#
# Idempotent: re-running is a no-op once both are present. Safe to source into
# a session-start script or run by hand.

set -e

cd "$(dirname "$0")/.."

# curl release built when CCLAW_SETUP_WS=1. Must be >= 8.13.0 (see below).
WS_CURL_VERSION=8.21.0
WS_CURL_PREFIX=/opt/curlws

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

# Can the runtime libcurl carry the conn.* WebSocket transport? Two conditions,
# both of which every distro package fails:
#   - ws/wss handlers present (libcurl-minimal and Debian/Ubuntu strip them), and
#   - >= 8.13.0. Older libcurl derived the frame flags from the opcode alone and
#     never surfaced FIN, so nothing in the API says where a fragmented message
#     ends — cclaw refuses that contract rather than truncate half the traffic.
# curl-config reports the same library the binary links against, so it answers
# both. --vernum is fixed-width hex; 080d00 is 8.13.0. $1 is the curl-config to
# interrogate, defaulting to the one on PATH — always ask the build itself
# rather than infer capability from a prefix existing, so a stale or
# hand-rolled $WS_CURL_PREFIX is judged on what it actually is.
ws_capable() {
    cc=${1:-curl-config}
    command -v "$cc" >/dev/null 2>&1 || return 1
    "$cc" --protocols 2>/dev/null | grep -qi '^WSS\{0,1\}$' || return 1
    [ "$(printf '%d' "0x$("$cc" --vernum 2>/dev/null)" 2>/dev/null || echo 0)" \
        -ge "$(printf '%d' 0x080d00)" ]
}

if [ "$CCLAW_SETUP_WS" = "1" ] && ws_capable; then
    echo "container_setup: system libcurl already carries the WS transport"
elif [ "$CCLAW_SETUP_WS" = "1" ] && ws_capable "$WS_CURL_PREFIX/bin/curl-config"; then
    echo "container_setup: WS libcurl already built at $WS_CURL_PREFIX"
elif [ "$CCLAW_SETUP_WS" = "1" ]; then
    echo "container_setup: building libcurl $WS_CURL_VERSION with WebSockets"
    # --enable-websockets is off by default in curl's own build too, so this is
    # a source build regardless of distro. Installed alongside the system
    # libcurl rather than over it: same soname, so LD_LIBRARY_PATH selects it
    # per-command and nothing else on the box changes.
    tmp=$(mktemp -d)
    curl -sSLo "$tmp/curl.tar.gz" \
        "https://curl.se/download/curl-$WS_CURL_VERSION.tar.gz"
    tar xzf "$tmp/curl.tar.gz" -C "$tmp"
    (cd "$tmp/curl-$WS_CURL_VERSION" && \
        ./configure --prefix="$WS_CURL_PREFIX" --with-openssl --enable-websockets \
            --disable-ldap --disable-ldaps --disable-manual --without-libpsl \
            --without-brotli --without-zstd --without-libidn2 \
            --without-nghttp2 --without-libssh2 >/dev/null && \
        make -s -j"$(nproc)" >/dev/null && make -s install >/dev/null)
    rm -rf "$tmp"
fi

echo "container_setup: building"
make -s -j"$(nproc)"

echo "container_setup: smoke test"
make -s smoke

cat <<'EOF'

container_setup: ready.
  make test              unit suite
  make test-integration  mock-server suite
EOF

# Say plainly whether the WebSocket suites will run. They SKIP on a libcurl
# that cannot carry the transport, and a skipped suite still exits 0 — so a
# green `make test-integration` is not by itself evidence that the Discord
# gateway and conn.* paths were exercised.
if ws_capable "$WS_CURL_PREFIX/bin/curl-config"; then
    cat <<EOF
  WS transport: available at $WS_CURL_PREFIX. The conn.* and Discord gateway
  suites only run against it:
    LD_LIBRARY_PATH=$WS_CURL_PREFIX/lib make test-integration
EOF
elif ws_capable; then
    echo "  WS transport: system libcurl can carry it — all suites run."
else
    cat <<EOF
  WS transport: NOT available. test_integration_channel_conn and
  test_integration_channel_discord will SKIP (green, but guarding nothing).
  Re-run as CCLAW_SETUP_WS=1 $0 to build libcurl $WS_CURL_VERSION into
  $WS_CURL_PREFIX, then run those suites with
    LD_LIBRARY_PATH=$WS_CURL_PREFIX/lib make test-integration
EOF
fi

cat <<'EOF'

Live LLM calls need a key and a reachable provider. In a locked-down
container most provider hosts are blocked at the egress proxy; check with
  curl -o /dev/null -w '%{http_code}\n' https://<provider-host>/
A 000 means the proxy refused CONNECT, so `make test-e2e` cannot run there.
EOF
