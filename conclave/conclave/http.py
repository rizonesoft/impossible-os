"""OpenRouter HTTP: chat completions, embeddings, and credit balance. Stdlib only."""
from __future__ import annotations

import json
import time
import urllib.request

BASE = "https://openrouter.ai/api/v1"
CHAT = f"{BASE}/chat/completions"
EMB = f"{BASE}/embeddings"
CREDITS = f"{BASE}/credits"


def _post(url: str, secret: str, payload: dict, timeout) -> dict:
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"),
        headers={"Authorization": f"Bearer {secret}", "Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def chat(secret, model, messages, timeout):
    t0 = time.time()
    d = _post(CHAT, secret, {"model": model, "messages": messages}, timeout)
    u = d.get("usage") or {}
    toks = {"prompt": int(u.get("prompt_tokens") or 0),
            "completion": int(u.get("completion_tokens") or 0)}
    return d["choices"][0]["message"]["content"], float(u.get("cost") or 0), time.time() - t0, toks


def embed(secret, model, inputs, timeout):
    d = _post(EMB, secret, {"model": model, "input": inputs}, timeout)
    vecs = [row["embedding"] for row in d.get("data", [])]
    return vecs, float((d.get("usage") or {}).get("cost") or 0)


def remaining_credits(secret, timeout=10) -> float:
    req = urllib.request.Request(CREDITS, headers={"Authorization": f"Bearer {secret}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = (json.loads(r.read().decode("utf-8")).get("data") or {})
    return float(d.get("total_credits", 0)) - float(d.get("total_usage", 0))
