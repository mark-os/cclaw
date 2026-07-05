#ifndef CCLAW_SECRET_H
#define CCLAW_SECRET_H

#include <stdint.h>
#include <stddef.h>

/* ChaCha20-Poly1305 AEAD encryption for config secrets.
 * Format: enc:<hex(nonce[24] || ciphertext || tag[16])>
 * Key: 32 bytes from <db_dir>/.cclaw_key */

/* Encrypt plaintext → "enc:<hex(...)>" string. Caller frees result. Returns NULL on error. */
char *secret_encrypt(const uint8_t key[32], const char *plaintext);

/* Decrypt "enc:<hex(...)>" string → plaintext. Caller frees result. Returns NULL on error/tamper. */
char *secret_decrypt(const uint8_t key[32], const char *enc_str);

/* Load or create .cclaw_key next to db_path. Writes 32 bytes into key_out.
 * Creates with getrandom() + mode 0600 if missing. Returns 0 on success, -1 on error. */
int secret_key_load_or_create(const char *db_path, uint8_t key_out[32]);

#endif
