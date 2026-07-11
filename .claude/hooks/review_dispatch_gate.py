#!/usr/bin/env python3
# block-via: exit 2 (stamp-writing todo edits only; fail-open on any error)
"""PreToolUse: per-review-pass agent-dispatch gate (compliance follow-up).

Measured 2026-07-03 (run-20260702-141810.log per-invocation matrix):
kernel-quality-auditor missing in 10/17 review passes, review-evidence-mapper
in 6/17, and TODO-12 section 25's ENTIRE review ran inline with zero
Skill(review-todo-section) invocation. The warning layer shipped the same
day; this hook is the promised hard gate.

Trigger: an Edit/Write/MultiEdit on todo/**/*.md that ADDS a
`**Verified:**` or `**Quality reviewed:**` stamp line (present in the new
text, absent from the old -- commit-hash fills into an EXISTING stamp line
do not trigger). Two requirements, both checked only at that moment:

  1. REVIEW-SKILL SHAPE: this session must have a live (non-orphaned)
     review-class skill invocation (review-todo-section /
     verify-todo-section / quality-review-section) whose args name this
     TODO file -- blocks the "review pipeline executed inline" shape.
  2. DISPATCH EVIDENCE: `.claude/state/last-agent-dispatch.json` `by_type`
     must show a `review-evidence-mapper` dispatch inside this pass's
     window; when the reviewed ship commit (the `adversarial_head` recorded
     by codex_review_completed in last-review-stamps.json) touched
     src/kernel|include/kernel -> also `kernel-quality-auditor`;
     src/boot -> `boot-quality-auditor`.

Pass window: the earliest review-kind timestamp recorded for this TODO in
last-review-stamps.json minus 2h grace (the mapper legitimately runs in
Phase 1, before any Codex dispatch); fallback when no stamps exist: 6h.

Fail-open: any parse/git/state error allows the edit -- a broken gate must
never block legitimate stamping. Opt-out (revert / stamp-repair /
known-false-positive): SKIP_DISPATCH_GATE=1 with
SKIP_DISPATCH_GATE_REASON (>= 12 chars), logged to
.claude/state/skip-log.jsonl best-effort.

Selftest: python3 review_dispatch_gate.py --selftest
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

STAMP_RE = re.compile(r"\*\*(?:Verified|Quality[ -]reviewed):\*\*")
GRACE_NS = 2 * 3600 * 1_000_000_000
FALLBACK_WINDOW_NS = 6 * 3600 * 1_000_000_000
REVIEW_SKILLS = ("review-todo-section", "verify-todo-section",
                 "quality-review-section")
KERNEL_PREFIXES = ("src/kernel/", "include/kernel/")
BOOT_PREFIX = "src/boot/"


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _adds_stamp(tool_name: str, ti: dict, root: Path) -> bool:
    """True iff this edit introduces a stamp line that was not there before."""
    if tool_name == "Edit":
        old = ti.get("old_string") or ""
        new = ti.get("new_string") or ""
        return bool(STAMP_RE.search(new)) and not STAMP_RE.search(old)
    if tool_name == "MultiEdit":
        for e in ti.get("edits") or []:
            if STAMP_RE.search(e.get("new_string") or "") and \
               not STAMP_RE.search(e.get("old_string") or ""):
                return True
        return False
    if tool_name == "Write":
        content = ti.get("content") or ""
        if not STAMP_RE.search(content):
            return False
        try:
            on_disk = (root / ti.get("file_path", "")).read_text(encoding="utf-8") \
                if not os.path.isabs(ti.get("file_path", "")) \
                else Path(ti["file_path"]).read_text(encoding="utf-8")
        except Exception:
            return True  # new file written with stamps -- suspicious, gate it
        return not STAMP_RE.search(on_disk)
    return False


def _session_has_review_skill(root: Path, session_id: str, todo_base: str) -> bool:
    try:
        prog = json.loads((root / ".claude/state/skill-progress.json").read_text())
    except Exception:
        return True  # fail-open
    for key, entry in prog.items():
        base = key.split(".orphan.")[0]
        if base not in REVIEW_SKILLS or ".orphan." in key:
            continue
        if not isinstance(entry, dict) or entry.get("compaction_orphaned"):
            continue
        if session_id and entry.get("session_id") not in ("", None, session_id):
            continue
        if todo_base and todo_base not in (entry.get("args") or ""):
            continue
        return True
    return False


def _pass_window_start(root: Path, todo_rel: str) -> int:
    now = time.time_ns()
    try:
        stamps = json.loads((root / ".claude/state/last-review-stamps.json").read_text())
        rec = stamps.get(todo_rel) or {}
        kind_ts = [v for k, v in rec.items()
                   if isinstance(v, int) and not k.endswith(("_head", "_section"))]
        if kind_ts:
            return min(kind_ts) - GRACE_NS
    except Exception:
        pass
    return now - FALLBACK_WINDOW_NS


def _dispatched_within(root: Path, agent_type: str, window_start: int) -> bool:
    try:
        st = json.loads((root / ".claude/state/last-agent-dispatch.json").read_text())
        ent = (st.get("by_type") or {}).get(agent_type) or {}
        ts = ent.get("timestamp_ns")
        return isinstance(ts, int) and ts >= window_start
    except Exception:
        return True  # fail-open


def _fresh_diff_facts(root: Path, window_start: int) -> bool:
    """A fresh diff-facts run IS the deterministic equivalent of the
    review-evidence-mapper's evidence gathering (changed defs/callers/tests +
    concurrency/ABI/user-ptr inventory + cppcheck), so it satisfies the MAPPER
    requirement only -- the kernel/boot QUALITY auditors are judgment nets and
    stay mandatory. FAIL-CLOSED: a missing/malformed/out-of-window/stale receipt
    returns False and the mapper dispatch stays required."""
    try:
        rc = json.loads((root / ".claude/state/last-diff-facts.json").read_text())
    except Exception:
        return False
    ts = rc.get("ts_ns")
    files = rc.get("changed_files")
    digest = rc.get("digest")
    if not (isinstance(ts, int) and ts >= window_start
            and isinstance(files, list) and isinstance(digest, str)):
        return False
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent
                              / "scripts/overnight"))
        from worktree_hash import worktree_key
        return worktree_key(root, files) == digest
    except Exception:
        return False


def _reviewed_ship_files(root: Path, todo_rel: str) -> list[str]:
    """Files touched by the reviewed ship commit (adversarial_head), or []."""
    try:
        stamps = json.loads((root / ".claude/state/last-review-stamps.json").read_text())
        head = (stamps.get(todo_rel) or {}).get("adversarial_head") or ""
        if not head:
            return []
        out = subprocess.check_output(
            ["git", "show", "--name-only", "--format=", head],
            cwd=str(root), text=True, timeout=5, stderr=subprocess.DEVNULL)
        return [ln.strip() for ln in out.splitlines() if ln.strip()]
    except Exception:
        return []


def _log_skip(root: Path, reason: str, target: str) -> None:
    try:
        p = root / ".claude/state/skip-log.jsonl"
        with p.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps({
                "hook": "review_dispatch_gate", "ts_ns": time.time_ns(),
                "reason": reason, "target": target}) + "\n")
    except Exception:
        pass


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    tool = d.get("tool_name") or ""
    if tool not in ("Edit", "Write", "MultiEdit"):
        return 0
    ti = d.get("tool_input") or {}
    fp = (ti.get("file_path") or "").replace("\\", "/")
    if "/todo/" not in fp and not fp.startswith("todo/"):
        return 0
    if not fp.endswith(".md"):
        return 0
    root = _repo_root()
    # scope to THIS repo (cross-repo lesson, kit LESSONS 31)
    if fp.startswith("/") and not fp.startswith(str(root) + "/"):
        return 0
    try:
        if not _adds_stamp(tool, ti, root):
            return 0
    except Exception:
        return 0  # fail-open

    sys.path.insert(0, str(Path(__file__).parent))
    try:
        import _skip_env
        envs = _skip_env.read_skip_envs(
            "", keys=("SKIP_DISPATCH_GATE", "SKIP_DISPATCH_GATE_REASON"))
    except Exception:
        envs = {}
    if envs.get("SKIP_DISPATCH_GATE") == "1":
        reason = envs.get("SKIP_DISPATCH_GATE_REASON", "")
        if len(reason) >= 12:
            _log_skip(root, reason, fp)
            return 0
        sys.stderr.write(
            "[review-dispatch-gate] SKIP_DISPATCH_GATE=1 requires "
            "SKIP_DISPATCH_GATE_REASON of >= 12 chars.\n")
        return 2

    todo_rel = fp[len(str(root)) + 1:] if fp.startswith(str(root) + "/") else fp
    todo_base = os.path.basename(todo_rel)
    session_id = d.get("session_id") or ""
    failures = []

    if not _session_has_review_skill(root, session_id, todo_base):
        failures.append(
            "no review-class Skill invocation for this TODO in this session "
            "(the 'review pipeline executed inline' shape, TODO-12 section-25 "
            "class): invoke Skill(review-todo-section) and run the pipeline "
            "through it")

    window_start = _pass_window_start(root, todo_rel)
    ship_files = _reviewed_ship_files(root, todo_rel)
    # Cost-neutral routing (2026-07-11): the mapper must REPLACE reading, not
    # supplement it. A tiny non-kernel/non-boot diff is cheaper for Opus to
    # read directly than to pay a Sonnet mapper -- so require the mapper ONLY
    # for large diffs or any kernel/boot/security surface (the expensive bug
    # class warrants the extra breadth net). Source .c/.h/.asm files only;
    # docs/TODO churn does not count toward the size.
    src_ship = [f for f in ship_files
                if f.endswith((".c", ".h", ".asm", ".S"))]
    kernelish = any(f.startswith(KERNEL_PREFIXES) or f.startswith(BOOT_PREFIX)
                    for f in ship_files)
    mapper_required = kernelish or len(src_ship) > 5
    if mapper_required and \
       not _dispatched_within(root, "review-evidence-mapper", window_start) and \
       not _fresh_diff_facts(root, window_start):
        failures.append(
            "no review-evidence-mapper dispatch (or fresh diff-facts receipt) "
            "in this pass window (required for large or kernel/boot diffs; a "
            "tiny non-kernel diff is exempt -- Opus reads it directly). Run "
            "scripts/overnight/diff-facts.py to satisfy this deterministically")

    if any(f.startswith(KERNEL_PREFIXES) for f in ship_files) and \
       not _dispatched_within(root, "kernel-quality-auditor", window_start):
        failures.append(
            "reviewed ship commit touches src/kernel|include/kernel but no "
            "kernel-quality-auditor dispatch in this pass window "
            "(review-todo-section step 7; measured missing in 10/17 passes)")
    if any(f.startswith(BOOT_PREFIX) for f in ship_files) and \
       not _dispatched_within(root, "boot-quality-auditor", window_start):
        failures.append(
            "reviewed ship commit touches src/boot but no boot-quality-auditor "
            "dispatch in this pass window (review-todo-section step 7)")

    if not failures:
        return 0
    sys.stderr.write(
        "[review-dispatch-gate] BLOCK -- stamp edit on " + todo_rel + ":\n  - "
        + "\n  - ".join(failures) +
        "\nDispatch the missing agent(s) / invoke the skill, then retry the "
        "stamp edit. Legitimate revert / stamp-repair / false-positive: "
        "SKIP_DISPATCH_GATE=1 SKIP_DISPATCH_GATE_REASON='<why, >=12 chars>' "
        "(logged). Doctrine: CLAUDE.md 'Specialist agents' + "
        "todo/overnight-runner-improvements.md compliance item.\n")
    return 2


def _selftest() -> int:
    import contextlib
    import io
    import tempfile
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    tmp = Path(tempfile.mkdtemp())
    (tmp / ".claude/state").mkdir(parents=True)
    (tmp / ".claude/hooks").mkdir(parents=True)
    (tmp / "todo/02-kernel-core").mkdir(parents=True)
    todo_rel = "todo/02-kernel-core/TODO-99-fixture.md"
    (tmp / todo_rel).write_text("## 1. X\n- [x] item\n")

    # A real git commit touching >5 src files so _reviewed_ship_files() sees a
    # LARGE diff -- the mapper-required routing (2026-07-11) exempts tiny
    # non-kernel diffs, so the mapper cases need a large-diff head to exercise.
    import subprocess as _sp
    _sp.run(["git", "init", "-q", str(tmp)], check=True)
    (tmp / "src").mkdir(parents=True, exist_ok=True)
    big_files = []
    for i in range(6):
        f = f"src/mod{i}.c"
        (tmp / f).write_text(f"int m{i}(void){{return {i};}}\n")
        big_files.append(f)
    _sp.run(["git", "-C", str(tmp), "add", "-A"], check=True, capture_output=True)
    _sp.run(["git", "-C", str(tmp), "-c", "user.email=t@t", "-c",
             "user.name=t", "commit", "-qm", "big"], check=True,
            capture_output=True)
    big_head = _sp.check_output(["git", "-C", str(tmp), "rev-parse", "HEAD"],
                                text=True).strip()

    global _repo_root
    orig_root = _repo_root
    _repo_root = lambda: tmp

    now = time.time_ns()

    def run(payload):
        old = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps(payload))
            buf = io.StringIO()
            with contextlib.redirect_stderr(buf):
                rc = main()
            return rc, buf.getvalue()
        finally:
            sys.stdin = old

    def stamp_edit(**extra):
        p = {"tool_name": "Edit", "session_id": "sess-1",
             "tool_input": {"file_path": str(tmp / todo_rel),
                            "old_string": "- [x] item",
                            "new_string": "- [x] item\n> **Verified:** 2026-07-03"}}
        p.update(extra)
        return p

    def write_state(skill_ok=True, mapper_ok=True, auditor_ok=False,
                    ship_kernel=False):
        prog = {}
        if skill_ok:
            prog["review-todo-section"] = {
                "session_id": "sess-1", "args": f"{todo_rel} section 1"}
        (tmp / ".claude/state/skill-progress.json").write_text(json.dumps(prog))
        by_type = {}
        if mapper_ok:
            by_type["review-evidence-mapper"] = {"timestamp_ns": now}
        if auditor_ok:
            by_type["kernel-quality-auditor"] = {"timestamp_ns": now}
        (tmp / ".claude/state/last-agent-dispatch.json").write_text(
            json.dumps({"timestamp_ns": now, "by_type": by_type}))
        # adversarial_head points at the 6-src-file commit -> LARGE diff, so
        # the mapper is genuinely required (the tiny-diff exemption does not
        # apply); this exercises the mapper cases as before the 2026-07-11
        # routing change.
        stamps = {todo_rel: {"adversarial": now, "adversarial_head": big_head}}
        (tmp / ".claude/state/last-review-stamps.json").write_text(
            json.dumps(stamps))

    # 1. non-stamp edit passes untouched.
    rc, _ = run({"tool_name": "Edit", "session_id": "s",
                 "tool_input": {"file_path": str(tmp / todo_rel),
                                "old_string": "a", "new_string": "b"}})
    check("non-stamp-edit-allowed", rc == 0)

    # 2. hash-fill into an existing stamp line does not trigger.
    rc, _ = run({"tool_name": "Edit", "session_id": "s",
                 "tool_input": {"file_path": str(tmp / todo_rel),
                                "old_string": "> **Verified:** <hash>",
                                "new_string": "> **Verified:** abc123"}})
    check("hash-fill-allowed", rc == 0)

    # 3. stamp edit with skill + mapper evidence -> allowed.
    write_state(skill_ok=True, mapper_ok=True)
    rc, _ = run(stamp_edit())
    check("full-evidence-allowed", rc == 0)

    # 4. inline-review shape (no review skill this session) -> BLOCK.
    write_state(skill_ok=False, mapper_ok=True)
    rc, err = run(stamp_edit())
    check("inline-shape-blocked", rc == 2 and "inline" in err)

    # 5. missing mapper dispatch on a LARGE diff -> BLOCK.
    write_state(skill_ok=True, mapper_ok=False)
    rc, err = run(stamp_edit())
    check("missing-mapper-blocked", rc == 2 and "review-evidence-mapper" in err)

    # 5b. tiny non-kernel diff (2 src files) with no mapper -> ALLOW
    #     (2026-07-11 cost-neutral routing: Opus reads a tiny diff directly).
    small_head = _sp.check_output(
        ["git", "-C", str(tmp), "rev-parse", "HEAD"], text=True).strip()
    # rewrite HEAD to a 2-file commit
    for i in range(2, 6):
        (tmp / f"src/mod{i}.c").unlink()
    _sp.run(["git", "-C", str(tmp), "add", "-A"], check=True, capture_output=True)
    _sp.run(["git", "-C", str(tmp), "-c", "user.email=t@t", "-c",
             "user.name=t", "commit", "-qm", "small"], check=True,
            capture_output=True)
    small_head = _sp.check_output(
        ["git", "-C", str(tmp), "rev-parse", "HEAD"], text=True).strip()
    (tmp / ".claude/state/last-review-stamps.json").write_text(json.dumps(
        {todo_rel: {"adversarial": now, "adversarial_head": small_head}}))
    prog = {"review-todo-section": {"session_id": "sess-1",
                                    "args": f"{todo_rel} section 1"}}
    (tmp / ".claude/state/skill-progress.json").write_text(json.dumps(prog))
    (tmp / ".claude/state/last-agent-dispatch.json").write_text(
        json.dumps({"timestamp_ns": now, "by_type": {}}))
    rc, _ = run(stamp_edit())
    check("tiny-diff-mapper-exempt", rc == 0)
    # restore the large head for later cases
    (tmp / ".claude/state/last-review-stamps.json").write_text(json.dumps(
        {todo_rel: {"adversarial": now, "adversarial_head": big_head}}))

    # 6. skip env with reason allows + logs.
    write_state(skill_ok=False, mapper_ok=False)
    os.environ["SKIP_DISPATCH_GATE"] = "1"
    os.environ["SKIP_DISPATCH_GATE_REASON"] = "selftest legitimate stamp repair"
    rc, _ = run(stamp_edit())
    check("skip-with-reason-allowed", rc == 0)
    check("skip-logged",
          (tmp / ".claude/state/skip-log.jsonl").exists())
    # 7. skip env without reason blocks.
    os.environ["SKIP_DISPATCH_GATE_REASON"] = "short"
    rc, _ = run(stamp_edit())
    check("skip-without-reason-blocked", rc == 2)
    os.environ.pop("SKIP_DISPATCH_GATE", None)
    os.environ.pop("SKIP_DISPATCH_GATE_REASON", None)

    # 8. missing state files entirely -> fail-open allow.
    for f in ("skill-progress.json", "last-agent-dispatch.json",
              "last-review-stamps.json"):
        (tmp / ".claude/state" / f).unlink(missing_ok=True)
    rc, _ = run(stamp_edit())
    # skill-progress missing -> fail-open True; dispatch missing -> fail-open
    check("missing-state-fail-open", rc == 0)

    # 9. foreign-repo absolute path silent.
    rc, _ = run({"tool_name": "Edit", "session_id": "s",
                 "tool_input": {"file_path": "/other/repo/todo/TODO-1.md",
                                "old_string": "x",
                                "new_string": "> **Verified:** y"}})
    check("foreign-repo-silent", rc == 0)

    _repo_root = orig_root
    if fails:
        sys.stderr.write("review_dispatch_gate selftest FAIL: "
                         + "; ".join(fails) + "\n")
        return 1
    print("review_dispatch_gate selftest OK")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    sys.exit(main())
