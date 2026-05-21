#include "telegram.h"
#include "db.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_start_no_token(void) {
    Config cfg = {0};
    cfg.db_path = "test_tg.db";
    cfg.workspace = "./workspace";
    /* No token → should fail */
    assert(telegram_start(&cfg, NULL) == -1);
}

static void test_start_empty_token(void) {
    Config cfg = {0};
    cfg.telegram_token = "";
    cfg.db_path = "test_tg.db";
    cfg.workspace = "./workspace";
    assert(telegram_start(&cfg, NULL) == -1);
}

int main(void) {
    printf("test_start_no_token...");
    test_start_no_token();
    printf(" OK\n");

    printf("test_start_empty_token...");
    test_start_empty_token();
    printf(" OK\n");

    printf("all telegram tests passed\n");
    return 0;
}
