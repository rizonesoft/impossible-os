#!/usr/bin/env python3
# block-via: exit 2 (main-session Read only; subagents are never gated)
"""PreToolUse (Read): content-hash read cache -- BLOCK a redundant re-read.

MEASURED WASTE (2026-07-27, todo/token-saver/token-saver-v01.md T1-1): across one working
period the main session issued 1,412 Read calls over 180 DISTINCT files --
1,232 of them (87%) re-reads of a file already in context.
src/kernel/quota/quota.c was read 131 times, quota.h 100, the active TODO 93,
task.c 91. A re-read is not a linear cost: it re-appends the whole body AND
every later turn then pays cache-read on BOTH copies in the cached prefix.
Cache-read is ~81% of run spend (1.337B tokens vs 3.43M output), so this is
the single largest cost item in the repo's token budget.

`slice_read_reminder.py` has warned about this shape since 2026-07-02 and the
131x re-read happened anyway -- the advisory does not bite. This hook promotes
it to a BLOCK with a pointer.

WHAT IS BLOCKED: a Read whose byte range is ALREADY COVERED by an earlier read
in the SAME session, of a file whose content hash is UNCHANGED since that read.
Nothing else.

WHY THE LEGITIMATE RE-READS STAY FREE (by construction, not by exception):
  - after an Edit/Write        -- the content hash changed, so no entry matches
  - after a FAILED Edit        -- `invalidate` mode drops the path (the model's
                                  in-context copy is provably stale)
  - after a compaction         -- pre_compact_flush.py deletes the state file
                                  (the content is genuinely gone from context)
  - in a subagent              -- separate context; subagent Reads pass through
  - in a new session/rollover  -- state is session-bound and resets
  - a WIDER or non-overlapping slice -- not covered, so not blocked
  - a file under MIN_BYTES     -- the saving would not pay for the friction

ANTI-WEDGE: a hook that can stop the model reading must never be able to trap
it. Three independent valves: (1) the same request is blocked at most
MAX_BLOCKS times, then allowed -- insistence is treated as evidence the block
is wrong; (2) one-shot file override `.claude/state/read-cache-override`;
(3) kill switch `READ_CACHE_DISABLE=1`. Fail-open on every error path.

Modes:
    (default)     PreToolUse  Read              -- gate + record
    invalidate    PostToolUseFailure Edit|MultiEdit -- drop the path's entries

Code: [READ-CACHED] -- docs/infrastructure/hook-codes.md
Selftest: python3 read_cache_block.py --selftest
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
import time
from pathlib import Path

STATE_REL = ".claude/state/read-cache.json"
OVERRIDE_REL = ".claude/state/read-cache-override"

MIN_BYTES = 2048            # below this a re-read is not worth the friction
HASH_MAX_BYTES = 8 << 20    # above this, fall back to a size+mtime digest
DEFAULT_LIMIT = 2000        # the Read tool's default line budget
MAX_BLOCKS = 2              # consecutive blocks on one request before relief
MAX_ENTRIES = 256           # bounded state

# Read renders these specially (image render / PDF `pages` / notebook cells),
# so "already in context" does not have the same meaning. Never gated.
SPECIAL_SUFFIXES = frozenset({
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".ico", ".tiff",
    ".pdf", ".ipynb",
})

_MSG = (
    "[READ-CACHED BLOCK] {path} lines {a}-{b} are ALREADY in this session's "
    "context -- read #{seq}, {ago} ago, and the file is byte-identical since "
    "(sha256 match). Scroll up and reuse that copy. A second copy costs the "
    "body once AND is re-read in the cached prefix on every later turn; "
    "cache-read is ~81% of run spend and 87% of this repo's Reads were "
    "measured redundant (quota.c x131, 2026-07-27, todo/token-saver/token-saver-v01.md T1-1). "
    "Legitimate re-reads are NOT gated and need no action from you: after an "
    "edit to this file, after a failed Edit on it, after a compaction, inside "
    "a subagent, or for a wider/non-overlapping slice. If you need a DIFFERENT "
    "region, pass offset/limit for it -- that is allowed. If you are certain "
    "this exact read is needed anyway, repeat it: the {maxb}-block limit "
    "releases it (attempt {n} of {maxb}). Kill switch: READ_CACHE_DISABLE=1."
)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _state_path() -> Path:
    return _repo_root() / STATE_REL


def _is_subagent_transcript(path: str) -> bool:
    """Delegate to runner_bash_guard's battle-tested detector.

    Single source of truth -- a private copy would silently diverge the next
    time the transcript-naming heuristic is hardened (same reuse finding that
    shaped websearch_offload_gate). The fallback only runs if the import
    itself breaks, and it fails toward "MAIN session", which is the gated
    path; the MAX_BLOCKS valve keeps even that misclassification harmless.
    """
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runner_bash_guard
        return bool(runner_bash_guard._is_subagent_transcript(path))
    except Exception:
        if not path:
            return False
        base = os.path.basename(path)
        return base.startswith("agent-") or "/subagents/" in path


def _is_subagent_payload(d: dict) -> bool:
    """Payload-level subagent identity, delegated to the shared detector.

    Separate from `_is_subagent_transcript` because they fail differently: the
    transcript heuristic sees the PARENT's path inside a subagent and returns
    False, while this reads the harness's own agent identity fields.
    """
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runner_bash_guard
        return bool(runner_bash_guard.is_subagent_payload(d))
    except Exception:
        return False


def _digest(fp: str, size: int) -> tuple[str, int]:
    """Return (digest, line_count). line_count is 0 when unknown.

    Files over HASH_MAX_BYTES use a cheap size+mtime digest rather than
    hashing tens of MB on every Read; their line count is unknown, so only an
    EXACT key match can block them (coverage math needs the line count).
    """
    if size > HASH_MAX_BYTES:
        return f"sm:{size}:{os.path.getmtime(fp):.0f}", 0
    with open(fp, "rb") as fh:
        data = fh.read()
    lines = data.count(b"\n") + (1 if data and not data.endswith(b"\n") else 0)
    return "sha256:" + hashlib.sha256(data).hexdigest(), lines


def _requested_range(ti: dict, total_lines: int) -> tuple[int, int]:
    """Map a Read tool_input to the inclusive line range it will return."""
    try:
        start = int(ti.get("offset") or 1)
    except Exception:
        start = 1
    start = max(1, start)
    limit = ti.get("limit")
    try:
        span = int(limit) if limit is not None else DEFAULT_LIMIT
    except Exception:
        span = DEFAULT_LIMIT
    span = max(1, span)
    end = start + span - 1
    if total_lines:
        end = min(end, max(total_lines, start))
    return start, end


def _load(sp: Path, session_id: str) -> dict:
    """Load session-scoped state; a different session starts empty.

    Session binding is what makes a rollover / new headless segment safe: the
    next session has an empty context AND an empty table, so nothing it needs
    is ever withheld.
    """
    try:
        st = json.loads(sp.read_text())
        if not isinstance(st, dict):
            raise ValueError
    except Exception:
        st = {}
    if st.get("session_id") != session_id:
        return {"session_id": session_id, "seq": 0, "entries": {}}
    st.setdefault("seq", 0)
    if not isinstance(st.get("entries"), dict):
        st["entries"] = {}
    return st


def _save(sp: Path, st: dict) -> None:
    entries = st.get("entries") or {}
    if len(entries) > MAX_ENTRIES:
        drop = sorted(entries.items(),
                      key=lambda kv: kv[1].get("ts", 0))[: len(entries) - MAX_ENTRIES]
        for k, _ in drop:
            entries.pop(k, None)
    try:
        sp.parent.mkdir(parents=True, exist_ok=True)
        tmp = sp.with_suffix(sp.suffix + ".tmp")
        tmp.write_text(json.dumps(st))
        os.replace(tmp, sp)
    except Exception:
        pass


def _ago(ns: int) -> str:
    s = max(0, ns) // 1_000_000_000
    if s < 90:
        return f"{s}s"
    if s < 5400:
        return f"{s // 60}m"
    return f"{s // 3600}h"


def _invalidate() -> int:
    """PostToolUseFailure(Edit|MultiEdit): drop every entry for that path.

    A FAILED edit is the one signal that the in-context copy is stale in a way
    the content hash cannot see (the file did not change -- the model's memory
    of it was wrong). Freeing the re-read here is what keeps the acceptance
    criterion true: the block never stands in the way of a post-Edit re-read.
    """
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    fp = str(((d.get("tool_input") or {}).get("file_path")) or "")
    if not fp:
        return 0
    sp = _state_path()
    try:
        st = json.loads(sp.read_text())
        if not isinstance(st, dict) or not isinstance(st.get("entries"), dict):
            return 0
    except Exception:
        return 0
    keep = {k: v for k, v in st["entries"].items() if v.get("path") != fp}
    if len(keep) != len(st["entries"]):
        st["entries"] = keep
        _save(sp, st)
    return 0


def main() -> int:
    if os.environ.get("READ_CACHE_DISABLE") == "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Read":
        return 0
    ti = d.get("tool_input") or {}
    if not isinstance(ti, dict):
        return 0
    fp = str(ti.get("file_path") or "")
    if not fp or ti.get("pages") is not None:
        return 0
    if Path(fp).suffix.lower() in SPECIAL_SUFFIXES:
        return 0

    # One-shot operator override: consume it and let this Read through.
    ov = _repo_root() / OVERRIDE_REL
    try:
        if ov.exists():
            ov.unlink()
            return 0
    except Exception:
        pass

    # A subagent has its own context -- what the main session read is not in
    # it. Gating here would starve the analysts of the reads they exist to do.
    #
    # BOTH detectors, because the transcript-path one is NOT sufficient inside a
    # subagent: the PreToolUse payload delivered there carries the PARENT's
    # `transcript_path`, so the agent's read was recorded as the main session's
    # and the main session was then blocked from reading the same lines. That
    # inverts the trust contract -- doctrine REQUIRES the main session to verify
    # every load-bearing agent claim at file:line, and this gate was refusing
    # exactly that verification. Measured 2026-08-06: a section-context-mapper
    # read of identity-gate.sh blocked the very next main-session Read of it.
    #
    # `is_subagent_payload` (runner_bash_guard, 2026-07-31) reads the harness's
    # own identity fields and was added for this same misclassification in a
    # different hook. A False from it means "no marker", not "main session", so
    # the two are OR-ed: this gate's harm direction is blocking the main
    # session, so it must fail toward NOT gating.
    if _is_subagent_transcript(str(d.get("transcript_path") or "")) \
            or _is_subagent_payload(d):
        return 0

    try:
        size = os.path.getsize(fp)
    except Exception:
        return 0          # missing / unreadable: let Read produce the error
    if size < MIN_BYTES:
        return 0

    try:
        digest, total_lines = _digest(fp, size)
    except Exception:
        return 0

    now = time.time_ns()
    sp = _state_path()
    st = _load(sp, str(d.get("session_id") or ""))
    entries = st["entries"]
    start, end = _requested_range(ti, total_lines)
    key = f"{fp}|{start}|{end}"

    # Find the cheapest covering read: same file, same content, and a range
    # that already contains everything this call would return.
    covering = None
    for k, e in entries.items():
        if e.get("path") != fp or e.get("digest") != digest:
            continue
        try:
            if int(e["start"]) <= start and int(e["end"]) >= end:
                if covering is None or e.get("ts", 0) > covering[1].get("ts", 0):
                    covering = (k, e)
        except Exception:
            continue

    if covering is not None:
        ck, ce = covering
        blocks = int(ce.get("blocks") or 0) + 1
        if blocks <= MAX_BLOCKS:
            ce["blocks"] = blocks
            _save(sp, st)
            sys.stderr.write(_MSG.format(
                path=os.path.relpath(fp, str(_repo_root())) if fp.startswith(
                    str(_repo_root())) else os.path.basename(fp),
                a=start, b=end, seq=ce.get("seq", "?"),
                ago=_ago(now - int(ce.get("ts") or now)),
                n=blocks, maxb=MAX_BLOCKS) + "\n")
            try:
                import _offload_log
                _offload_log.log_event(_repo_root(), "fire", "read_cache_block",
                                       f"{os.path.basename(fp)} {start}-{end}")
            except Exception:
                pass
            return 2
        # Relief valve: the model insisted past the limit. Treat that as
        # evidence the block is wrong here, release it, and re-arm from
        # scratch so one insistence does not disable the gate for the file.
        ce["blocks"] = 0
        entries.pop(ck, None)

    st["seq"] = int(st.get("seq") or 0) + 1
    entries[key] = {"path": fp, "digest": digest, "start": start, "end": end,
                    "ts": now, "seq": st["seq"], "blocks": 0}
    _save(sp, st)
    return 0


# --------------------------------------------------------------------------
# Selftest
# --------------------------------------------------------------------------

def _selftest() -> int:  # noqa: C901
    import contextlib
    import io
    import tempfile
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    tmp = Path(tempfile.mkdtemp())
    (tmp / ".claude/state").mkdir(parents=True)

    global _repo_root
    orig = _repo_root
    _repo_root = lambda: tmp                                    # noqa: E731

    def mkfile(name, nlines=400, tag="a"):
        p = tmp / name
        p.write_text("".join(f"{tag} line {i}\n" for i in range(nlines)))
        return p

    def run(ti, session="s1", transcript="/x/main.jsonl", mode=None):
        old = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps({
                "tool_name": "Read" if mode is None else "Edit",
                "tool_input": ti, "session_id": session,
                "transcript_path": transcript}))
            buf = io.StringIO()
            err = io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
                rc = _invalidate() if mode == "invalidate" else main()
            return rc, err.getvalue()
        finally:
            sys.stdin = old

    def reset():
        (tmp / STATE_REL).unlink(missing_ok=True)

    big = mkfile("big.c")
    check("big-fixture-over-min", big.stat().st_size > MIN_BYTES)

    # --- core: first read allows, identical re-read blocks
    reset()
    check("first-read-allows", run({"file_path": str(big)})[0] == 0)
    rc, err = run({"file_path": str(big)})
    check("identical-reread-blocks", rc == 2 and "READ-CACHED" in err)

    # --- content change releases it (this is the post-Edit path)
    big.write_text("changed\n" * 400)
    check("changed-file-allows", run({"file_path": str(big)})[0] == 0)

    # --- coverage: a whole-file read covers a slice inside it
    reset()
    run({"file_path": str(big)})
    check("covered-slice-blocks",
          run({"file_path": str(big), "offset": 10, "limit": 20})[0] == 2)

    # --- a slice read does NOT cover a wider or disjoint request
    reset()
    run({"file_path": str(big), "offset": 10, "limit": 20})
    check("wider-request-allows",
          run({"file_path": str(big), "offset": 5, "limit": 100})[0] == 0)
    reset()
    run({"file_path": str(big), "offset": 10, "limit": 20})
    check("disjoint-slice-allows",
          run({"file_path": str(big), "offset": 200, "limit": 20})[0] == 0)

    # --- the default limit is honoured: a whole-file read of a >2000-line
    #     file does not claim to cover line 3000
    reset()
    huge = mkfile("huge.c", nlines=3000)
    run({"file_path": str(huge)})
    check("beyond-default-limit-allows",
          run({"file_path": str(huge), "offset": 2500, "limit": 50})[0] == 0)
    check("within-default-limit-blocks",
          run({"file_path": str(huge), "offset": 100, "limit": 50})[0] == 2)

    # --- anti-wedge valve: MAX_BLOCKS then release
    reset()
    run({"file_path": str(big)})
    for i in range(MAX_BLOCKS):
        check(f"valve-blocks-{i}", run({"file_path": str(big)})[0] == 2)
    check("valve-releases", run({"file_path": str(big)})[0] == 0)
    check("valve-rearms", run({"file_path": str(big)})[0] == 2)

    # --- subagent reads are never gated
    reset()
    for _ in range(4):
        check("subagent-never-blocks",
              run({"file_path": str(big)},
                  transcript="/p/agent-abc.jsonl")[0] == 0)

    # --- a different session starts clean (rollover safety)
    reset()
    run({"file_path": str(big)})
    check("other-session-allows",
          run({"file_path": str(big)}, session="s2")[0] == 0)

    # --- small files, missing files, special suffixes, `pages` are exempt
    reset()
    small = tmp / "small.md"
    small.write_text("x" * 100)
    check("small-file-exempt", run({"file_path": str(small)})[0] == 0
          and run({"file_path": str(small)})[0] == 0)
    check("missing-file-exempt", run({"file_path": str(tmp / "gone.c")})[0] == 0)
    png = tmp / "shot.png"
    png.write_bytes(b"\x89PNG" + b"z" * MIN_BYTES)
    check("image-exempt", run({"file_path": str(png)})[0] == 0
          and run({"file_path": str(png)})[0] == 0)
    doc = tmp / "spec.pdf"
    doc.write_bytes(b"%PDF" + b"z" * MIN_BYTES)
    check("pdf-exempt", run({"file_path": str(doc)})[0] == 0
          and run({"file_path": str(doc)})[0] == 0)
    reset()
    run({"file_path": str(big)})
    check("pages-arg-exempt",
          run({"file_path": str(big), "pages": "1-3"})[0] == 0)

    # --- failed-Edit invalidation frees the re-read
    reset()
    run({"file_path": str(big)})
    check("blocked-before-invalidate", run({"file_path": str(big)})[0] == 2)
    run({"file_path": str(big)}, mode="invalidate")
    check("invalidate-frees-reread", run({"file_path": str(big)})[0] == 0)
    # invalidating an untouched path leaves other entries gated
    reset()
    other = mkfile("other.c", tag="b")
    run({"file_path": str(big)})
    run({"file_path": str(other)}, mode="invalidate")
    check("invalidate-is-path-scoped", run({"file_path": str(big)})[0] == 2)

    # --- kill switch + one-shot override
    reset()
    run({"file_path": str(big)})
    os.environ["READ_CACHE_DISABLE"] = "1"
    check("kill-switch-allows", run({"file_path": str(big)})[0] == 0)
    del os.environ["READ_CACHE_DISABLE"]
    check("kill-switch-removal-restores", run({"file_path": str(big)})[0] == 2)
    (tmp / OVERRIDE_REL).write_text("1")
    check("override-allows", run({"file_path": str(big)})[0] == 0)
    check("override-is-one-shot", run({"file_path": str(big)})[0] == 2)
    check("override-consumed", not (tmp / OVERRIDE_REL).exists())

    # --- malformed payloads fail open
    old = sys.stdin
    try:
        sys.stdin = io.StringIO("not json")
        with contextlib.redirect_stderr(io.StringIO()):
            check("malformed-fails-open", main() == 0)
        sys.stdin = io.StringIO(json.dumps({"tool_name": "Read"}))
        with contextlib.redirect_stderr(io.StringIO()):
            check("no-input-fails-open", main() == 0)
        sys.stdin = io.StringIO(json.dumps({"tool_name": "Grep"}))
        with contextlib.redirect_stderr(io.StringIO()):
            check("other-tool-ignored", main() == 0)
    finally:
        sys.stdin = old

    # --- state stays bounded
    reset()
    for i in range(MAX_ENTRIES + 40):
        f = mkfile(f"f{i}.c", nlines=200)
        run({"file_path": str(f)})
    st = json.loads((tmp / STATE_REL).read_text())
    check("state-bounded", len(st.get("entries") or {}) <= MAX_ENTRIES)

    _repo_root = orig
    if fails:
        sys.stderr.write("read_cache_block selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("read_cache_block selftest OK")
    return 0


if __name__ == "__main__":
    try:
        if "--selftest" in sys.argv:
            sys.exit(_selftest())
        sys.exit(_invalidate() if "invalidate" in sys.argv[1:] else main())
    except Exception:
        sys.exit(0)   # fail-open: a broken read gate must never wedge a run
