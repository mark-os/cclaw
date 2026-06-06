#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["requests>=2.31"]
# ///
"""Benchmark OpenRouter providers: throughput, latency, caching, cost.

Phase 1: Each model × each US provider → ~1500 tok input, ~500 tok output → throughput.
Phase 2: Top 5 by throughput → turns 2-5 (short output) → latency, cache hits, cost.

Non-streaming. Sends cache_control for providers that need explicit breakpoints.
Latency on short responses ≈ TTFB since generation is trivial (~10 tokens).

Usage: uv run bench_providers.py
"""

import json, os, sys, time, urllib.request
import requests as req

API_KEY = os.environ.get("OPENROUTER_API_KEY")
if not API_KEY:
    sys.exit("Set OPENROUTER_API_KEY")

# ┌──────────────────────────────────────────────────────────────────────┐
# │ CONFIG                                                               │
# └──────────────────────────────────────────────────────────────────────┘
MODELS = [
    "ibm-granite/granite-4.1-8b",
    "deepseek/deepseek-v4-flash",
    "openai/gpt-5-nano",
    "google/gemini-2.5-flash-lite",
]

TOP_N = 5

US_PROVIDERS = {
    "amazon-bedrock", "amazon-nova", "anthropic", "arcee-ai", "atlas-cloud",
    "avian", "azure", "baseten", "cerebras", "chutes", "clarifai",
    "cloudflare", "cohere", "crusoe", "deepinfra", "featherless", "fireworks",
    "friendli", "gmicloud", "google-ai-studio", "google-vertex", "groq",
    "hyperbolic", "inception", "inference-net", "ionstream", "io-net", "mara",
    "modelrun", "modular", "morph", "novita", "nvidia", "open-inference",
    "openai", "parasail", "perceptron", "perplexity", "phala", "sambanova",
    "switchpoint", "together", "venice", "wandb", "xai",
}

BASE = "https://openrouter.ai/api/v1"
HEADERS = {"Authorization": f"Bearer {API_KEY}", "Content-Type": "application/json"}

SYSTEM_MSG = "You are a concise technical assistant."

USER_MSG_TURN1 = (
    "Explain the complete history of Unix process management. "
    "Start with Research Unix V1 in 1971 and the original fork() system call by Ken Thompson. "
    "Cover the evolution through V6 and V7 Unix, the addition of signals, BSD job control in 4.1cBSD, "
    "System V Release 4 unification, Linux 0.01 in 1991, the clone() syscall in Linux 2.0, "
    "the O(1) scheduler, NPTL threads, CFS in 2.6.23, namespaces from mount (2.4.19) through "
    "user (3.8), cgroups v1 in 2.6.24 and v2 in 4.5, seccomp-bpf in 3.5, and how Docker "
    "composes these primitives. Include kernel version numbers for each feature. "
    "Cover at least: task_struct evolution, signal handling (V2 through signalfd), "
    "process groups and sessions, the /proc filesystem, real-time scheduling classes, "
    "the freezer cgroup, PID namespaces with nested init, and clone3() in 5.3. "
    "Explain how vfork() relates to clone(CLONE_VFORK|CLONE_VM), how NPTL uses "
    "clone(CLONE_THREAD|CLONE_SIGHAND|CLONE_VM), and how container runtimes use "
    "clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWNET|CLONE_NEWUSER). "
    "Discuss the pivot_root vs chroot distinction for mount namespaces, "
    "the user namespace UID mapping via /proc/pid/uid_map, and network namespace "
    "veth pair setup. Cover the seccomp notify mechanism in 5.9 for user-space "
    "syscall emulation. Finish with clone3() struct clone_args extensibility. "
    "Write approximately 500 words."
)

SHORT_PROMPTS = ["Reply with only: turn-2", "Reply with only: turn-3",
                 "Reply with only: turn-4", "Reply with only: turn-5"]


def get_us_providers_for_model(model_id):
    r = req.get(f"{BASE}/models/{model_id}/endpoints", headers=HEADERS)
    if r.status_code != 200:
        print(f"  Error fetching endpoints: {r.status_code}")
        return []
    endpoints = r.json().get("data", r.json()).get("endpoints", [])
    providers = []
    seen = set()
    for ep in endpoints:
        tag = ep.get("tag") or ""
        slug = tag.split("/")[0]
        if slug not in US_PROVIDERS:
            continue
        suffixes = tag.split("/")[1:]
        if any(s.startswith(("eu", "europe", "asia", "ap-")) for s in suffixes):
            continue
        if tag in seen:
            continue
        seen.add(tag)
        providers.append({"name": ep.get("provider_name", slug), "tag": tag,
                          "quant": ep.get("quantization") or "—"})
    return providers


def send(model, messages, provider_tag, session_id, max_tokens=512):
    """Non-streaming request with cache_control. Returns stats dict."""
    body = {
        "model": model,
        "messages": messages,
        "max_tokens": max_tokens,
        "cache_control": {"type": "ephemeral"},
        "provider": {"order": [provider_tag], "allow_fallbacks": False},
    }
    t0 = time.time()
    try:
        r = req.post(f"{BASE}/chat/completions", headers={**HEADERS, "x-session-id": session_id},
                     json=body, timeout=120)
        elapsed = time.time() - t0
        if r.status_code != 200:
            err = r.json().get("error", {}).get("message", r.text[:80])
            return {"error": err[:80], "elapsed": elapsed}
        data = r.json()
        usage = data.get("usage", {})
        ptd = usage.get("prompt_tokens_details", {})
        comp = usage.get("completion_tokens", 0)
        content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
        return {
            "prompt": usage.get("prompt_tokens", 0),
            "completion": comp,
            "cached": ptd.get("cached_tokens", 0) or 0,
            "cost": usage.get("cost", 0) or 0,
            "elapsed": elapsed,
            "throughput": comp / elapsed if elapsed > 0 else 0,
            "content": content,
            "provider": data.get("provider", "?"),
        }
    except Exception as e:
        return {"error": str(e)[:80], "elapsed": time.time() - t0}


def main():
    print("═══════════════════════════════════════════════════════════════════════")
    print("  Provider Benchmark — US providers, cache_control enabled")
    print("═══════════════════════════════════════════════════════════════════════")

    for model in MODELS:
        print(f"\n{'━'*70}")
        print(f"  {model}")
        print(f"{'━'*70}")

        providers = get_us_providers_for_model(model)
        if not providers:
            print("  No US providers found")
            continue

        # ── Phase 1: Throughput ──
        print(f"\n  Phase 1: Throughput ({len(providers)} providers)")
        print(f"  {'Provider':<20} {'Quant':<7} {'Out':<6} {'Time':<8} {'Tok/s':<8} {'Cost'}")
        print(f"  {'─'*20} {'─'*7} {'─'*6} {'─'*8} {'─'*8} {'─'*10}")

        msgs_t1 = [{"role": "system", "content": SYSTEM_MSG},
                   {"role": "user", "content": USER_MSG_TURN1}]
        results = []

        for p in providers:
            sid = f"bench-{model.split('/')[-1]}-{p['tag']}-{int(time.time())}"
            r = send(model, msgs_t1, p["tag"], sid, max_tokens=512)
            if "error" in r:
                print(f"  {p['name']:<20} {p['quant']:<7} {'ERR':<6} {r['elapsed']:.1f}s   —       {r['error'][:40]}")
                continue
            print(f"  {p['name']:<20} {p['quant']:<7} {r['completion']:<6} {r['elapsed']:.1f}s   {r['throughput']:.0f} t/s  ${r['cost']:.6f}")
            results.append({**p, **r, "session_id": sid, "messages": msgs_t1})

        results.sort(key=lambda x: x["throughput"], reverse=True)
        top = results[:TOP_N]
        if not top:
            continue

        # ── Phase 2: Latency + Cache ──
        print(f"\n  Phase 2: Latency + Cache (top {len(top)} by throughput, turns 2-5)")
        print(f"  {'Provider':<20} {'T':<3} {'Prompt':<7} {'Cached':<7} {'Hit%':<6} {'Latency':<9} {'Tput':<8} {'Cost'}")
        print(f"  {'─'*20} {'─'*3} {'─'*7} {'─'*7} {'─'*6} {'─'*9} {'─'*8} {'─'*10}")

        for p in top:
            msgs = list(p["messages"]) + [{"role": "assistant", "content": p["content"]}]
            for i, prompt in enumerate(SHORT_PROMPTS):
                msgs.append({"role": "user", "content": prompt})
                r = send(model, msgs, p["tag"], p["session_id"], max_tokens=32)
                if "error" in r:
                    print(f"  {p['name']:<20} {i+2:<3} {'ERR':<7} {'':7} {'':6} {r['elapsed']*1000:.0f}ms{'':4} {'':8} {r['error'][:30]}")
                    msgs.append({"role": "assistant", "content": "(error)"})
                    continue
                msgs.append({"role": "assistant", "content": r["content"]})
                hit = f"{r['cached']*100//r['prompt']}%" if r['cached'] > 0 else "—"
                print(f"  {p['name']:<20} {i+2:<3} {r['prompt']:<7} {r['cached']:<7} {hit:<6} {r['elapsed']*1000:.0f}ms{'':4} {r['throughput']:.0f} t/s  ${r['cost']:.6f}")
            print()

    print("Done.")


if __name__ == "__main__":
    main()
