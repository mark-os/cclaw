CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g
VENDOR_CFLAGS = -std=c11 -Wall -Wextra -g

BUILD = build
VENDOR_OBJS = $(BUILD)/cJSON.o $(BUILD)/sqlite3.o

.PHONY: all test clean

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/cJSON.o: vendor/cJSON/cJSON.c | $(BUILD)
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

$(BUILD)/sqlite3.o: vendor/sqlite3/sqlite3.c | $(BUILD)
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

$(BUILD)/test_vendor: test/test_vendor.c $(VENDOR_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -Ivendor/cJSON -Ivendor/sqlite3 $< $(VENDOR_OBJS) -lpthread -ldl -lm -o $@

test: $(BUILD)/test_vendor
	./$(BUILD)/test_vendor

clean:
	rm -rf $(BUILD)
