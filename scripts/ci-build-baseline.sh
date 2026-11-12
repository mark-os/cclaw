#!/bin/bash
# Build the release binary inside debian:bullseye (glibc 2.31) so it runs on
# any glibc >= 2.31: Debian 11+, Ubuntu 20.04+, RHEL/Rocky/Alma 9, Amazon
# Linux 2023. A binary built on a newer distro picks up newer glibc symbol
# versions (e.g. glibc 2.38 redirects strtol/sscanf to __isoc23_*) and then
# refuses to start on older systems.
#
# Bullseye's own libcurl (7.74) predates curl_ws_send/curl_ws_recv (7.86), so
# build a current libcurl into /usr/local and compile against that. The binary
# still links the libcurl.so.4 soname and uses whatever libcurl the target has
# at runtime; curl_ws_available() gates the WebSocket transport on the
# runtime version. This libcurl only serves the build, so it is built without
# TLS (current curl needs OpenSSL 3; bullseye has 1.1.1).
#
# Usage (from the repo root, in the container): scripts/ci-build-baseline.sh
# Expects curl-$CURL_VERSION.tar.gz in the repo root (downloaded by the host).
set -euo pipefail

: "${CURL_VERSION:?set CURL_VERSION}"

export DEBIAN_FRONTEND=noninteractive
# Bullseye is past end of life and its packages now live on
# archive.debian.org; deb.debian.org has already pruned bullseye-security.
rm -f /etc/apt/sources.list.d/*
cat > /etc/apt/sources.list <<'SRC'
deb http://archive.debian.org/debian bullseye main
deb http://archive.debian.org/debian bullseye-updates main
deb http://archive.debian.org/debian-security bullseye-security main
SRC
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential xxd git ca-certificates >/dev/null

tmp=$(mktemp -d)
tar xzf "curl-${CURL_VERSION}.tar.gz" -C "$tmp"
(
  cd "$tmp/curl-${CURL_VERSION}"
  ./configure --prefix=/usr/local --without-ssl --enable-websockets \
    --disable-static --disable-ldap --disable-ldaps --disable-manual \
    --without-libpsl --without-brotli --without-zstd --without-libidn2 \
    --without-nghttp2 --without-libssh2 >/dev/null
  make -j"$(nproc)" >/dev/null
  make install >/dev/null
)
ldconfig

git config --global --add safe.directory "$PWD"
make clean >/dev/null
make all
./build/cclaw --version
