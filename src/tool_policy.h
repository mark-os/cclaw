#ifndef CCLAW_TOOL_POLICY_H
#define CCLAW_TOOL_POLICY_H

#include "sqlite3.h"

typedef enum { POLICY_ALLOW, POLICY_DENY, POLICY_ASK, POLICY_ERROR } PolicyEffect;

/* Evaluate a tool's policy against its call arguments (SQLite JSON1 — no
 * token caps). NULL/empty policy, or a valid policy object with no rules
 * array, is ALLOW. Malformed policy or args is POLICY_ERROR — fail closed:
 * the dispatch gate turns it into an error tool-result and the call never
 * executes. First matching rule wins; match values are exact string, one-of
 * array, or "*" (key present). */
PolicyEffect policy_eval(sqlite3 *db, const char *args_json,
                         const char *policy_json);

#endif
