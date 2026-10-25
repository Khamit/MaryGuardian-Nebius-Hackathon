#!/usr/bin/env python3
"""Summarise the outbound ledger as Markdown (for the hackathon feedback field).

Usage: python3 scripts/outbound_stats.py [path/to/outbound.jsonl]

Latency is measured by the Guardian gateway around one model call and therefore
includes retries and back-off performed inside the remote client.
"""
import json
import math
import sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "mg_workspace/logs/outbound.jsonl"

rows = []
with open(path, encoding="utf-8") as f:
    for line in f:
        try:
            rows.append(json.loads(line))
        except ValueError:
            pass


def pct(values, p):
    if not values:
        return 0
    s = sorted(values)
    return s[max(0, math.ceil(p / 100 * len(s)) - 1)]


def mean(values):
    return sum(values) / len(values) if values else 0


sent = [r for r in rows if r.get("status") == "sent"]
blocked = sum(1 for r in rows if r.get("status") == "blocked")
failed = sum(1 for r in rows if r.get("status") not in ("sent", "blocked"))
masked = sum(r.get("masked_total", 0) for r in rows)
tok_in = sum(r.get("prompt_tokens", 0) for r in sent)
tok_out = sum(r.get("completion_tokens", 0) for r in sent)

print(f"Requests sent: {len(sent)}, blocked by Guardian: {blocked}, failed: {failed}. "
      f"Values replaced by placeholders: {masked}. Tokens: {tok_in} in / {tok_out} out.\n")


def table(title, key_fn, headers):
    groups = defaultdict(list)
    for r in sent:
        groups[key_fn(r)].append(r)
    print(f"**{title}**\n")
    print("| " + " | ".join(headers) + " |")
    print("|" + "---|" * len(headers))
    for key in sorted(groups):
        g = groups[key]
        lat = [r.get("latency_ms", 0) for r in g]
        label = " | ".join(key) if isinstance(key, tuple) else key
        print(f"| {label} | {len(g)} | {pct(lat, 50)} | {pct(lat, 95)} | "
              f"{mean([r.get('prompt_tokens', 0) for r in g]):.0f} | "
              f"{mean([r.get('completion_tokens', 0) for r in g]):.0f} |")
    print()


cols = ["Calls", "p50 ms", "p95 ms", "avg prompt tokens", "avg completion tokens"]
table("By tier", lambda r: (r.get("tier", "?"), r.get("model", "?")), ["Tier", "Model"] + cols)
table("By purpose", lambda r: r.get("purpose", "?"), ["Purpose"] + cols)