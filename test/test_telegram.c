#include "telegram.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_start_no_token(void) {
    Config cfg = {0};
    cfg.db_path = "test_tg.db";
    cfg.workspace = "./workspace";
    assert(telegram_start(&cfg, NULL) == -1);
}

static void test_start_empty_token(void) {
    Config cfg = {0};
    cfg.telegram_token = "";
    cfg.db_path = "test_tg.db";
    cfg.workspace = "./workspace";
    assert(telegram_start(&cfg, NULL) == -1);
}

static void test_split_short_text(void) {
    /* Text shorter than max → return full length */
    const char *text = "Hello world";
    size_t r = tg_find_split(text, strlen(text), 4096);
    assert(r == strlen(text));
}

static void test_split_paragraph(void) {
    /* Split at paragraph boundary (\n\n) */
    char text[200];
    memset(text, 'a', sizeof(text));
    text[50] = '\n';
    text[51] = '\n';
    text[199] = '\0';
    size_t r = tg_find_split(text, 199, 100);
    assert(r == 52); /* after \n\n */
}

static void test_split_newline(void) {
    /* No paragraph break, split at newline */
    char text[200];
    memset(text, 'a', sizeof(text));
    text[60] = '\n';
    text[199] = '\0';
    size_t r = tg_find_split(text, 199, 100);
    assert(r == 61); /* after \n */
}

static void test_split_sentence(void) {
    /* No newline, split at sentence end */
    char text[200];
    memset(text, 'a', sizeof(text));
    text[70] = '.';
    text[71] = ' ';
    text[199] = '\0';
    size_t r = tg_find_split(text, 199, 100);
    assert(r == 71); /* after '.' */
}

static void test_split_hard_cut(void) {
    /* No good split point → hard cut at max_len */
    char text[200];
    memset(text, 'a', sizeof(text));
    text[199] = '\0';
    size_t r = tg_find_split(text, 199, 100);
    assert(r == 100);
}

static void test_split_exact_max(void) {
    /* Text exactly at max → return full length */
    char text[100];
    memset(text, 'b', 99);
    text[99] = '\0';
    size_t r = tg_find_split(text, 99, 100);
    assert(r == 99);
}

static void test_backoff_delay(void) {
    /* V2: 1, 2, 4, 8, 16, 32, 60(cap) */
    assert(tg_backoff_delay(1) == 1);
    assert(tg_backoff_delay(2) == 2);
    assert(tg_backoff_delay(3) == 4);
    assert(tg_backoff_delay(4) == 8);
    assert(tg_backoff_delay(5) == 16);
    assert(tg_backoff_delay(6) == 32);
    assert(tg_backoff_delay(7) == 60);
    assert(tg_backoff_delay(100) == 60);
}

/* V53: telegram_is_admin tests */
static void test_is_admin_match(void) {
    int64_t ids[] = {111, 222, 333};
    Config cfg = {0};
    cfg.admin_chat_ids = ids;
    cfg.admin_chat_id_count = 3;
    assert(telegram_is_admin(&cfg, 111) == 1);
    assert(telegram_is_admin(&cfg, 222) == 1);
    assert(telegram_is_admin(&cfg, 333) == 1);
}

static void test_is_admin_no_match(void) {
    int64_t ids[] = {111, 222};
    Config cfg = {0};
    cfg.admin_chat_ids = ids;
    cfg.admin_chat_id_count = 2;
    assert(telegram_is_admin(&cfg, 999) == 0);
}

static void test_is_admin_empty(void) {
    Config cfg = {0};
    assert(telegram_is_admin(&cfg, 111) == 0);
    assert(telegram_is_admin(NULL, 111) == 0);
}

int main(void) {
    printf("test_start_no_token...");
    test_start_no_token();
    printf(" OK\n");

    printf("test_start_empty_token...");
    test_start_empty_token();
    printf(" OK\n");

    printf("test_split_short_text...");
    test_split_short_text();
    printf(" OK\n");

    printf("test_split_paragraph...");
    test_split_paragraph();
    printf(" OK\n");

    printf("test_split_newline...");
    test_split_newline();
    printf(" OK\n");

    printf("test_split_sentence...");
    test_split_sentence();
    printf(" OK\n");

    printf("test_split_hard_cut...");
    test_split_hard_cut();
    printf(" OK\n");

    printf("test_split_exact_max...");
    test_split_exact_max();
    printf(" OK\n");

    printf("test_backoff_delay...");
    test_backoff_delay();
    printf(" OK\n");

    printf("test_is_admin_match...");
    test_is_admin_match();
    printf(" OK\n");

    printf("test_is_admin_no_match...");
    test_is_admin_no_match();
    printf(" OK\n");

    printf("test_is_admin_empty...");
    test_is_admin_empty();
    printf(" OK\n");

    printf("all telegram tests passed\n");
    return 0;
}
