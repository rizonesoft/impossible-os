#!/usr/bin/env python3
# block-via: exit 2
"""Refuse a Codex REVIEW dispatch whose prompt the recorder cannot attribute.

Why
---
The review recorder (`codex_review_completed.py`) attributes a dispatch from the
COMMAND LINE: the `[review-kind: X]` marker must be literal text at the start of
the prompt argument. A prompt passed as `"$(cat /tmp/p.txt)"`, as `"$P"`, or built
inside a `for k in ...; do ... "[review-kind: $k] ..."` loop expands only at run
time, so the dispatch RUNS, Codex reviews, and the recorder stores nothing.

Measured 2026-09-05 (`run-20260905-143453.log:358-376`): a five-leg wave sent as
`"$(cat /tmp/w4-<kind>.txt)"` all returned `approve`, the per-kind stamps still
named the previous section, and all five legs were re-dispatched and waited on
again -- 11 calls and one full Codex wave for work worth 6. Refusing before the
round trip turns that into one corrected call.

Recognition reuses the recorder's own machinery (`_codex_dispatch` +
`_review_kind`), so this gate refuses exactly the dispatches the recorder would
record with no kind, and can never disagree with it. The canonical heredoc shape
(`PROMPT=$(cat <<'EOF' ... EOF)` then `"$PROMPT"`) resolves through that
machinery and is allowed.

Scope: review wrappers only -- a basename ending in `codex-dispatch.sh` (the
direct wrapper and the broker) or `codex-companion.mjs adversarial-review`.
Rescue (`codex ... task --write`) and other Codex use are not review evidence and
are left alone. Opt-out: REVIEW_KIND_LITERAL_OVERRIDE=1 on the same call.
"""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import _codex_dispatch as _cd
    from _review_kind import _REVIEW_KIND_RE
except Exception:  # noqa: BLE001 -- a helper import failure must never gate work
    _cd = None
    _REVIEW_KIND_RE = None


# Shell words that can lead a segment before the executed program (`for ...;
# do bash x.sh`, `if ...; then ...`, `{ ...; }`, `! cmd`, `time cmd`).
_KEYWORDS = {"do", "then", "else", "elif", "{", "(", "!", "time"}


def _review_dispatch_count(cmd: str) -> int:
    n = 0
    try:
        for seg in _cd._segment_by_separators(_cd._tokenize(_cd._harvest_heredoc_vars(cmd)[0])):
            seg = _cd._trim_heredoc_body(_cd._strip_env_and_wrappers(list(seg)))
            while seg and seg[0] in _KEYWORDS:
                seg = seg[1:]
            if not seg:
                continue
            # The EXECUTED program, not any token: `grep codex-dispatch.sh f` is
            # a search. `bash|sh <script>` and `node <script> <subcommand>`.
            argv = seg[1:] if os.path.basename(seg[0]) in ("bash", "sh") else seg
            if argv and os.path.basename(argv[0]).endswith("codex-dispatch.sh"):
                n += 1
            elif (len(seg) >= 3 and os.path.basename(seg[0]) == "node"
                    and os.path.basename(seg[1]) == "codex-companion.mjs"
                    and seg[2] == "adversarial-review"):
                n += 1
    except Exception:  # noqa: BLE001
        return 0
    return n


def unattributable_prompts(cmd: str) -> list:
    """Why review dispatches in `cmd` would be recorded with no kind. Empty list =
    allow. The shared extractor returns NOTHING for a `$(...)` argument, so a bad
    leg beside a good one is caught by COUNTING: every review-dispatch segment
    needs its own prompt whose first non-blank line carries the marker (the
    recorder's exact rule)."""
    if _cd is None or _REVIEW_KIND_RE is None:
        return []
    n = _review_dispatch_count(cmd)
    if not n:
        return []
    bad, ok = [], 0
    for body in _cd.extract_dispatch_prompts(cmd) or []:
        first = next((ln.strip() for ln in (body or "").splitlines() if ln.strip()), "")
        if _REVIEW_KIND_RE.search(first):
            ok += 1
        else:
            bad.append(first[:80] or "<empty prompt>")
    if ok < n and not bad:
        bad.append("%d of %d review dispatch(es) have no literal prompt (a $(...) or "
                   "$VAR argument the recorder cannot read)" % (n - ok, n))
    return bad


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    if os.environ.get("REVIEW_KIND_LITERAL_OVERRIDE") == "1":
        return 0
    cmd = str((d.get("tool_input") or {}).get("command") or "")
    bad = unattributable_prompts(cmd)
    if not bad:
        return 0
    sys.stderr.write(
        "[review-kind-literal] BLOCK -- a Codex review dispatch whose prompt does not "
        "START with a literal [review-kind: X] marker: %s\n" % "; ".join(repr(b) for b in bad)
    )
    sys.stderr.write(
        "[review-kind-literal] why: the recorder reads the marker from the command "
        "line. \"$(cat file)\", \"$VAR\" and loop variables expand only at run time, "
        "so Codex would review and NOTHING would be recorded -- the wave is then "
        "re-dispatched (measured 2026-09-05: 5 approved legs thrown away).\n"
    )
    sys.stderr.write(
        "[review-kind-literal] use: one call per kind, the prompt single-quoted "
        "inline:\n    bash scripts/overnight/review-broker-codex-dispatch.sh "
        "'[review-kind: <kind>] <todo-path> <body>'\n"
        "  (a long prompt may use the heredoc shape: PROMPT=$(cat <<'EOF' ... EOF) "
        "then \"$PROMPT\"). Opt-out: REVIEW_KIND_LITERAL_OVERRIDE=1.\n"
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
