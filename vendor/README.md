# Vendored Dependencies

## SQLite 3.53.1

- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3530100.zip
- Docs: https://www.sqlite.org/
- License: Public domain

## Civetweb 1.16

- Repo: https://github.com/civetweb/civetweb
- Files: `civetweb.c`, `civetweb.h`, plus `*.inl` includes
- Built with: `-DNO_SSL -DNO_CGI -DUSE_IPV6 -DNO_CACHING`
- License: MIT

## QuickJS (2026-06-04)

- Repo: https://github.com/bellard/quickjs
- Files: `quickjs.c`, `quickjs.h`, `quickjs-atom.h`, `quickjs-opcode.h`,
  `quickjs-libc.c`, `quickjs-libc.h`, `cutils.c`, `cutils.h`, `dtoa.c`, `dtoa.h`,
  `list.h`, `libunicode.c`, `libunicode.h`, `libunicode-table.h`,
  `libregexp.c`, `libregexp.h`, `libregexp-opcode.h`
- Built with: `-std=gnu11 -D_GNU_SOURCE -DCONFIG_VERSION`
- License: MIT
- Arena-based slab allocator for small blocks (16–512 bytes), ES2025 support

## Monocypher 4.0.2

- Source: https://monocypher.org
- Files: `monocypher.c`, `monocypher.h`
- License: BSD-2-Clause OR CC0-1.0
