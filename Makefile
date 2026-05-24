CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Iinclude -Ivendor/cJSON -Ivendor/sqlite3 -Ivendor/civetweb -Ivendor/mquickjs
LDFLAGS := -lcurl -lm -lpthread -ldl

BUILDDIR := build
SRC      := $(wildcard src/*.c)
OBJ      := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRC))

VENDOR_SRC := vendor/cJSON/cJSON.c vendor/sqlite3/sqlite3.c vendor/civetweb/civetweb.c \
              vendor/mquickjs/mquickjs.c vendor/mquickjs/cutils.c vendor/mquickjs/dtoa.c \
              vendor/mquickjs/libm.c vendor/mquickjs/mquickjs_stdlib.c
VENDOR_OBJ := $(BUILDDIR)/cJSON.o $(BUILDDIR)/sqlite3.o $(BUILDDIR)/civetweb.o \
              $(BUILDDIR)/mquickjs.o $(BUILDDIR)/mqjs_cutils.o $(BUILDDIR)/mqjs_dtoa.o \
              $(BUILDDIR)/mqjs_libm.o $(BUILDDIR)/mqjs_stdlib.o

INTEG_SRC := $(wildcard test/test_integration_*.c)
TEST_SRC  := $(filter-out $(INTEG_SRC),$(wildcard test/test_*.c))
TEST_BIN  := $(patsubst test/%.c,$(BUILDDIR)/%,$(TEST_SRC))
INTEG_BIN := $(patsubst test/%.c,$(BUILDDIR)/%,$(INTEG_SRC))

.PHONY: all build clean test test-integration test-all

all: $(BUILDDIR)/cclaw
build: all

$(BUILDDIR)/cclaw: $(OBJ) $(VENDOR_OBJ) | $(BUILDDIR)/
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)/
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/cJSON.o: vendor/cJSON/cJSON.c | $(BUILDDIR)/
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

$(BUILDDIR)/sqlite3.o: vendor/sqlite3/sqlite3.c | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c -o $@ $<

$(BUILDDIR)/civetweb.o: vendor/civetweb/civetweb.c | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -DNO_SSL -DNO_CGI -DUSE_IPV6 -DNO_CACHING -Ivendor/civetweb -Wno-unused-parameter -c -o $@ $<

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

vendor/mquickjs/mquickjs_stdlib.c: vendor/mquickjs/gen_stdlib.c vendor/mquickjs/mquickjs_build.c vendor/mquickjs/cutils.c vendor/mquickjs/mqjs_host.c | $(BUILDDIR)/
	$(CC) -Ivendor/mquickjs -o $(BUILDDIR)/gen_stdlib vendor/mquickjs/gen_stdlib.c vendor/mquickjs/mquickjs_build.c vendor/mquickjs/cutils.c -lm
	printf '#define _POSIX_C_SOURCE 199309L\n#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n#include <math.h>\n#include <time.h>\n#include "mquickjs_priv.h"\n\n' > $@
	cat vendor/mquickjs/mqjs_host.c >> $@
	./$(BUILDDIR)/gen_stdlib -m64 | sed '1,/^#include "mquickjs_priv.h"/d' >> $@

$(BUILDDIR)/:
	mkdir -p $(BUILDDIR)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "--- $$t ---"; ./$$t || exit 1; done

test-integration: $(INTEG_BIN)
	@for t in $(INTEG_BIN); do echo "--- $$t ---"; ./$$t || exit 1; done

test-all: test test-integration

LIB_OBJ := $(filter-out $(BUILDDIR)/main.o,$(OBJ))
$(BUILDDIR)/libcclaw.a: $(LIB_OBJ) $(VENDOR_OBJ) | $(BUILDDIR)/
	$(AR) rcs $@ $^

$(BUILDDIR)/test_%: test/test_%.c $(BUILDDIR)/libcclaw.a | $(BUILDDIR)/
	$(CC) $(CFLAGS) -o $@ $< $(BUILDDIR)/libcclaw.a $(LDFLAGS)

clean:
	rm -rf $(BUILDDIR)
