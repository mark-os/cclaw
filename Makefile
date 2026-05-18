CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g
VENDOR_CFLAGS = -std=c11 -Wall -Wextra -g
LDFLAGS = -lcurl -lpthread -ldl -lm

BUILD = build
SRC = $(wildcard src/*.c)
SRC_OBJS = $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
VENDOR_OBJS = $(BUILD)/cJSON.o $(BUILD)/sqlite3.o

.PHONY: all test clean

all: $(BUILD)/cclaw

$(BUILD):
	mkdir -p $(BUILD)

# Vendor objects (no -Werror, upstream code)
$(BUILD)/cJSON.o: vendor/cJSON/cJSON.c | $(BUILD)
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

$(BUILD)/sqlite3.o: vendor/sqlite3/sqlite3.c | $(BUILD)
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

# Source objects
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON -Ivendor/sqlite3 -c $< -o $@

# Main binary
$(BUILD)/cclaw: $(SRC_OBJS) $(VENDOR_OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

# Tests
$(BUILD)/test_vendor: test/test_vendor.c $(VENDOR_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -Ivendor/cJSON -Ivendor/sqlite3 $< $(VENDOR_OBJS) $(LDFLAGS) -o $@

test: $(BUILD)/test_vendor
	./$(BUILD)/test_vendor

clean:
	rm -rf $(BUILD)
