#include <stdio.h>
#include <string.h>
#include "arena.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_create_destroy(void) {
    TEST(create_destroy);
    Arena *a = arena_create(1024);
    if (!a) { FAIL("arena_create returned NULL"); return; }
    if (a->cap != 1024) { FAIL("wrong capacity"); arena_destroy(a); return; }
    if (a->used != 0) { FAIL("used not zero"); arena_destroy(a); return; }
    arena_destroy(a);
    PASS();
}

static void test_alloc_basic(void) {
    TEST(alloc_basic);
    Arena *a = arena_create(256);
    void *p1 = arena_alloc(a, 10);
    void *p2 = arena_alloc(a, 20);
    if (!p1 || !p2) { FAIL("alloc returned NULL"); arena_destroy(a); return; }
    if (p1 == p2) { FAIL("same pointer"); arena_destroy(a); return; }
    /* p2 should be 16 bytes after p1 (10 rounded up to 16) */
    if ((char *)p2 - (char *)p1 != 16) { FAIL("bad alignment"); arena_destroy(a); return; }
    arena_destroy(a);
    PASS();
}

static void test_alloc_overflow(void) {
    TEST(alloc_overflow);
    Arena *a = arena_create(64);
    void *p1 = arena_alloc(a, 60);
    if (!p1) { FAIL("first alloc failed"); arena_destroy(a); return; }
    void *p2 = arena_alloc(a, 16);
    if (p2 != NULL) { FAIL("overflow should return NULL"); arena_destroy(a); return; }
    arena_destroy(a);
    PASS();
}


static void test_default_size(void) {
    TEST(default_size);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    if (!a) { FAIL("create failed"); return; }
    if (a->cap != 512 * 1024) { FAIL("wrong default size"); arena_destroy(a); return; }
    arena_destroy(a);
    PASS();
}

int main(void) {
    printf("--- test_arena ---\n");
    test_create_destroy();
    test_alloc_basic();
    test_alloc_overflow();
    test_default_size();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
