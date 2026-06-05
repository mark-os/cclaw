#!/usr/bin/env python3
"""Find models at least as good as Gemini 2.5 Flash Lite from US providers.

Scans all OpenRouter models, fetches endpoint stats for each, averages
throughput/latency/cost across US providers only, and prints models that
meet or beat the Gemini 2.5 Flash Lite baseline.

This is a metadata enumeration tool — no inference requests are sent.
"""

import json, os, sys, urllib.request

API_KEY = os.environ.get("OPENROUTER_API_KEY")
if not API_KEY:
    sys.exit("Set OPENROUTER_API_KEY")

BASE = "https://openrouter.ai/api/v1"

# ┌──────────────────────────────────────────────────────────────────────┐
# │ USAGE ASSUMPTIONS                                                    │
# └──────────────────────────────────────────────────────────────────────┘
HOURS_PER_DAY = 8
DAYS_PER_MONTH = 22
TURNS_PER_HOUR = 30
INPUT_TOKENS_PER_TURN = 4000
OUTPUT_TOKENS_PER_TURN = 800
CACHE_HIT_RATE = 0.80

TURNS_PER_DAY = TURNS_PER_HOUR * HOURS_PER_DAY
INPUT_PER_DAY = INPUT_TOKENS_PER_TURN * TURNS_PER_DAY
OUTPUT_PER_DAY = OUTPUT_TOKENS_PER_TURN * TURNS_PER_DAY

# ┌──────────────────────────────────────────────────────────────────────┐
# │ BASELINE — at most this expensive                                    │
# └──────────────────────────────────────────────────────────────────────┘
BASELINE_MONTHLY_COST = 3.00    # dollars (Gemini 2.5 Flash Lite ≈ $2.28)
BASELINE_CONTEXT = 32000        # minimum context window
REQUIRE_TOOLS = True
REQUIRE_CACHE = True

# ┌──────────────────────────────────────────────────────────────────────┐
# │ US PROVIDER DETECTION                                                │
# └──────────────────────────────────────────────────────────────────────┘
CN_SLUGS = {"deepseek", "streamlake", "baidu", "siliconflow", "alibaba",
            "volc-engine", "minimax", "zhipu-ai", "stepfun", "tencent", "xiaomi"}
EU_SLUGS = {"nebius", "mistral", "scaleway"}

def is_us_provider(name):
    slug = name.lower().replace(" ", "-")
    if slug in CN_SLUGS or slug in EU_SLUGS:
        return False
    return True  # assume US/unknown = acceptable

def api_get(path):
    req = urllib.request.Request(f"{BASE}{path}", headers={
        "Authorization": f"Bearer {API_KEY}",
        "Content-Type": "application/json",
    })
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read())

def monthly_cost(prompt_pt, cache_pt, comp_pt):
    cached_in = INPUT_PER_DAY * CACHE_HIT_RATE * cache_pt
    uncached_in = INPUT_PER_DAY * (1 - CACHE_HIT_RATE) * prompt_pt
    out = OUTPUT_PER_DAY * comp_pt
    return (cached_in + uncached_in + out) * DAYS_PER_MONTH

def main():
    print("Fetching models...", file=sys.stderr)
    models = api_get("/models")["data"]

    # Step 1: filter models by basic criteria
    candidates = []
    for m in models:
        arch = m.get("architecture", {})
        if not (arch.get("modality") or "").endswith("->text"):
            continue
        if (m.get("context_length") or 0) < BASELINE_CONTEXT:
            continue
        params = m.get("supported_parameters") or []
        if REQUIRE_TOOLS and "tools" not in params:
            continue
        p = m.get("pricing", {})
        cache_read = p.get("input_cache_read")
        if REQUIRE_CACHE and (not cache_read or float(cache_read) == 0):
            continue

        prompt_pt = float(p.get("prompt", "999"))
        comp_pt = float(p.get("completion", "999"))
        cache_pt = float(cache_read) if cache_read else prompt_pt

        cost = monthly_cost(prompt_pt, cache_pt, comp_pt)
        if cost > BASELINE_MONTHLY_COST:
            continue

        candidates.append({
            "id": m["id"],
            "context": m.get("context_length", 0),
            "prompt_m": prompt_pt * 1e6,
            "comp_m": comp_pt * 1e6,
            "cache_m": cache_pt * 1e6,
            "monthly": cost,
            "endpoints_path": f"/models/{m.get('canonical_slug') or m['id']}/endpoints",
        })

    print(f"Found {len(candidates)} models under ${BASELINE_MONTHLY_COST:.0f}/mo with tools+cache. Fetching endpoints...", file=sys.stderr)

    # Step 2: for each candidate, fetch US provider stats
    results = []
    for c in candidates:
        try:
            data = api_get(c["endpoints_path"])
        except Exception:
            # Try alternate path
            try:
                data = api_get(f"/models/{c['id']}/endpoints")
            except Exception:
                continue

        endpoints = (data.get("data") or data).get("endpoints", [])
        us_eps = [ep for ep in endpoints if is_us_provider(ep.get("provider_name", ""))]
        if not us_eps:
            continue

        # Average stats across US providers (only those reporting data)
        tputs = [ep["throughput_last_30m"]["p50"] for ep in us_eps
                 if ep.get("throughput_last_30m") and ep["throughput_last_30m"].get("p50")]
        lats = [ep["latency_last_30m"]["p50"] for ep in us_eps
                if ep.get("latency_last_30m") and ep["latency_last_30m"].get("p50")]
        uptimes = [ep.get("uptime_last_30m", 0) for ep in us_eps if ep.get("uptime_last_30m")]

        if not tputs or not lats:
            avg_tp = None
            avg_lat = None
        else:
            avg_tp = sum(tputs) / len(tputs)
            avg_lat = sum(lats) / len(lats)
        avg_up = sum(uptimes) / len(uptimes) if uptimes else 0

        results.append({
            **c,
            "avg_tp": avg_tp,
            "avg_lat": avg_lat,
            "avg_up": avg_up,
            "us_providers": len(us_eps),
        })

    # Sort by monthly cost
    results.sort(key=lambda x: x["monthly"])

    # Print
    print(f"\n  Models ≤${BASELINE_MONTHLY_COST:.0f}/mo, tools+cache, ≥{BASELINE_CONTEXT//1000}K context, US providers — avg stats")
    print(f"  {'═'*95}")
    print(f"  {'Model':<40} {'$/mo':<7} {'Prompt$/M':<10} {'Cache$/M':<9} {'Comp$/M':<8} {'Tput':<7} {'Lat':<8} {'Up':<5} {'Ctx':<6} {'#US'}")
    print(f"  {'─'*40} {'─'*7} {'─'*10} {'─'*9} {'─'*8} {'─'*7} {'─'*8} {'─'*5} {'─'*6} {'─'*3}")
    for r in results:
        ctx = f"{r['context']//1024}K"
        tp_str = f"{r['avg_tp']:.0f} t/s" if r['avg_tp'] else "—"
        lat_str = f"{r['avg_lat']:.0f}ms" if r['avg_lat'] else "—"
        up_str = f"{r['avg_up']:.0f}%" if r['avg_up'] else "—"
        print(f"  {r['id']:<40} ${r['monthly']:<5.2f} ${r['prompt_m']:.4f}  ${r['cache_m']:.4f} ${r['comp_m']:.3f} {tp_str:<7} {lat_str:<8} {up_str:<5} {ctx:<6} {r['us_providers']}")

    print(f"\n  Assumptions: {TURNS_PER_DAY} turns/day, {INPUT_TOKENS_PER_TURN} in + {OUTPUT_TOKENS_PER_TURN} out/turn, {CACHE_HIT_RATE:.0%} cache hit, {DAYS_PER_MONTH} days/mo")
    print(f"  Filter: ≤${BASELINE_MONTHLY_COST}/mo, ≥{BASELINE_CONTEXT//1000}K context, tools+cache required, US providers only")

if __name__ == "__main__":
    main()
