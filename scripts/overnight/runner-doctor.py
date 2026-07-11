#!/usr/bin/env python3
"""Deterministic pre-launch health check + transient-context pruning.

Runs BEFORE Claude spawns (wired into overnight-launch.sh): a run that would
die on a dead Codex login, a stale graph cache, or a broken hook should fail
in this script for free, not after Opus has loaded 200K of context.

Checks (JSON verdict on stdout; exit 1 on any HARD failure):
  codex_auth        auth.json present + parseable (Codex is the sole external
                    reviewer; a revoked login halts every section gate)
  hook_selftests    run_phase_guard selftest, edit_preflight --selftest
  recorder_health   last-codex-review.json / last-review-stamps.json parseable
  state_writable    .claude/state + .claude/overnight writable
  push_state        upstream configured; unpushed count reported (soft)
  graph_fresh       build/todo-cache.json newer than the newest todo/ md (soft
                    -- PREFLIGHT rebuilds it anyway; reported for visibility)
  toolchain         clang-19 / nasm / ld.lld-19 / node / gh on PATH
  watchdog          armed marker implies watchdog timer active (soft)

Pruning (always; keeps the situational brief action-relevant):
  - live-gotchas entries whose `(expires YYYY-MM-DD)` has passed
  - read-offload sliding-window state older than a day
  - agent-cache + artifacts hygiene prune (delegated to their own caps)
"""
from __future__ import annotations

import datetime
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd, cwd, timeout=60):
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True,
                          timeout=timeout)


def main(argv) -> int:
    root = Path(argv[0] if argv else ".").resolve()
    hard, soft = [], []
    out = {"checks": {}, "pruned": []}

    def check(name, ok, why="", hard_fail=True):
        out["checks"][name] = {"ok": bool(ok), "why": why}
        if not ok:
            (hard if hard_fail else soft).append(f"{name}: {why}")

    # codex auth
    auth = Path.home() / ".codex/auth.json"
    try:
        json.loads(auth.read_text())
        check("codex_auth", True)
    except Exception as exc:  # noqa: BLE001
        check("codex_auth", False, f"~/.codex/auth.json unreadable ({exc}); "
              "run `codex login` interactively")

    # hook selftests
    for name, cmd in (
        ("guard_selftest", [sys.executable, ".claude/hooks/run_phase_guard.py",
                            "selftest"]),
        ("edit_preflight_selftest", [sys.executable,
                                     ".claude/hooks/edit_preflight.py",
                                     "--selftest"]),
    ):
        try:
            r = run(cmd, root)
            check(name, r.returncode == 0,
                  (r.stderr or r.stdout).strip()[:200])
        except Exception as exc:  # noqa: BLE001
            check(name, False, str(exc)[:200])

    # recorder health (parseable state; absence is fine)
    rec_ok, rec_why = True, ""
    for f in ("last-codex-review.json", "last-review-stamps.json"):
        p = root / ".claude/state" / f
        if p.exists():
            try:
                json.loads(p.read_text())
            except ValueError:
                rec_ok, rec_why = False, f"{f} is corrupt JSON"
    check("recorder_health", rec_ok, rec_why)

    # writable state
    for d in (".claude/state", ".claude/overnight"):
        p = root / d
        try:
            p.mkdir(parents=True, exist_ok=True)
            probe = p / ".doctor-probe"
            probe.write_text("x")
            probe.unlink()
        except OSError as exc:
            check("state_writable", False, f"{d}: {exc}")
            break
    else:
        check("state_writable", True)

    # push state (soft -- the run pushes as it goes)
    try:
        r = run(["git", "rev-list", "--count", "@{u}..HEAD"], root, 30)
        if r.returncode != 0:
            check("push_state", False, "no upstream configured", hard_fail=False)
        else:
            n = r.stdout.strip()
            check("push_state", n == "0", f"{n} unpushed commit(s)",
                  hard_fail=False)
    except Exception as exc:  # noqa: BLE001
        check("push_state", False, str(exc)[:100], hard_fail=False)

    # graph cache freshness (soft; preflight rebuilds)
    cache = root / "build/todo-cache.json"
    try:
        newest = max(p.stat().st_mtime for p in (root / "todo").rglob("*.md"))
        check("graph_fresh", cache.exists() and cache.stat().st_mtime >= newest,
              "todo-cache.json older than newest todo edit (preflight rebuilds)",
              hard_fail=False)
    except Exception:
        check("graph_fresh", False, "could not compare", hard_fail=False)

    # toolchain
    missing = [t for t in ("clang-19", "nasm", "ld.lld-19", "node", "gh")
               if shutil.which(t) is None]
    check("toolchain", not missing, f"missing: {missing}")

    # watchdog (soft): armed implies the watchdog timer exists
    armed = (root / ".claude/state/sequencer-armed").exists()
    if armed:
        try:
            r = run(["systemctl", "--user", "list-timers", "--all"], root, 15)
            ok = "overnight-" in r.stdout and "watchdog" in r.stdout
            check("watchdog", ok, "armed but no watchdog timer visible",
                  hard_fail=False)
        except Exception:
            check("watchdog", False, "systemctl unavailable", hard_fail=False)

    # ---- pruning ----
    gotchas = root / ".claude/state/live-gotchas.md"
    if gotchas.exists():
        today = datetime.date.today().isoformat()
        kept, dropped = [], 0
        for ln in gotchas.read_text(encoding="utf-8").splitlines():
            m = re.search(r"\(expires (\d{4}-\d{2}-\d{2})\)", ln)
            if m and m.group(1) < today:
                dropped += 1
                continue
            kept.append(ln)
        if dropped:
            gotchas.write_text("\n".join(kept) + "\n", encoding="utf-8")
            out["pruned"].append(f"live-gotchas: {dropped} expired entrie(s)")
    ro = root / ".claude/state/read-offload.json"
    try:
        import time
        if ro.exists() and time.time() - ro.stat().st_mtime > 86400:
            ro.unlink()
            out["pruned"].append("read-offload.json (stale > 1 day)")
    except OSError:
        pass

    out["hard_failures"] = hard
    out["soft_notes"] = soft
    out["ok"] = not hard
    print(json.dumps(out, indent=1))
    return 0 if not hard else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
