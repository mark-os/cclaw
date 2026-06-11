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
VTYPE_PREFIX = 0   # Fixed prefix: validate tail charset + length
VTYPE_KEYWORD = 1  # Contextual keyword: check assignment pattern + entropy


def classify_rule(rule):
    """Determine if a rule is prefix-based or keyword-contextual."""
    regex = rule.get('regex', '')
    # Prefix rules typically start with a literal prefix match: \b(PREFIX...
    # or just the keyword IS the prefix (like ghp_, AKIA, etc.)
    # Keyword rules have contextual patterns with (?i) and assignment operators
    if '(?:=|>|:{1,3}=|' in regex or '(?:=|>|:' in regex:
        return VTYPE_KEYWORD
    return VTYPE_PREFIX


def extract_tail_params(rule):
    """For prefix rules, extract expected tail charset and length from regex."""
    regex = rule.get('regex', '')
    # Common patterns: [A-Z2-7]{16}, [a-z0-9]{36}, [a-zA-Z0-9_-]{93}
    m = re.search(r'\[([A-Za-z0-9\-_\\]+)\]\{(\d+)(?:,(\d+))?\}', regex)
    if m:
        charset_str = m.group(1)
        min_len = int(m.group(2))
        max_len = int(m.group(3)) if m.group(3) else min_len
        # Map charset to enum
        if 'A-Z' in charset_str and 'a-z' in charset_str:
            charset = 3  # CHARSET_ALNUM
        elif 'A-Z' in charset_str:
            charset = 1  # CHARSET_UPPER_ALNUM
        elif 'a-z' in charset_str:
            charset = 2  # CHARSET_LOWER_ALNUM
        else:
            charset = 3  # CHARSET_ALNUM
        return min_len, max_len, charset
    # Try {N,M} without charset (means any)
    m = re.search(r'\{(\d+),(\d+)\}', regex)
    if m:
        return int(m.group(1)), int(m.group(2)), 3
    return 20, 100, 3  # defaults for unknown patterns


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

    # --- secret_scan_ac.h ---
    # Build dense transition table (state × 128 bytes, case-folded)
    # For each state and byte, compute next state following failure links
    transitions = []
    for s in range(state_count):
        row = [0] * 128
        for b in range(128):
            # Case-fold: uppercase -> lowercase
            folded = b
            if 65 <= b <= 90:
                folded = b + 32
            # Follow goto/failure chain
            cur = s
            while cur != 0 and folded not in goto[cur]:
                cur = failure[cur]
            row[b] = goto[cur].get(folded, 0)
        transitions.append(row)

    # Build accept table: for each state, list of keyword indices
    max_accepts = max(len(a) for a in accept) if accept else 0

    with open(os.path.join(include_dir, 'secret_scan_ac.h'), 'w') as f:
        f.write("/* AUTO-GENERATED by scripts/gen_secret_scan.py — DO NOT EDIT */\n")
        f.write("#ifndef CCLAW_SECRET_SCAN_AC_H\n#define CCLAW_SECRET_SCAN_AC_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define SCAN_AC_STATES {state_count}\n")
        f.write(f"#define SCAN_AC_KEYWORDS {len(keywords)}\n\n")

        # Transition table
        f.write(f"static const int16_t scan_ac_goto[{state_count}][128] = {{\n")
        for s, row in enumerate(transitions):
            # Compress: only emit non-zero
            f.write("  {")
            f.write(",".join(str(v) for v in row))
            f.write("},\n")
        f.write("};\n\n")

        # Accept table: scan_ac_accept[state][0] = count, then keyword indices
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
        f.write("#define SCAN_VTYPE_KEYWORD 1\n\n")
        f.write("/* Charset classes */\n")
        f.write("#define SCAN_CHARSET_ANY        0\n")
        f.write("#define SCAN_CHARSET_UPPER_ALNUM 1\n")
        f.write("#define SCAN_CHARSET_LOWER_ALNUM 2\n")
        f.write("#define SCAN_CHARSET_ALNUM      3\n\n")

        f.write(f"#define SCAN_RULE_COUNT {len(rules_meta)}\n\n")
        f.write("typedef struct {\n")
        f.write("    const char *id;\n")
        f.write("    const char *keyword;\n")
        f.write("    int vtype;        /* SCAN_VTYPE_PREFIX or SCAN_VTYPE_KEYWORD */\n")
        f.write("    int tail_min;     /* min chars after prefix (prefix type) */\n")
        f.write("    int tail_max;     /* max chars after prefix */\n")
        f.write("    int charset;      /* SCAN_CHARSET_* */\n")
        f.write("    float entropy;    /* min entropy threshold (0 = no check) */\n")
        f.write("} ScanRule;\n\n")

        f.write(f"static const ScanRule scan_rules[{len(rules_meta)}] = {{\n")
        for rm in rules_meta:
            f.write(f'    {{"{rm["id"]}", "{rm["keyword"]}", '
                    f'{rm["vtype"]}, {rm["tail_min"]}, {rm["tail_max"]}, '
                    f'{rm["charset"]}, {rm["entropy"]:.1f}f}},\n')
        f.write("};\n\n")

        f.write("#endif /* CCLAW_SECRET_SCAN_RULES_H */\n")

    print(f"Generated: {state_count} states, {len(keywords)} keywords, "
          f"{len(rules_meta)} rules")
    print(f"  include/secret_scan_ac.h ({state_count * 128 * 2} bytes transition table)")
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

    for rule in curated:
        vtype = classify_rule(rule)
        for kw in rule.get('keywords', []):
            kw_lower = kw.lower()
            if kw_lower in seen_keywords:
                continue
            seen_keywords.add(kw_lower)
            keywords.append(kw_lower)

            if vtype == VTYPE_PREFIX:
                tail_min, tail_max, charset = extract_tail_params(rule)
            else:
                tail_min, tail_max, charset = 10, 150, 3

            entropy = rule.get('entropy', 0.0)
            if vtype == VTYPE_KEYWORD and entropy == 0.0:
                entropy = 3.5  # default for contextual rules

            rules_meta.append({
                'id': rule['id'],
                'keyword': kw_lower,
                'vtype': vtype,
                'tail_min': tail_min,
                'tail_max': tail_max,
                'charset': charset,
                'entropy': entropy,
            })

    print(f"Keywords: {len(keywords)}")

    # Build AC automaton
    goto, failure, accept, state_count = build_ac(keywords)

    # Emit C headers
    emit_headers(keywords, rules_meta, goto, failure, accept, state_count)


if __name__ == '__main__':
    main()
