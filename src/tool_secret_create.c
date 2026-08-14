#define _GNU_SOURCE
#include "tool_secret_create.h"
#include "tool_args.h"
#include "validate.h"
#include "db.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/random.h>

static const char CHARSET_ALNUM[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
static const char CHARSET_HEX[] = "0123456789abcdef";
/* "full": printable ASCII (0x21-0x7e) minus quotes/backslash/backtick — safe
 * to carry through shell/JSON contexts without escaping surprises. */
static const char CHARSET_FULL[] =
    "!#$%&()*+,-./0123456789:;<=>?@"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_"
    "abcdefghijklmnopqrstuvwxyz{|}~";

static const char *charset_for(const char *name, size_t *out_len) {
    if (!name || strcmp(name, "alnum") == 0) {
        *out_len = sizeof(CHARSET_ALNUM) - 1;
        return CHARSET_ALNUM;
    }
    if (strcmp(name, "hex") == 0) {
        *out_len = sizeof(CHARSET_HEX) - 1;
        return CHARSET_HEX;
    }
    if (strcmp(name, "full") == 0) {
        *out_len = sizeof(CHARSET_FULL) - 1;
        return CHARSET_FULL;
    }
    *out_len = 0;
    return NULL;
}

/* Rejection-sample len random bytes from getrandom() into charset. out must
 * hold len+1 bytes (NUL-terminated). Returns 0 on success, -1 on error. */
static int random_from_charset(char *out, size_t len, const char *charset, size_t clen) {
    unsigned int limit = 256u - (256u % clen);   /* widen: 256 truncates to 0 in a byte */
    size_t filled = 0;
    while (filled < len) {
        unsigned char buf[64];
        size_t want = len - filled;
        if (want > sizeof(buf)) want = sizeof(buf);
        if (getrandom(buf, want, 0) != (ssize_t)want) return -1;
        for (size_t i = 0; i < want && filled < len; i++) {
            if (buf[i] >= limit) continue;   /* reject: would bias the distribution */
            out[filled++] = charset[buf[i] % clen];
        }
    }
    out[len] = '\0';
    return 0;
}

static const char *SECRET_CREATE_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Secret name, ^[A-Z][A-Z0-9_]*$ — becomes {{SECRET:name}}\"},"
    "\"length\":{\"type\":\"integer\",\"description\":\"Character length, 8-128 (default 32)\"},"
    "\"charset\":{\"type\":\"string\",\"description\":\"'alnum' (default), 'hex', or 'full'\"}"
    "},\"required\":[\"name\"]}";

static char *tool_secret_create_handler(const char *arguments, void *user_data, int *is_error) {
    ToolSecretCreateCtx *ctx = (ToolSecretCreateCtx *)user_data;

    char *name = tool_args_str(ctx->db, arguments, "name");
    int length = tool_args_int(ctx->db, arguments, "length", 32);
    char *charset_name = tool_args_str(ctx->db, arguments, "charset");

    if (length < 8) length = 8;
    if (length > 128) length = 128;

    if (!name || !is_valid_secret_name(name)) {
        free(name); free(charset_name);
        return tool_fail(is_error, "error: invalid secret name (expected ^[A-Z][A-Z0-9_]*$)");
    }
    if (db_secret_exists(ctx->db, name)) {
        char err[128];
        snprintf(err, sizeof(err), "error: secret %s already exists", name);
        free(name); free(charset_name);
        return tool_fail(is_error, "%s", err);
    }
    size_t clen = 0;
    const char *charset = charset_for(charset_name, &clen);
    if (!charset) {
        free(name); free(charset_name);
        return tool_fail(is_error, "error: charset must be 'alnum', 'hex', or 'full'");
    }

    char value[129];
    if (random_from_charset(value, (size_t)length, charset, clen) != 0) {
        free(name); free(charset_name);
        return tool_fail(is_error, "error: failed to generate random value");
    }

    int rc = db_secret_set(ctx->db, name, value, "generated", "agent");
    explicit_bzero(value, sizeof(value));

    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s", name);
    char charset_buf[16];
    snprintf(charset_buf, sizeof(charset_buf), "%s", charset_name ? charset_name : "alnum");
    free(name); free(charset_name);

    if (rc != 0)
        return tool_fail(is_error, "error: failed to store secret");

    char *result = malloc(256);
    if (!result) return tool_fail(is_error, "error: out of memory");
    snprintf(result, 256,
             "stored as {{SECRET:%s}} (%d chars, %s). No host bindings yet — "
             "first use will require approval; ALWAYS-approve a url-bearing "
             "call to bind it.",
             name_buf, length, charset_buf);
    return result;
}

int tool_secret_create_register(ToolRegistry *reg, ToolSecretCreateCtx *ctx) {
    return tools_register(reg, "secret_create",
                          "Mint a new random credential, stored encrypted in the DB. "
                          "Returns only its {{SECRET:name}} placeholder — never the value.",
                          SECRET_CREATE_PARAMS_JSON, tool_secret_create_handler, ctx);
}
