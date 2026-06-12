#!/usr/bin/env python3
"""Generate Aho-Corasick state tables for secret scanning from gitleaks.toml.

Reads vendor/gitleaks.toml (and optional vendor/secrets_custom.toml),
extracts keywords and regex metadata, builds an AC automaton, and emits:
  - include/secret_scan_ac.h   (state transitions, failure links, accept states)
  - include/secret_scan_rules.h (per-rule validation parameters)

The generated tables are const and live in .rodata (zero heap on Pogoplug).
"""

import re
import sys
import os
from collections import deque

# Minimal TOML parser — only needs [[rules]] with id, keywords, regex, entropy
def parse_toml_rules(path):
    """Extract rules from gitleaks-style TOML. Returns list of dicts."""
    rules = []
    current = None
    in_array = None  # track multi-line array parsing
    with open(path) as f:
        for line in f:
            line = line.rstrip('\n')
            if line.strip() == '[[rules]]':
                if current:
                    rules.append(current)
                current = {'id': '', 'keywords': [], 'regex': '', 'entropy': 0.0}
                in_array = None
                continue
            if current is None:
                continue
            # Skip sub-tables like [[rules.allowlists]]
            if line.strip().startswith('[['):
                if current:
                    rules.append(current)
                    current = None
                in_array = None
                continue
            # Multi-line array continuation
            if in_array is not None:
                kws = re.findall(r'"([^"]*)"', line)
                current[in_array].extend(k.lower() for k in kws)
                if ']' in line:
                    in_array = None
                continue
            # Parse key = value
            m = re.match(r'^(\w+)\s*=\s*(.+)$', line)
            if not m:
                continue
            key, val = m.group(1), m.group(2).strip()
            if key == 'id':
                current['id'] = val.strip("'\"")
            elif key == 'entropy':
                try:
                    current['entropy'] = float(val)
                except ValueError:
                    pass
            elif key == 'regex':
                # Handle triple-quoted strings
                if val.startswith("'''"):
                    current['regex'] = val.strip("'")
                else:
                    current['regex'] = val.strip("'\"")
            elif key == 'keywords':
                # Single-line array or start of multi-line
                kws = re.findall(r'"([^"]*)"', val)
                current['keywords'] = [k.lower() for k in kws]
                if '[' in val and ']' not in val:
                    in_array = 'keywords'
    if current:
        rules.append(current)
    return rules


# High-signal rules to include (curated subset)
CURATED_IDS = {
    'aws-access-token', 'anthropic-api-key', 'anthropic-admin-api-key',
    'gcp-api-key', 'azure-ad-client-secret',
    'github-pat', 'github-fine-grained-pat', 'github-oauth', 'github-app-token',
    'github-refresh-token',
    'gitlab-pat', 'gitlab-ptt', 'gitlab-runner-authentication-token',
    'openai-api-key',
    'slack-bot-token', 'slack-user-token', 'slack-app-token', 'slack-webhook-url',
    'stripe-access-token',
    'npm-access-token', 'pypi-upload-token',
    'private-key',
    'jwt',
    'digitalocean-pat', 'digitalocean-access-token',
    'heroku-api-key-v2',
    'shopify-access-token', 'shopify-custom-access-token',
    'sendgrid-api-token',
    'twilio-api-key',
    'telegram-bot-api-token',
    'databricks-api-token',
    'huggingface-access-token',
    'generic-api-key',
    'age-secret-key',
    'vault-batch-token', 'vault-service-token',
    'doppler-api-token',
    'flyio-access-token',
    'grafana-api-key', 'grafana-cloud-api-token',
    'linear-api-key',
    'notion-api-token',
    'perplexity-api-key',
    'snyk-api-token',
    'sourcegraph-access-token',
}

# Validation types
VTYPE_PREFIX  = 0   # Fixed prefix: validate tail charset + length
VTYPE_KEYWORD = 1   # Contextual keyword: check assignment pattern + entropy
VTYPE_LITERAL = 2   # Strong literal marker: flag on the anchor alone

# Charset classes (must match secret_scan.c charset_ok)
CHARSET_ANY         = 0
CHARSET_UPPER_ALNUM = 1
CHARSET_LOWER_ALNUM = 2
CHARSET_ALNUM       = 3
CHARSET_HEX         = 4

# Lowering thresholds
MIN_ANCHOR_LEN     = 3      # anchors shorter than this carpet-bomb the AC scan
LITERAL_ANCHOR_LEN = 8      # strong enough to flag on its own (e.g. -----BEGIN)
OPEN_TAIL_CAP      = 4096   # cap for open-ended {n,} quantifiers


def classify_rule(rule):
    """Determine if a rule is prefix-based or keyword-contextual."""
    regex = rule.get('regex', '')
    # Prefix rules typically start with a literal prefix match: \b(PREFIX...
    # or just the keyword IS the prefix (like ghp_, AKIA, etc.)
    # Keyword rules have contextual patterns with (?i) and assignment operators
    if '(?:=|>|:{1,3}=|' in regex or '(?:=|>|:' in regex:
        return VTYPE_KEYWORD
    return VTYPE_PREFIX


def map_charset(charset_str):
    """Map a regex character-class body to a SCAN_CHARSET_* enum, preserving
    hex-ness and case (a collapse to generic ALNUM is what made e.g. the
    twilio 'SK[0-9a-fA-F]{32}' rule match base64 blobs and file paths)."""
    norm = charset_str.replace('\\', '')
    # Pure hex: ranges drawn only from 0-9 / a-f / A-F, and at least one of a-f.
    leftover = re.sub(r'(?:0-9|a-f|A-F)', '', norm)
    if leftover == '' and ('a-f' in norm or 'A-F' in norm):
        return CHARSET_HEX
    # Punctuation (=, ., +, /) is only covered by the broad ALNUM class; the
    # LOWER/UPPER classes exclude it and would truncate the tail early.
    if any(p in norm for p in ('=', '.', '+', '/')):
        return CHARSET_ALNUM
    has_upper = 'A-Z' in charset_str
    has_lower = 'a-z' in charset_str
    if has_upper and has_lower:
        return CHARSET_ALNUM
    if has_upper:
        return CHARSET_UPPER_ALNUM
    if has_lower:
        return CHARSET_LOWER_ALNUM
    return CHARSET_ALNUM


def extract_tail_params(rule):
    """Extract (tail_min, tail_max, charset) from the first bounded character
    class in a prefix rule's regex.

    Returns None when the tail cannot be faithfully represented as a single
    contiguous charset run — an open-set class like [\\s\\S], or no bounded
    quantifier at all. The caller then either treats the rule as a literal
    marker (strong anchor) or drops it, rather than inventing parameters. The
    old blind '{N,M}' fallback is what turned the jwt rule into a degenerate
    'ey' + {0,2} matcher (it latched onto the trailing '={0,2}')."""
    regex = rule.get('regex', '')
    # Common patterns: [A-Z2-7]{16}, [a-z0-9]{36}, [a-zA-Z0-9_-]{93}, [\w-]{17,}
    m = re.search(r'\[([A-Za-z0-9\-_\\/+=.]+)\]\{(\d+)(?:,(\d+)?)?\}', regex)
    if not m:
        return None
    charset_str = m.group(1)
    # [\s..]/[\S..] match arbitrary bytes (incl. newlines) — not a tail we can
    # validate with one charset run; let the caller fall back to literal/drop.
    if '\\s' in charset_str or '\\S' in charset_str:
        return None
    min_len = int(m.group(2))
    if m.group(3):
        max_len = int(m.group(3))
    elif ',' in m.group(0):          # open-ended {n,}
        max_len = max(min_len, OPEN_TAIL_CAP)
    else:                            # exact {n}
        max_len = min_len
    return min_len, max_len, map_charset(charset_str)


def extract_literal_prefix(regex):
    """Leading run of literal characters at the start of the secret body, used
    to pin the exact case of a short anchor (so e.g. twilio's 'SK' stays upper
    even though the AC table is case-folded). Returns '' when the regex opens
    with an alternation/group/metaclass — i.e. no single literal prefix."""
    s = regex
    for pre in ('(?i)', r'\b', '^', '(?:', '('):
        while s.startswith(pre):
            s = s[len(pre):]
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt.isalpha():        # \w \d \s ... — a metaclass, stop
                break
            out.append(nxt); i += 2; continue   # \. \- ... — literal punct
        if c.isalnum() or c in '_-':
            out.append(c); i += 1; continue
        break
    return ''.join(out)


def build_ac(keywords):
    """Build Aho-Corasick automaton from keyword list.
    Returns (goto_table, failure, accept) where:
      - goto_table[state][byte] = next_state (-1 = fail)
      - failure[state] = failure_state
      - accept[state] = list of (keyword_idx, keyword)
    """
    goto = [{}]  # state 0 = root
    accept = [[]]
    state_count = 1

    # Build trie
    for idx, kw in enumerate(keywords):
        cur = 0
        for ch in kw:
            b = ord(ch)
            if b not in goto[cur]:
                goto[cur][b] = state_count
                goto.append({})
                accept.append([])
                state_count += 1
            cur = goto[cur][b]
        accept[cur].append(idx)

    # Build failure links via BFS
    failure = [0] * state_count
    queue = deque()
    # Depth-1 states fail to root
    for b, s in goto[0].items():
        failure[s] = 0
        queue.append(s)

    while queue:
        r = queue.popleft()
        for b, s in goto[r].items():
            queue.append(s)
            state = failure[r]
            while state != 0 and b not in goto[state]:
                state = failure[state]
            failure[s] = goto[state].get(b, 0)
            if failure[s] == s:
                failure[s] = 0
            # Merge accept states
            accept[s] = accept[s] + accept[failure[s]]

    return goto, failure, accept, state_count


def emit_headers(keywords, rules_meta, goto, failure, accept, state_count):
    """Emit C headers for the AC automaton."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    include_dir = os.path.join(repo_root, 'include')

    # --- Compute column remap ---
    # Find all bytes actually used in goto transitions (already lowercase from trie)
    used_bytes = set()
    for s in range(state_count):
        for b in goto[s]:
            used_bytes.add(b)
    used_bytes = sorted(used_bytes)  # deterministic order
    num_cols = len(used_bytes) + 1   # col 0 = unmapped (always → state 0)

    # byte_to_col: 128 entries, uppercase maps to same col as lowercase
    byte_to_col = [0] * 128
    for idx, b in enumerate(used_bytes):
        col = idx + 1
        byte_to_col[b] = col
        # Map uppercase to same column
        if 97 <= b <= 122:
            byte_to_col[b - 32] = col

    # --- Build compressed transition table ---
    transitions = []
    for s in range(state_count):
        row = [0] * num_cols  # col 0 always 0
        for idx, b in enumerate(used_bytes):
            col = idx + 1
            cur = s
            while cur != 0 and b not in goto[cur]:
                cur = failure[cur]
            row[col] = goto[cur].get(b, 0)
        transitions.append(row)

    # Build accept table
    max_accepts = max(len(a) for a in accept) if accept else 0

    # --- secret_scan_ac.h ---
    with open(os.path.join(include_dir, 'secret_scan_ac.h'), 'w') as f:
        f.write("/* AUTO-GENERATED by scripts/gen_secret_scan.py — DO NOT EDIT */\n")
        f.write("#ifndef CCLAW_SECRET_SCAN_AC_H\n#define CCLAW_SECRET_SCAN_AC_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define SCAN_AC_STATES {state_count}\n")
        f.write(f"#define SCAN_AC_COLS {num_cols}\n")
        f.write(f"#define SCAN_AC_KEYWORDS {len(keywords)}\n\n")

        # Column remap table
        f.write("static const uint8_t scan_ac_col[128] = {\n  ")
        for i in range(0, 128, 16):
            f.write(",".join(str(byte_to_col[j]) for j in range(i, min(i+16, 128))))
            if i + 16 < 128:
                f.write(",\n  ")
        f.write("\n};\n\n")

        # Transition table
        f.write(f"static const int16_t scan_ac_goto[{state_count}][{num_cols}] = {{\n")
        for row in transitions:
            f.write("  {")
            f.write(",".join(str(v) for v in row))
            f.write("},\n")
        f.write("};\n\n")

        # Accept table
        f.write(f"#define SCAN_AC_MAX_ACCEPT {max(max_accepts, 1)}\n")
        f.write(f"static const int16_t scan_ac_accept[{state_count}][{max_accepts + 1}] = {{\n")
        for s in range(state_count):
            a = accept[s]
            vals = [len(a)] + a + [0] * (max_accepts - len(a))
            f.write("  {" + ",".join(str(v) for v in vals) + "},\n")
        f.write("};\n\n")

        f.write("#endif /* CCLAW_SECRET_SCAN_AC_H */\n")

    # --- secret_scan_rules.h ---
    with open(os.path.join(include_dir, 'secret_scan_rules.h'), 'w') as f:
        f.write("/* AUTO-GENERATED by scripts/gen_secret_scan.py — DO NOT EDIT */\n")
        f.write("#ifndef CCLAW_SECRET_SCAN_RULES_H\n#define CCLAW_SECRET_SCAN_RULES_H\n\n")
        f.write("/* Validation types */\n")
        f.write("#define SCAN_VTYPE_PREFIX  0\n")
        f.write("#define SCAN_VTYPE_KEYWORD 1\n")
        f.write("#define SCAN_VTYPE_LITERAL 2\n\n")
        f.write("/* Charset classes */\n")
        f.write("#define SCAN_CHARSET_ANY         0\n")
        f.write("#define SCAN_CHARSET_UPPER_ALNUM 1\n")
        f.write("#define SCAN_CHARSET_LOWER_ALNUM 2\n")
        f.write("#define SCAN_CHARSET_ALNUM       3\n")
        f.write("#define SCAN_CHARSET_HEX         4\n\n")

        f.write(f"#define SCAN_RULE_COUNT {len(rules_meta)}\n\n")
        f.write("typedef struct {\n")
        f.write("    const char *id;\n")
        f.write("    const char *keyword;\n")
        f.write("    int vtype;        /* SCAN_VTYPE_PREFIX/KEYWORD/LITERAL */\n")
        f.write("    int tail_min;     /* min chars after prefix (prefix type) */\n")
        f.write("    int tail_max;     /* max chars after prefix */\n")
        f.write("    int charset;      /* SCAN_CHARSET_* */\n")
        f.write("    int nocase;       /* 1 = match anchor case-insensitively; 0 = exact case */\n")
        f.write("    float entropy;    /* min entropy threshold (0 = no check) */\n")
        f.write("} ScanRule;\n\n")

        f.write(f"static const ScanRule scan_rules[{len(rules_meta)}] = {{\n")
        for rm in rules_meta:
            f.write(f'    {{"{rm["id"]}", "{rm["keyword"]}", '
                    f'{rm["vtype"]}, {rm["tail_min"]}, {rm["tail_max"]}, '
                    f'{rm["charset"]}, {rm["nocase"]}, {rm["entropy"]:.1f}f}},\n')
        f.write("};\n\n")

        f.write("#endif /* CCLAW_SECRET_SCAN_RULES_H */\n")

    table_bytes = state_count * num_cols * 2
    print(f"Generated: {state_count} states, {len(keywords)} keywords, "
          f"{len(rules_meta)} rules, {num_cols} columns")
    print(f"  include/secret_scan_ac.h ({table_bytes} bytes transition table + 128 byte remap)")
    print(f"  include/secret_scan_rules.h")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    toml_path = os.path.join(repo_root, 'vendor', 'gitleaks.toml')
    custom_path = os.path.join(repo_root, 'vendor', 'secrets_custom.toml')

    if not os.path.exists(toml_path):
        print(f"Error: {toml_path} not found", file=sys.stderr)
        sys.exit(1)

    rules = parse_toml_rules(toml_path)
    if os.path.exists(custom_path):
        rules += parse_toml_rules(custom_path)
        print(f"Loaded custom rules from {custom_path}")

    # Filter to curated set
    curated = [r for r in rules if r['id'] in CURATED_IDS]
    print(f"Parsed {len(rules)} rules, using {len(curated)} curated rules")

    # Build keyword list and rule metadata
    keywords = []
    rules_meta = []
    seen_keywords = set()

    dropped = 0
    for rule in curated:
        vtype = classify_rule(rule)
        params = extract_tail_params(rule) if vtype == VTYPE_PREFIX else None
        for kw in rule.get('keywords', []):
            kw_lower = kw.lower()
            if kw_lower in seen_keywords:
                continue

            kw_canonical = kw_lower   # case used for the C-side exact re-check
            nocase = 1                # default: case-insensitive (AC table is folded)

            if vtype == VTYPE_KEYWORD:
                kw_vtype = VTYPE_KEYWORD
                tail_min, tail_max, charset = 10, 150, CHARSET_ALNUM
            elif params is not None:
                tail_min, tail_max, charset = params
                if len(kw_lower) < MIN_ANCHOR_LEN:
                    # A short anchor only earns its place if the tail is strong
                    # enough that the anchor isn't doing the work — a fixed-length
                    # HEX run — AND we can pin the exact case so it doesn't match
                    # folded (twilio: SK[0-9a-fA-F]{32}, not every "sk"/"Sk").
                    lit = extract_literal_prefix(rule.get('regex', ''))
                    if (charset == CHARSET_HEX and tail_min == tail_max
                            and tail_min >= 16 and lit and lit.lower() == kw_lower
                            and '(?i)' not in rule.get('regex', '')):
                        nocase = 0
                        kw_canonical = lit
                    else:
                        print(f"  drop {rule['id']!r} anchor {kw_lower!r}: "
                              f"too short ({len(kw_lower)} < {MIN_ANCHOR_LEN})",
                              file=sys.stderr)
                        dropped += 1
                        continue
                kw_vtype = VTYPE_PREFIX
            elif len(kw_lower) >= LITERAL_ANCHOR_LEN:
                # Unparseable tail but a strong, distinctive literal: flag on
                # the anchor alone (e.g. -----BEGIN ... PRIVATE KEY-----).
                kw_vtype = VTYPE_LITERAL
                tail_min, tail_max, charset = 0, 0, CHARSET_ANY
            else:
                print(f"  drop {rule['id']!r} anchor {kw_lower!r}: "
                      f"unparseable tail and weak anchor", file=sys.stderr)
                dropped += 1
                continue

            seen_keywords.add(kw_lower)
            keywords.append(kw_lower)   # AC trie: lowercase; the table is folded

            entropy = rule.get('entropy', 0.0)
            if kw_vtype == VTYPE_KEYWORD and entropy == 0.0:
                entropy = 3.5  # default for contextual rules

            # Cap entropy at what's achievable: log2(tail_max) is the theoretical
            # max for a tail of that length. Use 90% of that as ceiling.
            if kw_vtype == VTYPE_PREFIX and entropy > 0 and tail_max > 0:
                import math
                max_possible = math.log2(tail_max) if tail_max > 1 else 0
                if entropy > max_possible:
                    entropy = round(max_possible - 0.5, 1)  # conservative cap

            rules_meta.append({
                'id': rule['id'],
                'keyword': kw_canonical,
                'vtype': kw_vtype,
                'tail_min': tail_min,
                'tail_max': tail_max,
                'charset': charset,
                'nocase': nocase,
                'entropy': entropy,
            })

    print(f"Keywords: {len(keywords)} ({dropped} anchors dropped)")

    # Build AC automaton
    goto, failure, accept, state_count = build_ac(keywords)

    # Emit C headers
    emit_headers(keywords, rules_meta, goto, failure, accept, state_count)


if __name__ == '__main__':
    main()
