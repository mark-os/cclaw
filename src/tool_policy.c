/* Per-argument tool policy, evaluated in the trusted parent on the same
 * parser as everything else (SQLite JSON1). This replaced a jsmn walk with a
 * 128-token budget whose overflow substituted empty args and let keyed deny
 * rules silently miss (review-4 F9 / review-2 F10) — with SQLite there is no
 * size limit, so the only failure left is genuinely invalid JSON, and that
 * is POLICY_ERROR, never ALLOW. */
#include "tool_policy.h"

#include <string.h>

PolicyEffect policy_eval(sqlite3 *db, const char *args_json,
                         const char *policy_json) {
    if (!policy_json || !policy_json[0]) return POLICY_ALLOW;
    if (!db || !args_json) return POLICY_ERROR;

    /* One query: first rule (by array order) whose match object has no
     * failing key. Match semantics per key:
     *   "*"        — key present in args (any type)
     *   "literal"  — args value is text and equals it
     *   [...]      — args value is text and equals one of the text elements
     * A key absent from args fails the rule. Empty match {} is a catch-all.
     * Rules without a match object are skipped (never match). */
    static const char *SQL =
        "SELECT CASE json_extract(r.value,'$.effect')"
        "         WHEN 'deny' THEN 1 WHEN 'ask' THEN 2 ELSE 0 END"
        " FROM json_each(?1,'$.rules') r"
        " WHERE json_type(r.value,'$.match')='object'"
        "   AND NOT EXISTS ("
        "     SELECT 1 FROM json_each(r.value,'$.match') m"
        "     WHERE NOT CASE"
        "       WHEN m.type='text' AND m.atom='*'"
        "         THEN json_type(?2,'$.'||m.key) IS NOT NULL"
        "       WHEN m.type='text'"
        "         THEN json_type(?2,'$.'||m.key)='text'"
        "          AND json_extract(?2,'$.'||m.key)=m.atom"
        "       WHEN m.type='array'"
        "         THEN json_type(?2,'$.'||m.key)='text'"
        "          AND EXISTS (SELECT 1 FROM json_each(m.value) e"
        "                      WHERE e.type='text'"
        "                        AND e.atom=json_extract(?2,'$.'||m.key))"
        "       ELSE 0 END)"
        " ORDER BY r.id LIMIT 1";

    /* Pre-checks: malformed policy/args error out; a policy without a rules
     * array is semantically empty (ALLOW), matching the old contract. */
    sqlite3_stmt *chk;
    if (sqlite3_prepare_v2(db,
            "SELECT json_valid(?1) AND json_type(?1)='object',"
            "       json_valid(?2) AND json_type(?2)='object',"
            "       json_type(?1,'$.rules')='array'",
            -1, &chk, NULL) != SQLITE_OK)
        return POLICY_ERROR;
    sqlite3_bind_text(chk, 1, policy_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(chk, 2, args_json, -1, SQLITE_STATIC);
    int pol_ok = 0, args_ok = 0, has_rules = 0;
    if (sqlite3_step(chk) == SQLITE_ROW) {
        pol_ok = sqlite3_column_int(chk, 0);
        args_ok = sqlite3_column_int(chk, 1);
        has_rules = sqlite3_column_int(chk, 2);
    }
    sqlite3_finalize(chk);
    if (!pol_ok || !args_ok) return POLICY_ERROR;
    if (!has_rules) return POLICY_ALLOW;

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK)
        return POLICY_ERROR;
    sqlite3_bind_text(st, 1, policy_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, args_json, -1, SQLITE_STATIC);
    PolicyEffect eff = POLICY_ALLOW;  /* no rule matched */
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        switch (sqlite3_column_int(st, 0)) {
        case 1: eff = POLICY_DENY; break;
        case 2: eff = POLICY_ASK; break;
        default: eff = POLICY_ALLOW; break;
        }
    } else if (rc != SQLITE_DONE) {
        eff = POLICY_ERROR;  /* json_* raised mid-scan — fail closed */
    }
    sqlite3_finalize(st);
    return eff;
}
