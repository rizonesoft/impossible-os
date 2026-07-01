"""The discard-synthesis judge -- the thing that makes the ensemble beat one model."""
from __future__ import annotations

from conclave import http

JUDGE_SYS = (
    "You are the judge of an expert panel. You are given several independent "
    "analyses of the same hard problem (some may be prior analyses from the primary "
    "agent and its assistant). Synthesize: consensus, contradictions, any UNIQUE "
    "correct insight a single analysis had that the others missed, and blind spots. "
    "Explicitly call out and DISCARD any fabricated errata numbers, microcode "
    "revisions, or undocumented MSRs -- keep only verifiable facts. Then commit to "
    "the single most likely root cause and the concrete fix.")


def judge(secret, judge_model, brief, panel_results, prior, timeout):
    parts = []
    for p in (prior or []):
        parts.append(f"### {p.get('source', 'prior')} (prior tier analysis)\n{p.get('content', '')}")
    for r in panel_results:
        if r.get("status") == "ok" and r.get("content", "").strip():
            parts.append(f"### {r['model']}\n{r['content']}")
    blob = "\n\n".join(parts)
    msgs = [{"role": "system", "content": JUDGE_SYS},
            {"role": "user", "content": f"PROBLEM:\n{brief}\n\nANALYSES:\n{blob}"}]
    return http.chat(secret, judge_model, msgs, timeout)
