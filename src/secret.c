#define _POSIX_C_SOURCE 200809L
#include "secret.h"
#include "monocypher.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>

#define NONCE_LEN 24
#define TAG_LEN   16
#define ENC_PREFIX "enc:"

static void hex_encode(char *out, const uint8_t *data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int hex_decode(uint8_t *out, const char *hex_str, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned hi, lo;
        char c = hex_str[i * 2];
        if (c >= '0' && c <= '9') hi = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') hi = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hi = (unsigned)(c - 'A' + 10);
        else return -1;
        c = hex_str[i * 2 + 1];
        if (c >= '0' && c <= '9') lo = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') lo = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') lo = (unsigned)(c - 'A' + 10);
        else return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

char *secret_encrypt(const uint8_t key[32], const char *plaintext) {
    if (!key || !plaintext) return NULL;
    size_t pt_len = strlen(plaintext);
    /* nonce(24) + ciphertext(pt_len) + tag(16) */
    size_t blob_len = NONCE_LEN + pt_len + TAG_LEN;
    uint8_t *blob = malloc(blob_len);
    if (!blob) return NULL;

    uint8_t *nonce = blob;
    uint8_t *ct    = blob + NONCE_LEN;
    uint8_t *mac   = blob + NONCE_LEN + pt_len;

    if (getrandom(nonce, NONCE_LEN, 0) != NONCE_LEN) {
        free(blob);
        return NULL;
    }

    crypto_aead_lock(ct, mac, key, nonce, NULL, 0,
                     (const uint8_t *)plaintext, pt_len);

    /* "enc:" + hex(blob) + '\0' */
    size_t result_len = 4 + blob_len * 2 + 1;
    char *result = malloc(result_len);
    if (!result) { free(blob); return NULL; }

    memcpy(result, ENC_PREFIX, 4);
    hex_encode(result + 4, blob, blob_len);

    free(blob);
    return result;
}

char *secret_decrypt(const uint8_t key[32], const char *enc_str) {
    if (!key || !enc_str) return NULL;
    if (strncmp(enc_str, ENC_PREFIX, 4) != 0) return NULL;

    const char *hex_str = enc_str + 4;
    size_t hex_len = strlen(hex_str);
    if (hex_len % 2 != 0) return NULL;

    size_t blob_len = hex_len / 2;
    if (blob_len < NONCE_LEN + TAG_LEN) return NULL;

    uint8_t *blob = malloc(blob_len);
    if (!blob) return NULL;
    if (hex_decode(blob, hex_str, blob_len) != 0) { free(blob); return NULL; }

    size_t pt_len = blob_len - NONCE_LEN - TAG_LEN;
    uint8_t *nonce = blob;
    uint8_t *ct    = blob + NONCE_LEN;
    uint8_t *mac   = blob + NONCE_LEN + pt_len;

    char *plaintext = malloc(pt_len + 1);
    if (!plaintext) { free(blob); return NULL; }

    if (crypto_aead_unlock((uint8_t *)plaintext, mac, key, nonce, NULL, 0,
                           ct, pt_len) != 0) {
        /* Authentication failed — tampered or wrong key */
        free(plaintext);
        free(blob);
        return NULL;
    }

    plaintext[pt_len] = '\0';
    crypto_wipe(blob, blob_len);
    free(blob);
    return plaintext;
}

int secret_key_load_or_create(const char *db_path, uint8_t key_out[32]) {
    if (!db_path || !key_out) return -1;

    /* Build path: <dir of db_path>/.cclaw_key */
    char *path_copy = strdup(db_path);
    if (!path_copy) return -1;
    char *dir = dirname(path_copy);

    size_t key_path_len = strlen(dir) + sizeof("/.cclaw_key");
    char *key_path = malloc(key_path_len);
    if (!key_path) { free(path_copy); return -1; }
    snprintf(key_path, key_path_len, "%s/.cclaw_key", dir);
    free(path_copy);

    int fd = open(key_path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, key_out, 32);
        close(fd);
        free(key_path);
        return (n == 32) ? 0 : -1;
    }

    /* Create new key */
    if (getrandom(key_out, 32, 0) != 32) { free(key_path); return -1; }

    fd = open(key_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        /* Race: another process created it — try reading */
        fd = open(key_path, O_RDONLY);
        if (fd < 0) { free(key_path); return -1; }
        ssize_t n = read(fd, key_out, 32);
        close(fd);
        free(key_path);
        return (n == 32) ? 0 : -1;
    }

    ssize_t w = write(fd, key_out, 32);
    close(fd);
    free(key_path);
    return (w == 32) ? 0 : -1;
}
