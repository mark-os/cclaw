# Vendored Dependencies

## cJSON 1.7.19

- Source: https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/cJSON.c
- Source: https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/cJSON.h
- Repo: https://github.com/DaveGamble/cJSON
- License: MIT

## SQLite 3.53.1

- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3530100.zip
- Docs: https://www.sqlite.org/
- License: Public domain

## Civetweb 1.16

- Repo: https://github.com/civetweb/civetweb
- Files: `civetweb.c`, `civetweb.h`, plus `*.inl` includes
- Built with: `-DNO_SSL -DNO_CGI -DUSE_IPV6 -DNO_CACHING`
- License: MIT

## MicroQuickJS

- Repo: https://github.com/nicholasgasior/mquickjs (fork of QuickJS by Bellard)
- Files: Full source tree (mquickjs.c, build system, stdlib generator)
- Built with: gen_atoms → mquickjs_atom.h, gen_stdlib → mquickjs_stdlib.c
- License: MIT
- **Custom modifications:**
  - `mqjs_host.c` — CClaw host functions (http_fetch with allowed_hosts + SSRF protection)
  - stdlib upgraded with `Date.now` support

## Monocypher 4.0.2

- Source: https://monocypher.org
- Files: `monocypher.c`, `monocypher.h`
- License: BSD-2-Clause OR CC0-1.0
