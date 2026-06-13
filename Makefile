CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Iinclude -Ivendor/sqlite3 -Ivendor/civetweb -Ivendor/mquickjs -Ivendor/monocypher -Ivendor/jsmn
LDFLAGS := -lcurl -lm -lpthread -ldl

BUILDDIR := build
TEMPLATES := $(wildcard templates/*)
SRC      := $(filter-out src/mjs_main.c src/preload_net.c src/channel_runner.c,$(wildcard src/*.c))
OBJ      := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)

VENDOR_SRC := vendor/sqlite3/sqlite3.c vendor/civetweb/civetweb.c \
              vendor/mquickjs/mquickjs.c vendor/mquickjs/cutils.c vendor/mquickjs/dtoa.c \
              vendor/mquickjs/libm.c vendor/mquickjs/mquickjs_stdlib.c \
              vendor/monocypher/monocypher.c
VENDOR_OBJ := $(BUILDDIR)/sqlite3.o $(BUILDDIR)/civetweb.o \
              $(BUILDDIR)/mquickjs.o $(BUILDDIR)/mqjs_cutils.o $(BUILDDIR)/mqjs_dtoa.o \
              $(BUILDDIR)/mqjs_libm.o $(BUILDDIR)/mqjs_stdlib.o $(BUILDDIR)/monocypher.o

INTEG_SRC := $(wildcard test/test_integration_*.c)
E2E_SRC   := $(wildcard test/test_e2e_*.c)
TEST_SRC  := $(filter-out $(INTEG_SRC) $(E2E_SRC),$(wildcard test/test_*.c))
TEST_BIN  := $(patsubst test/%.c,$(BUILDDIR)/%,$(TEST_SRC))
INTEG_BIN := $(patsubst test/%.c,$(BUILDDIR)/%,$(INTEG_SRC))
E2E_BIN   := $(patsubst test/%.c,$(BUILDDIR)/%,$(E2E_SRC))

# mquickjs core objects are shared by cclaw, mjs, and channel_runner — only
# the generated stdlib (which embeds a per-binary host file) differs.
MQJS_CORE_OBJ := $(BUILDDIR)/mquickjs.o $(BUILDDIR)/mqjs_cutils.o $(BUILDDIR)/mqjs_dtoa.o \
                 $(BUILDDIR)/mqjs_libm.o

.PHONY: all clean test test-integration test-e2e test-all check-gen install debug

all: $(BUILDDIR)/cclaw $(BUILDDIR)/mjs $(BUILDDIR)/libcclaw_net.so $(BUILDDIR)/channel_runner

# Development build: debug symbols, no optimization, sanitizers (clang preferred for better traces)
debug: clean
	$(MAKE) all CC=clang \
	            CFLAGS="$(CFLAGS) -O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined" \
	            LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

$(BUILDDIR)/cclaw: $(OBJ) $(VENDOR_OBJ) | $(BUILDDIR)/
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/templates.h: $(TEMPLATES) scripts/gen_templates.sh | $(BUILDDIR)/
	./scripts/gen_templates.sh $@ templates

$(BUILDDIR)/%.o: src/%.c $(BUILDDIR)/templates.h | $(BUILDDIR)/
	$(CC) $(CFLAGS) -I$(BUILDDIR) -MMD -MP -c -o $@ $<

$(BUILDDIR)/tool_shell.o: $(BUILDDIR)/preload_blob.h

$(BUILDDIR)/sqlite3.o: vendor/sqlite3/sqlite3.c | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c -o $@ $<

$(BUILDDIR)/civetweb.o: vendor/civetweb/civetweb.c | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -DNO_SSL -DNO_CGI -DUSE_IPV6 -DNO_CACHING -Ivendor/civetweb -Wno-unused-parameter -c -o $@ $<

$(BUILDDIR)/monocypher.o: vendor/monocypher/monocypher.c | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -c -o $@ $<

MQJS_CFLAGS := -std=c11 -O2 -Ivendor/mquickjs -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable

$(BUILDDIR)/mquickjs.o: vendor/mquickjs/mquickjs.c vendor/mquickjs/mquickjs_atom.h | $(BUILDDIR)/
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/mqjs_cutils.o: vendor/mquickjs/cutils.c | $(BUILDDIR)/
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/mqjs_dtoa.o: vendor/mquickjs/dtoa.c | $(BUILDDIR)/
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/mqjs_libm.o: vendor/mquickjs/libm.c | $(BUILDDIR)/
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/mqjs_stdlib.o: vendor/mquickjs/mquickjs_stdlib.c | $(BUILDDIR)/
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

vendor/mquickjs/mquickjs_atom.h: vendor/mquickjs/gen_atoms.c | $(BUILDDIR)/
	$(CC) -o $(BUILDDIR)/gen_atoms $<
	./$(BUILDDIR)/gen_atoms > $@

vendor/mquickjs/mquickjs_stdlib.c: vendor/mquickjs/gen_stdlib.c vendor/mquickjs/mquickjs_build.c vendor/mquickjs/cutils.c vendor/mquickjs/mqjs_host.c $(BUILDDIR)/gen_stdlib | $(BUILDDIR)/
	printf '#define _POSIX_C_SOURCE 199309L\n#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n#include <math.h>\n#include <time.h>\n#include "mquickjs_priv.h"\n\n' > $@
	cat vendor/mquickjs/mqjs_host.c >> $@
	./$(BUILDDIR)/gen_stdlib -m64 | sed '1,/^#include "mquickjs_priv.h"/d' >> $@

$(BUILDDIR)/gen_stdlib: vendor/mquickjs/gen_stdlib.c vendor/mquickjs/mquickjs_build.c vendor/mquickjs/cutils.c | $(BUILDDIR)/
	$(CC) -Ivendor/mquickjs -o $@ vendor/mquickjs/gen_stdlib.c vendor/mquickjs/mquickjs_build.c vendor/mquickjs/cutils.c -lm

# mjs standalone stdlib (uses mqjs_host_mjs.c instead of mqjs_host.c)
$(BUILDDIR)/mquickjs_stdlib_mjs.c: vendor/mquickjs/mqjs_host_mjs.c $(BUILDDIR)/gen_stdlib | $(BUILDDIR)/
	printf '#define _POSIX_C_SOURCE 199309L\n#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n#include <math.h>\n#include <time.h>\n#include <unistd.h>\n#include "mquickjs_priv.h"\n\n' > $@
	cat vendor/mquickjs/mqjs_host_mjs.c >> $@
	./$(BUILDDIR)/gen_stdlib -m64 | sed '1,/^#include "mquickjs_priv.h"/d' >> $@

$(BUILDDIR)/mjs_stdlib.o: $(BUILDDIR)/mquickjs_stdlib_mjs.c | $(BUILDDIR)/
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/mjs_main.o: src/mjs_main.c | $(BUILDDIR)/
	$(CC) -std=c11 -Wall -Wextra -Werror -Ivendor/mquickjs -c -o $@ $<

$(BUILDDIR)/mjs: $(BUILDDIR)/mjs_main.o $(MQJS_CORE_OBJ) $(BUILDDIR)/mjs_stdlib.o | $(BUILDDIR)/
	$(CC) -std=c11 -o $@ $^ -lm

$(BUILDDIR)/libcclaw_net.so: src/preload_net.c | $(BUILDDIR)/
	$(CC) -std=c11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -ldl

$(BUILDDIR)/preload_blob.h: $(BUILDDIR)/libcclaw_net.so
	printf 'static const unsigned char preload_net_blob[] = {\n' > $@
	xxd -i < $< >> $@
	printf '};\nstatic const unsigned int preload_net_blob_len = sizeof(preload_net_blob);\n' >> $@

# Channel runner: universal JS channel binary
$(BUILDDIR)/mquickjs_stdlib_channel.c: vendor/mquickjs/mqjs_host_channel.c $(BUILDDIR)/gen_stdlib | $(BUILDDIR)/
	printf '#define _POSIX_C_SOURCE 199309L\n#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n#include <math.h>\n#include <time.h>\n#include "mquickjs_priv.h"\n\n' > $@
	cat vendor/mquickjs/mqjs_host_channel.c >> $@
	./$(BUILDDIR)/gen_stdlib -m64 | sed '1,/^#include "mquickjs_priv.h"/d' >> $@

$(BUILDDIR)/cr_stdlib.o: $(BUILDDIR)/mquickjs_stdlib_channel.c | $(BUILDDIR)/
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

CR_LIB_OBJ := $(BUILDDIR)/admin_api.o $(BUILDDIR)/agent_config.o $(BUILDDIR)/channel_api.o \
              $(BUILDDIR)/db.o $(BUILDDIR)/wake.o $(BUILDDIR)/secret.o $(BUILDDIR)/secret_scan.o $(BUILDDIR)/config.o \
              $(BUILDDIR)/sqlite3.o $(BUILDDIR)/monocypher.o

$(BUILDDIR)/channel_runner: $(BUILDDIR)/channel_runner.o $(MQJS_CORE_OBJ) $(BUILDDIR)/cr_stdlib.o $(CR_LIB_OBJ) | $(BUILDDIR)/
	$(CC) $(CFLAGS) -o $@ $^ -lcurl -lm -lpthread -ldl

# Everything a production box needs: the daemon binary, the mjs evaluator
# (system prompts reference /usr/local/lib/cclaw/mjs from sandboxed shells),
# channel_runner (the --channel branch resolves it relative to the cclaw
# binary: sibling dir, then ../lib/cclaw/), env file, and the systemd unit.
install: $(BUILDDIR)/cclaw $(BUILDDIR)/mjs $(BUILDDIR)/channel_runner
	install -d /usr/local/bin /usr/local/lib/cclaw /etc/cclaw
	install -m 755 $(BUILDDIR)/cclaw /usr/local/bin/cclaw
	install -m 755 $(BUILDDIR)/mjs /usr/local/lib/cclaw/mjs
	install -m 755 $(BUILDDIR)/channel_runner /usr/local/lib/cclaw/channel_runner
	test -f /etc/cclaw/env || install -m 600 cclaw.env.example /etc/cclaw/env
	install -m 644 cclaw.service /etc/systemd/system/cclaw.service
	systemctl daemon-reload
	@echo "Installed. Edit /etc/cclaw/env then: systemctl enable --now cclaw"

$(BUILDDIR)/:
	mkdir -p $(BUILDDIR)

# Each test binary's output goes to a /tmp file, never to make's stdout.
# This keeps `make test | tail/grep/head` safe: leaked mock-server children
# hold the file fd, not the pipe, so readers always see EOF; and only make's
# own one-liners can take a SIGPIPE.
test: $(TEST_BIN)
	@fail=0; for t in $(TEST_BIN); do \
		out="/tmp/cclaw_$$(basename $$t).txt"; \
		timeout 20 ./$$t > $$out 2>&1; rc=$$?; \
		if [ $$rc -eq 0 ]; then echo "PASS $$t (→ $$out)"; \
		elif [ $$rc -eq 124 ]; then echo "TIMEOUT $$t (killed after 20s → $$out)"; fail=1; \
		else echo "FAIL $$t (exit $$rc → $$out)"; tail -20 $$out | sed 's/^/  | /'; fail=1; fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "All unit tests passed ($(words $(TEST_BIN)) suites)"; fi; \
	exit $$fail

test-integration: $(INTEG_BIN)
	@fail=0; for t in $(INTEG_BIN); do \
		out="/tmp/cclaw_$$(basename $$t).txt"; \
		timeout 45 ./$$t > $$out 2>&1; rc=$$?; \
		lines=$$(wc -l < $$out); \
		if [ $$rc -eq 0 ]; then echo "PASS $$t ($$lines lines → $$out)"; \
		elif [ $$rc -eq 124 ]; then echo "TIMEOUT $$t (killed after 45s, $$lines lines → $$out)"; fail=1; \
		else echo "FAIL $$t (exit $$rc, $$lines lines → $$out)"; tail -20 $$out | sed 's/^/  | /'; fail=1; fi; \
	done; exit $$fail

test-e2e: $(E2E_BIN)
	@fail=0; for t in $(E2E_BIN); do \
		out="/tmp/cclaw_$$(basename $$t).txt"; \
		timeout 120 ./$$t > $$out 2>&1; rc=$$?; \
		if [ $$rc -eq 0 ]; then echo "PASS $$t (→ $$out)"; \
		elif [ $$rc -eq 124 ]; then echo "TIMEOUT $$t (killed after 120s → $$out)"; fail=1; \
		else echo "FAIL $$t (exit $$rc → $$out)"; tail -20 $$out | sed 's/^/  | /'; fail=1; fi; \
	done; exit $$fail

# Verify the checked-in secret-scan headers match the generator output.
# Dev-only (needs python3) — skipped silently when python3 is absent, so
# embedded builds never grow a python dependency.
check-gen:
	@command -v python3 >/dev/null 2>&1 || { echo "check-gen: skipped (no python3)"; exit 0; }; \
	cp include/secret_scan_ac.h /tmp/cclaw_checkgen_ac.h; \
	cp include/secret_scan_rules.h /tmp/cclaw_checkgen_rules.h; \
	python3 scripts/gen_secret_scan.py >/dev/null 2>&1; \
	if cmp -s include/secret_scan_ac.h /tmp/cclaw_checkgen_ac.h && \
	   cmp -s include/secret_scan_rules.h /tmp/cclaw_checkgen_rules.h; then \
		echo "check-gen: secret-scan headers in sync"; \
	else \
		cp /tmp/cclaw_checkgen_ac.h include/secret_scan_ac.h; \
		cp /tmp/cclaw_checkgen_rules.h include/secret_scan_rules.h; \
		echo "check-gen: STALE — rerun 'python3 scripts/gen_secret_scan.py' and commit"; \
		exit 1; \
	fi

test-all: test test-integration check-gen

LIB_OBJ := $(filter-out $(BUILDDIR)/main.o,$(OBJ))
$(BUILDDIR)/libcclaw.a: $(LIB_OBJ) $(VENDOR_OBJ) | $(BUILDDIR)/
	$(AR) rcs $@ $^

$(BUILDDIR)/mock_server.o: test/mock_server.c | $(BUILDDIR)/
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/libtest.a: $(BUILDDIR)/mock_server.o
	$(AR) rcs $@ $^

$(BUILDDIR)/test_%: test/test_%.c $(BUILDDIR)/libcclaw.a $(BUILDDIR)/libtest.a | $(BUILDDIR)/
	$(CC) $(CFLAGS) -I$(BUILDDIR) -o $@ $< $(BUILDDIR)/libtest.a $(BUILDDIR)/libcclaw.a $(LDFLAGS)

compile_commands.json: $(BUILDDIR)/templates.h
	@echo "[" > $@
	@first=1; for f in $(SRC); do \
		[ $$first -eq 0 ] && printf ",\n" >> $@; first=0; \
		printf '  {"directory":"%s","file":"%s","command":"%s %s -I%s -c %s"}' \
			"$(CURDIR)" "$$f" "$(CC)" "$(CFLAGS)" "$(BUILDDIR)" "$$f" >> $@; \
	done
	@printf "\n]\n" >> $@
	@echo "Generated $@ ($(words $(SRC)) entries)"

clean:
	rm -rf $(BUILDDIR)

-include $(DEP)
