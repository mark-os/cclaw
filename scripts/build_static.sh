#!/bin/sh
# Build a release binary: cclaw linked statically against musl, with a pinned
# libcurl + mbedTLS inside it. The result asks the host for nothing but kernel
# syscalls, so there is no glibc floor and one binary behaves the same on
# every distro (see specs/static-build.md).
#
#   scripts/build_static.sh <x64|arm64|i586|armv5te> [make-target...]
#
# Runs on an x86_64 host with the distro GCC for the target (gcc,
# gcc-aarch64-linux-gnu, gcc-i686-linux-gnu, gcc-arm-linux-gnueabi). musl is
# built from source and wrapped around that GCC with its own musl-gcc specs,
# so the only inputs are apt packages and three checksummed tarballs.
#
# Dependencies land in build-deps/<target>/ and are reused; the cclaw build
# itself goes through the ordinary Makefile into build/. Extra arguments are
# make targets (default: all), e.g. `build_static.sh x64 test-integration`.
set -eu

MUSL_VERSION=1.2.6
MUSL_SHA256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
MBEDTLS_VERSION=3.6.7
MBEDTLS_SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
CURL_VERSION=8.23.0
CURL_SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5

target=${1:-}
[ $# -gt 0 ] && shift
case "$target" in
    x64)     cross=;                   musl_arch=x86_64;  archflags= ;;
    arm64)   cross=aarch64-linux-gnu-; musl_arch=aarch64; archflags= ;;
    # i586, not the i686 Debian's toolchain defaults to: no CMOV/SSE, so VIA C3
    # and Geode thin clients can run it. musl and the deps get the same flags.
    # -fcf-protection=none: Ubuntu's GCC turns CET on by default, and its
    # ENDBR instructions are multi-byte NOPs an i586 does not have.
    i586)    cross=i686-linux-gnu-;    musl_arch=i386;    archflags="-march=i586 -mtune=generic -fcf-protection=none" ;;
    armv5te) cross=arm-linux-gnueabi-; musl_arch=arm;     archflags="-march=armv5te -mfloat-abi=soft" ;;
    *) echo "usage: $0 <x64|arm64|i586|armv5te> [make-target...]" >&2; exit 2 ;;
esac

root=$(cd "$(dirname "$0")/.." && pwd)
dl=$root/build-deps/dl
work=$root/build-deps/$target
prefix=$work/prefix
jobs=$(nproc 2>/dev/null || echo 2)
gcc=${cross}gcc
command -v "$gcc" >/dev/null || { echo "error: $gcc not found" >&2; exit 1; }
mkdir -p "$dl" "$work"

# fetch <url> <sha256> — download once, verify always. A tarball that does not
# match is deleted rather than left for the next run to trust.
fetch() {
    f=$dl/$(basename "$1")
    [ -f "$f" ] || curl -fsSL --retry 3 -o "$f" "$1"
    echo "$2  $f" | sha256sum -c --quiet - || { rm -f "$f"; exit 1; }
}

# unpack <tarball-basename> <dirname> — fresh tree every time a dep is built.
unpack() {
    rm -rf "$work/$2"
    tar -xf "$dl/$1" -C "$work"
}

# failed <name> — a dependency build keeps its output in a log; show the end
# of it only when something went wrong.
failed() {
    echo "error: building $1 failed — tail of $work/$1.log:" >&2
    tail -40 "$work/$1.log" >&2
    exit 1
}

# ── musl ─────────────────────────────────────────────────────────────
# --enable-wrapper=gcc installs bin/musl-gcc: the target GCC plus a specs file
# that swaps in musl's headers, crt files and libc. That wrapper is CC for
# everything below.
if [ ! -x "$prefix/bin/musl-gcc" ]; then
    fetch "https://musl.libc.org/releases/musl-$MUSL_VERSION.tar.gz" "$MUSL_SHA256"
    unpack "musl-$MUSL_VERSION.tar.gz" "musl-$MUSL_VERSION"
    (cd "$work/musl-$MUSL_VERSION" &&
     ./configure --prefix="$prefix" --target="$musl_arch" --disable-shared \
                 --enable-wrapper=gcc CC="$gcc" CFLAGS="-O2 $archflags" &&
     make -j"$jobs" AR="${cross}ar" RANLIB="${cross}ranlib" &&
     make install AR="${cross}ar" RANLIB="${cross}ranlib"
    ) >"$work/musl.log" 2>&1 || failed musl
fi
cc="$prefix/bin/musl-gcc $archflags"

# ── mbedTLS ──────────────────────────────────────────────────────────
if [ ! -f "$prefix/lib/libmbedtls.a" ]; then
    fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2" "$MBEDTLS_SHA256"
    unpack "mbedtls-$MBEDTLS_VERSION.tar.bz2" "mbedtls-$MBEDTLS_VERSION"
    (cd "$work/mbedtls-$MBEDTLS_VERSION" &&
     make -j"$jobs" -C library CC="$cc" AR="${cross}ar" CFLAGS="-O2" \
          libmbedcrypto.a libmbedx509.a libmbedtls.a &&
     cp -R include/mbedtls include/psa "$prefix/include/" &&
     cp library/libmbedcrypto.a library/libmbedx509.a library/libmbedtls.a "$prefix/lib/"
    ) >"$work/mbedtls.log" 2>&1 || failed mbedtls
fi

# ── libcurl ──────────────────────────────────────────────────────────
# HTTP(S), WebSocket and file:// (pre-release update channels) only. No CA
# path is baked in — it would describe this build machine; util_ca_bundle()
# finds the host's bundle at runtime.
if [ ! -f "$prefix/lib/libcurl.a" ]; then
    fetch "https://curl.se/download/curl-$CURL_VERSION.tar.xz" "$CURL_SHA256"
    unpack "curl-$CURL_VERSION.tar.xz" "curl-$CURL_VERSION"
    (cd "$work/curl-$CURL_VERSION" &&
     ./configure --host="$("$gcc" -dumpmachine)" --prefix="$prefix" \
        CC="$cc" AR="${cross}ar" RANLIB="${cross}ranlib" CFLAGS="-O2" \
        --disable-shared --enable-static --with-mbedtls="$prefix" \
        --enable-websockets --without-ca-bundle --without-ca-path \
        --disable-ftp --disable-ldap --disable-ldaps --disable-rtsp \
        --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
        --disable-imap --disable-smb --disable-smtp --disable-gopher \
        --disable-mqtt --disable-ipfs --disable-manual --disable-docs \
        --disable-ntlm --disable-unix-sockets --disable-alt-svc --disable-hsts \
        --without-zlib --without-brotli --without-zstd --without-libpsl \
        --without-libidn2 --without-nghttp2 --without-libssh2 &&
     make -j"$jobs" -C lib && make -C lib install && make -C include install
    ) >"$work/curl.log" 2>&1 || failed curl
fi

# ── cclaw ────────────────────────────────────────────────────────────
# The two embedded shims are the exception to "everything is musl":
#  - net_shim runs inside the sandbox on its own, so it is static too.
#  - libcclaw_net.so is LD_PRELOADed into the host's programs and must speak
#    the host's libc, so it is built by the plain (glibc) target compiler.
cd "$root"

make -j"$jobs" "${@:-all}" \
    CC="$cc" PRELOAD_CC="$gcc $archflags" NET_SHIM_LDFLAGS=-static \
    EXTRA_CFLAGS="-I$prefix/include" \
    LDFLAGS="-static -L$prefix/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lm -lpthread"

# The property the whole exercise is for — check it, do not assume it.
if "${cross}readelf" -lW build/cclaw | grep -Eq 'INTERP|DYNAMIC'; then
    echo "error: build/cclaw is not fully static" >&2
    exit 1
fi
echo "static build ok: build/cclaw ($target, $(wc -c < build/cclaw) bytes unstripped)"
