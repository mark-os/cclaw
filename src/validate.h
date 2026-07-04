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

/* Validate agent name: PascalCase — ^[A-Z][a-zA-Z0-9]*$, max 63 chars. */
static inline int is_valid_agent_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (!(name[0] >= 'A' && name[0] <= 'Z')) return 0;
    size_t len = strlen(name);
    if (len > 63) return 0;
    for (size_t i = 1; i < len; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9')))
            return 0;
    }
    return 1;
}

/* Validate secret name: ^[A-Z][A-Z0-9_]{0,62}$ — becomes CCLAW_SECRET_<name>
 * in shell children and {{SECRET:name}} placeholders, so no lowercase/symbols. */
static inline int is_valid_secret_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (!(name[0] >= 'A' && name[0] <= 'Z')) return 0;
    size_t len = strlen(name);
    if (len > 63) return 0;
    for (size_t i = 1; i < len; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

#endif
