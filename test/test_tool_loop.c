#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* V43: replicate tool loop detection logic from agent.c for unit testing */
#define TOOL_LOOP_RING_SIZE 30
#define TOOL_LOOP_WARN_THRESHOLD 5
#define TOOL_LOOP_BREAK_THRESHOLD 10

typedef struct {
    uint32_t hashes[TOOL_LOOP_RING_SIZE];
    int count;
} ToolLoopRing;

static uint32_t tool_call_hash(const char *name, const char *args) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    if (args) {
        for (const char *p = args; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 16777619u;
        }
    }
    return h;
}

static int tool_loop_streak(const ToolLoopRing *ring, uint32_t hash) {
    int n = ring->count < TOOL_LOOP_RING_SIZE ? ring->count : TOOL_LOOP_RING_SIZE;
    int tail = (ring->count - 1) % TOOL_LOOP_RING_SIZE;
    int streak = 0;
    for (int i = 0; i < n; i++) {
        int idx = tail - i;
        if (idx < 0) idx += TOOL_LOOP_RING_SIZE;
        if (ring->hashes[idx] == hash) streak++;
        else break;
    }
    return streak;
}

static void record(ToolLoopRing *ring, const char *name, const char *args) {
    uint32_t h = tool_call_hash(name, args);
    ring->hashes[ring->count % TOOL_LOOP_RING_SIZE] = h;
    ring->count++;
}

static void test_hash_deterministic(void) {
    TEST(hash_deterministic);
    uint32_t h1 = tool_call_hash("file_read", "{\"path\":\"/tmp/x\"}");
    uint32_t h2 = tool_call_hash("file_read", "{\"path\":\"/tmp/x\"}");
    if (h1 != h2) { FAIL("same input should produce same hash"); return; }
    PASS();
}

static void test_hash_differs_on_name(void) {
    TEST(hash_differs_on_name);
    uint32_t h1 = tool_call_hash("file_read", "{\"path\":\"/tmp/x\"}");
    uint32_t h2 = tool_call_hash("file_write", "{\"path\":\"/tmp/x\"}");
    if (h1 == h2) { FAIL("different names should differ"); return; }
    PASS();
}

static void test_hash_differs_on_args(void) {
    TEST(hash_differs_on_args);
    uint32_t h1 = tool_call_hash("file_read", "{\"path\":\"/tmp/a\"}");
    uint32_t h2 = tool_call_hash("file_read", "{\"path\":\"/tmp/b\"}");
    if (h1 == h2) { FAIL("different args should differ"); return; }
    PASS();
}

static void test_streak_zero_empty(void) {
    TEST(streak_zero_empty);
    ToolLoopRing ring = {.count = 0};
    uint32_t h = tool_call_hash("shell_exec", "{\"cmd\":\"ls\"}");
    int s = tool_loop_streak(&ring, h);
    if (s != 0) { FAIL("empty ring should have 0 streak"); return; }
    PASS();
}

static void test_streak_one(void) {
    TEST(streak_one);
    ToolLoopRing ring = {.count = 0};
    record(&ring, "shell_exec", "{\"cmd\":\"ls\"}");
    uint32_t h = tool_call_hash("shell_exec", "{\"cmd\":\"ls\"}");
    int s = tool_loop_streak(&ring, h);
    if (s != 1) { FAIL("expected streak 1"); return; }
    PASS();
}

static void test_streak_broken_by_different(void) {
    TEST(streak_broken_by_different);
    ToolLoopRing ring = {.count = 0};
    record(&ring, "shell_exec", "{\"cmd\":\"ls\"}");
    record(&ring, "shell_exec", "{\"cmd\":\"ls\"}");
    record(&ring, "file_read", "{\"path\":\"/x\"}");
    record(&ring, "shell_exec", "{\"cmd\":\"ls\"}");
    uint32_t h = tool_call_hash("shell_exec", "{\"cmd\":\"ls\"}");
    int s = tool_loop_streak(&ring, h);
    if (s != 1) { FAIL("streak should reset after different call"); return; }
    PASS();
}

static void test_streak_reaches_warn(void) {
    TEST(streak_reaches_warn);
    ToolLoopRing ring = {.count = 0};
    for (int i = 0; i < TOOL_LOOP_WARN_THRESHOLD; i++)
        record(&ring, "file_read", "{\"path\":\"/same\"}");
    uint32_t h = tool_call_hash("file_read", "{\"path\":\"/same\"}");
    int s = tool_loop_streak(&ring, h);
    if (s < TOOL_LOOP_WARN_THRESHOLD) { FAIL("expected streak >= warn threshold"); return; }
    PASS();
}

static void test_streak_reaches_break(void) {
    TEST(streak_reaches_break);
    ToolLoopRing ring = {.count = 0};
    for (int i = 0; i < TOOL_LOOP_BREAK_THRESHOLD; i++)
        record(&ring, "file_read", "{\"path\":\"/same\"}");
    uint32_t h = tool_call_hash("file_read", "{\"path\":\"/same\"}");
    /* streak is count of consecutive same at tail — should be BREAK_THRESHOLD */
    int s = tool_loop_streak(&ring, h);
    if (s < TOOL_LOOP_BREAK_THRESHOLD) { FAIL("expected streak >= break threshold"); return; }
    PASS();
}

static void test_ring_wraps(void) {
    TEST(ring_wraps);
    ToolLoopRing ring = {.count = 0};
    /* Fill ring with different calls, then add same calls at end */
    for (int i = 0; i < TOOL_LOOP_RING_SIZE - 3; i++) {
        char args[32];
        snprintf(args, sizeof(args), "{\"n\":%d}", i);
        record(&ring, "shell_exec", args);
    }
    /* Now add 3 identical calls */
    record(&ring, "file_read", "{\"path\":\"/loop\"}");
    record(&ring, "file_read", "{\"path\":\"/loop\"}");
    record(&ring, "file_read", "{\"path\":\"/loop\"}");
    uint32_t h = tool_call_hash("file_read", "{\"path\":\"/loop\"}");
    int s = tool_loop_streak(&ring, h);
    if (s != 3) { FAIL("expected streak 3 after wrap"); return; }
    PASS();
}

static void test_ring_overflow(void) {
    TEST(ring_overflow);
    ToolLoopRing ring = {.count = 0};
    /* Record more than RING_SIZE identical calls */
    for (int i = 0; i < TOOL_LOOP_RING_SIZE + 5; i++)
        record(&ring, "file_read", "{\"path\":\"/x\"}");
    uint32_t h = tool_call_hash("file_read", "{\"path\":\"/x\"}");
    int s = tool_loop_streak(&ring, h);
    /* All slots in ring are same hash, so streak = RING_SIZE */
    if (s != TOOL_LOOP_RING_SIZE) { FAIL("expected full ring streak"); return; }
    PASS();
}

int main(void) {
    printf("test_tool_loop:\n");
    test_hash_deterministic();
    test_hash_differs_on_name();
    test_hash_differs_on_args();
    test_streak_zero_empty();
    test_streak_one();
    test_streak_broken_by_different();
    test_streak_reaches_warn();
    test_streak_reaches_break();
    test_ring_wraps();
    test_ring_overflow();
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
