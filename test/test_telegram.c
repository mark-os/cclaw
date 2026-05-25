#include "telegram.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* T140: telegram_parse_admin_command tests */
static void test_parse_admin_key(void) {
    const char *args = NULL;
    assert(telegram_parse_admin_command("/key", &args) == 1);
    assert(args == NULL);
    assert(telegram_parse_admin_command("/key openrouter", &args) == 1);
    assert(args != NULL && strcmp(args, "openrouter") == 0);
}

static void test_parse_admin_config(void) {
    const char *args = NULL;
    assert(telegram_parse_admin_command("/config", &args) == 2);
    assert(args == NULL);
    assert(telegram_parse_admin_command("/config model gpt-4", &args) == 2);
    assert(args != NULL && strcmp(args, "model gpt-4") == 0);
}

static void test_parse_admin_whitelist(void) {
    const char *args = NULL;
    assert(telegram_parse_admin_command("/whitelist", &args) == 3);
    assert(args == NULL);
    assert(telegram_parse_admin_command("/whitelist add api.example.com", &args) == 3);
    assert(args != NULL && strcmp(args, "add api.example.com") == 0);
}

static void test_parse_admin_unknown(void) {
    const char *args = NULL;
    assert(telegram_parse_admin_command("/help", &args) == 0);
    assert(telegram_parse_admin_command("hello", &args) == 0);
}

/* T141: telegram_key_env_name tests */
static void test_key_env_name(void) {
    assert(strcmp(telegram_key_env_name("openrouter"), "OPENROUTER_API_KEY") == 0);
    assert(strcmp(telegram_key_env_name("gemini"), "GEMINI_API_KEY") == 0);
    assert(telegram_key_env_name("unknown") == NULL);
}

/* T141: telegram_write_env_key tests */
static void test_write_env_key_new(void) {
    const char *path = "/tmp/cclaw_test_env_key";
    unlink(path);
    assert(telegram_write_env_key(path, "MY_KEY", "sk-123") == 0);

    FILE *f = fopen(path, "r");
    assert(f);
    char buf[256];
    assert(fgets(buf, sizeof(buf), f));
    assert(strcmp(buf, "MY_KEY=sk-123\n") == 0);
    fclose(f);
    unlink(path);
}

static void test_write_env_key_replace(void) {
    const char *path = "/tmp/cclaw_test_env_key2";
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "OTHER=abc\nMY_KEY=old-value\nANOTHER=xyz\n");
    fclose(f);

    assert(telegram_write_env_key(path, "MY_KEY", "new-value") == 0);

    f = fopen(path, "r");
    assert(f);
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    assert(strstr(buf, "MY_KEY=new-value\n") != NULL);
    assert(strstr(buf, "OTHER=abc\n") != NULL);
    assert(strstr(buf, "ANOTHER=xyz\n") != NULL);
    assert(strstr(buf, "old-value") == NULL);
    unlink(path);
}

static void test_write_env_key_append(void) {
    const char *path = "/tmp/cclaw_test_env_key3";
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "EXISTING=val\n");
    fclose(f);

    assert(telegram_write_env_key(path, "NEW_KEY", "new-val") == 0);

    f = fopen(path, "r");
    assert(f);
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    assert(strstr(buf, "EXISTING=val\n") != NULL);
    assert(strstr(buf, "NEW_KEY=new-val\n") != NULL);
    unlink(path);
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

    printf("test_parse_admin_key...");
    test_parse_admin_key();
    printf(" OK\n");

    printf("test_parse_admin_config...");
    test_parse_admin_config();
    printf(" OK\n");

    printf("test_parse_admin_whitelist...");
    test_parse_admin_whitelist();
    printf(" OK\n");

    printf("test_parse_admin_unknown...");
    test_parse_admin_unknown();
    printf(" OK\n");

    printf("test_key_env_name...");
    test_key_env_name();
    printf(" OK\n");

    printf("test_write_env_key_new...");
    test_write_env_key_new();
    printf(" OK\n");

    printf("test_write_env_key_replace...");
    test_write_env_key_replace();
    printf(" OK\n");

    printf("test_write_env_key_append...");
    test_write_env_key_append();
    printf(" OK\n");

    printf("all telegram tests passed\n");
    return 0;
}
