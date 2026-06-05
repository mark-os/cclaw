#!/bin/bash
# Test OpenRouter prompt caching + sticky routing across providers.
# Non-streaming requests to get full usage details including provider name.
# Usage: ./test_openrouter_caching.sh
set -euo pipefail

API_KEY="${OPENROUTER_API_KEY:?Set OPENROUTER_API_KEY}"
BASE="https://openrouter.ai/api/v1/chat/completions"
SESSION_ID="cache-test-$$"  # unique per run for sticky routing

# Large system prompt (~2000 tokens) to exceed all provider cache minimums
SYSPROMPT=$(python3 -c "
import sys
base = 'You are an expert Unix systems programmer. You have deep knowledge of POSIX standards, Linux kernel internals, process management, memory allocation, IPC mechanisms, signal handling, namespace isolation, cgroup resource control, seccomp filtering, and container runtimes. When answering questions, cite specific kernel versions, system call numbers, man page references, and relevant data structures from the kernel source. '
# Pad to ~2500 tokens
padding = 'The Unix philosophy emphasizes modularity, composability, and simplicity. Programs should do one thing well. Text streams are the universal interface. Small is beautiful. Make each program do one thing well. Build a prototype as soon as possible. Choose portability over efficiency. Store data in flat text files. Use software leverage to your advantage. Use shell scripts to increase leverage and portability. Avoid captive user interfaces. Make every program a filter. '
sys.stdout.write(base + padding * 12)
")

MODELS=("deepseek/deepseek-v4-flash" "openai/gpt-4o-mini" "google/gemini-2.5-flash")

echo "=== OpenRouter Cache + Provider Test ==="
echo "=== Session ID: $SESSION_ID (for sticky routing) ==="
echo ""

for MODEL in "${MODELS[@]}"; do
    echo "━━━ $MODEL ━━━"

    for TURN in 1 2 3 4; do
        START_NS=$(date +%s%N)

        RESP=$(curl -s "$BASE" \
            -H "Authorization: Bearer $API_KEY" \
            -H "Content-Type: application/json" \
            -H "x-session-id: $SESSION_ID-$MODEL" \
            -d "$(python3 -c "
import json, sys
msgs = [
    {'role': 'system', 'content': '''$SYSPROMPT'''},
    {'role': 'user', 'content': 'Say only: turn-$TURN'}
]
print(json.dumps({'model': '$MODEL', 'messages': msgs, 'max_tokens': 10}))
")")

        END_NS=$(date +%s%N)
        LATENCY_MS=$(( (END_NS - START_NS) / 1000000 ))

        echo "$RESP" | python3 -c "
import json, sys
r = json.load(sys.stdin)
if 'error' in r:
    print(f'  Turn $TURN: ERROR — {r[\"error\"]}')
    sys.exit(0)
u = r.get('usage', {})
ptd = u.get('prompt_tokens_details', {})
provider = r.get('provider', '?')
cached = ptd.get('cached_tokens', 0)
prompt = u.get('prompt_tokens', 0)
cost = u.get('cost', 0)
hit = f'{cached*100//prompt}%' if cached and prompt else '-'
print(f'  Turn $TURN: provider={provider:<15} prompt={prompt:<5} cached={cached:<5} hit={hit:<4} cost=\${cost:.6f}  latency=${LATENCY_MS}ms')
"
    done
    echo ""
done

# --- Also show what providers serve each model ---
echo "=== Provider routing analysis ==="
echo "(Which backend provider did OpenRouter choose for each model?)"
echo ""
for MODEL in "${MODELS[@]}"; do
    RESP=$(curl -s "$BASE" \
        -H "Authorization: Bearer $API_KEY" \
        -H "Content-Type: application/json" \
        -d "{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"max_tokens\":1}")
    PROVIDER=$(echo "$RESP" | python3 -c "import json,sys; print(json.load(sys.stdin).get('provider','?'))" 2>/dev/null)
    ACTUAL_MODEL=$(echo "$RESP" | python3 -c "import json,sys; print(json.load(sys.stdin).get('model','?'))" 2>/dev/null)
    echo "  $MODEL"
    echo "    → routed to: $PROVIDER (actual model: $ACTUAL_MODEL)"
done
echo ""
echo "TIP: To pin a provider, add to cclaw request body:"
echo '  "provider": {"order": ["deepseek"], "allow_fallbacks": false}'
