#include <stdio.h>
#include <string.h>
#include "cli.h"
#include "config.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_cli_null_config(void) {
    TEST(cli_null_config);
    int rc = cli_run(NULL);
    if (rc != -1) { FAIL("expected -1 for NULL config"); return; }
    PASS();
}

static void test_cli_no_api_key(void) {
    TEST(cli_no_api_key);
    /* Load config without API key env var */
    Config cfg = {0};
    cfg.db_path = "test_cli.db";
    cfg.workspace = "./workspace";
    cfg.provider.base_url = "http://localhost:9999";
    cfg.provider.model = "test";
    cfg.provider.api_key = NULL;
    int rc = cli_run(&cfg);
    if (rc != -1) { FAIL("expected -1 without api_key"); return; }
    PASS();
}

int main(void) {
    printf("test_cli:\n");
    test_cli_null_config();
    test_cli_no_api_key();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
