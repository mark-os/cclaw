#ifndef CCLAW_VALIDATE_H
#define CCLAW_VALIDATE_H

#include <string.h>

/* Validate name for agents and memory block labels: [A-Za-z0-9_-]+, max 63 chars. */
static inline int is_valid_name(const char *name) {
    if (!name || !name[0]) return 0;
    size_t len = strlen(name);
    if (len > 63) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

#endif
