"""The panel: probe one model, run many in parallel. The teachers."""
from __future__ import annotations

import concurrent.futures

from conclave import http


def probe(secret, model, sys_prompt, brief, timeout) -> dict:
    try:
        content, cost, sec, toks = http.chat(
            secret, model,
            [{"role": "system", "content": sys_prompt}, {"role": "user", "content": brief}],
            timeout)
        return {"model": model, "status": "ok", "content": content, "cost": cost,
                "sec": round(sec, 1), "chars": len(content),
                "prompt_tokens": toks["prompt"], "completion_tokens": toks["completion"]}
    except Exception as e:
        return {"model": model, "status": "error", "content": "", "cost": 0, "sec": 0,
                "chars": 0, "prompt_tokens": 0, "completion_tokens": 0, "err": str(e)[:160]}


def run(secret, models, sys_prompt, brief, timeout, workers) -> list:
    if not models:
        return []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers or len(models)) as ex:
        return list(ex.map(lambda m: probe(secret, m, sys_prompt, brief, timeout), models))
