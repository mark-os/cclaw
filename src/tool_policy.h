#ifndef CCLAW_TOOL_POLICY_H
#define CCLAW_TOOL_POLICY_H

typedef enum { POLICY_ALLOW, POLICY_DENY, POLICY_ASK } PolicyEffect;

/* Evaluate a tool's policy against its call arguments.
 * Both strings are JSON parsed with jsmn. NULL/empty policy -> ALLOW.
 * Malformed policy -> ALLOW (fail-open). First matching rule wins. */
PolicyEffect policy_eval(const char *args_json, const char *policy_json);

#endif
