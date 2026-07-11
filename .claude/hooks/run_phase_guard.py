#!/usr/bin/env python3
"""
run_phase_guard.py -- the overnight sequencer's no-deviation phase machine.

Enforces the per-file pipeline from todo/TODO-Claude-Overnight-Runner.md so an
unattended `bypassPermissions` run physically cannot skip a stage, reorder the
sequence, ask a human, or stop before fixpoint.

Roles (dispatched on argv[1]):
  pretool   PreToolUse hook: reads {tool_name, tool_input} on stdin.
            exit 0 = allow, exit 2 + stderr = BLOCK.
  stop      Stop hook: for the headless unattended run (env discriminator
            OVERNIGHT_SEQUENCER_RUN=1) while ARMED and not at FIXPOINT, exit 2
            keeps it going so it physically cannot voluntarily stop -- ONLY the
            human's `--disarm` (which removes the armed marker) ends it. The
            watchdog handles real death. Interactive sessions (env unset) are
            never blocked, so the operator is never trapped in the repo.
  CLI       start / status / phase / cursor / progress / next-pass / fixpoint /
            clear / selftest / relifecycle / wait / wait-ready / wake /
            rollover -- the overnight-sequencer skill drives phase
            transitions (and structural waits + verified context rotations)
            through these.

State (run cursor only; deferrals live in the TODO files, not here):
  .claude/state/sequencer-run.json
    {active, phase, pass_no, domain, file, section_idx, progress_this_pass,
     started_at, updated_at}

The phase-guard governs the OUTER sequence (you cannot implement/review a
section until the file passed VALIDATE + GAP_AUDIT; you cannot re-validate
mid-section; AskUserQuestion is never allowed). The INNER per-section pipeline
(Codex dispatches, build/test, commit gates) is governed by the existing
within-section hooks -- this guard does not micro-manage it.
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HOOK_DIR = Path(__file__).resolve().parent


def repo_root():
    d = HOOK_DIR
    while d != d.parent:
        if (d / ".git").is_dir():
            return d
        d = d.parent
    return Path.cwd()


STATE_PATH = repo_root() / ".claude/state/sequencer-run.json"
# Written by arm-sequencer.sh; present means "an unattended sequencer run is
# armed". This is the PERSISTENT MASTER SWITCH: it stays on disk for the entire
# run (it is NOT removed when the run goes active) and is cleared ONLY by the
# human's `--disarm` or by an oracle-verified FIXPOINT. While armed, every fresh
# watchdog-spawned agent is redirected onto overnight-sequencer and cannot
# voluntarily stop. This is what makes the death-thrash impossible: even if the
# run cursor (sequencer-run.json) is reset to inactive out from under the guard,
# the marker keeps the headless run alive and guarded until the operator disarms.
ARMED_MARKER = repo_root() / ".claude/state/sequencer-armed"
# Written ONLY by `run_phase_guard.py fixpoint` AFTER the triage oracle confirms
# zero remaining work. The headless run's Stop is blocked unless this exists, so
# the ONLY graceful self-stop is a machine-verified completion. The agent cannot
# fabricate it: creating/editing it (or the cursor, or the marker) is a blocked
# self-teardown, and `phase FIXPOINT` / `clear` are blocked too. The run cannot
# talk itself into stopping on a reserved decision or a per-file blocker.
FIXPOINT_SENTINEL = repo_root() / ".claude/state/sequencer-fixpoint"


def is_headless():
    """True only inside the unattended systemd-launched run.

    arm-sequencer.sh writes Environment=OVERNIGHT_SEQUENCER_RUN=1 into the
    systemd unit drop-in, so the headless `claude -p` process and all its hook
    subprocesses see it; an interactive operator session in the same repo does
    NOT. This is the load-bearing discriminator: the guard governs ONLY the
    headless run, so it can be absolute there (no stopping, no self-disarm)
    while never trapping the human who shares the repo's settings.json hooks.
    """
    return os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1"


# Bash command shapes that would let the headless run tear itself down or fake a
# completion. Only the human (interactive session, env discriminator absent) may
# run these. NOTE: `run_phase_guard.py fixpoint` is intentionally NOT blocked --
# it is the legitimate completion path and is itself oracle-gated (it REFUSES
# unless the triage oracle reports zero remaining work), so it cannot be abused.
_STATE_FILES = ("sequencer-armed", "sequencer-run.json", "sequencer-fixpoint")
# touch/cp/dd close the fabricate-the-fixpoint-sentinel bypass (runner-kit
# 2026-07-03: touch/cp/dd wrote the sentinel without tripping the old list).
_MUTATE_OPS = ("rm ", "rm-", "mv ", "unlink", "truncate", "tee ", " > ", ">>",
               "touch ", "cp ", "dd ", "os.remove", "os.unlink", "rmtree",
               "shutil.")


def _is_self_teardown(cmd):
    c = " ".join((cmd or "").split())
    # Disarm via either wrapper.
    if ("arm-sequencer.sh" in c or "overnight-arm.sh" in c) and "--disarm" in c:
        return True
    # Guard CLI used to fake completion or wipe the cursor (fixpoint excluded).
    if "run_phase_guard.py" in c and (" clear" in c or "phase FIXPOINT" in c):
        return True
    # Direct tampering with any guard state file.
    if any(f in c for f in _STATE_FILES) and any(op in c for op in _MUTATE_OPS):
        return True
    # Stopping/disabling the systemd units or killing the headless claude.
    if "systemctl" in c and any(op in c for op in ("stop", "disable", "kill")) and "overnight" in c:
        return True
    if any(k in c for k in ("pkill", "killall", "kill ")) and "claude" in c:
        return True
    return False

# Ordered phases of the per-file pipeline.
PHASES = ["PREFLIGHT", "TRIAGE", "VALIDATE", "GAP_AUDIT", "SECTIONS",
          "FILE_CLOSE", "ADVANCE", "FIXPOINT"]

# Skills the sequencer's OUTER pipeline controls. A controlled skill may only
# run in a phase whose allow-set lists it; any other controlled skill in the
# wrong phase is a deviation -> BLOCK. Skills NOT in this set (the inner-pipeline
# helpers implement/review invoke) pass freely -- the within-section gates own
# them.
SEQUENCE_SKILLS = {
    "validate-todo-file", "gap-audit-todo", "codex-gap-audit",
    "implement-todo-section", "implement-todo-item", "implement-ssdt-range",
    "review-todo-section", "verify-todo-section", "complete-todo-file",
    "overnight-todo-runner",  # legacy System A: never inside a sequencer run
}

PHASE_ALLOWED_SKILLS = {
    "PREFLIGHT": set(),
    "TRIAGE": set(),
    "VALIDATE": {"validate-todo-file"},
    "GAP_AUDIT": {"gap-audit-todo", "codex-gap-audit"},
    # SECTIONS allows the per-section drivers; their internal pipeline skills
    # are not in SEQUENCE_SKILLS so they pass freely.
    "SECTIONS": {"implement-todo-section", "implement-todo-item",
                 "implement-ssdt-range", "review-todo-section",
                 "verify-todo-section"},
    "FILE_CLOSE": {"complete-todo-file", "review-todo-section",
                   "verify-todo-section"},
    "ADVANCE": set(),
    "FIXPOINT": set(),
}

# Stage 1-2 skills, blocked on a cursor file that already carries BOTH
# file-preamble stamps (`> **Validated:**` + `> **Gap-audited:**`). The oracle
# reports this as `stages_1_2_done: true`; doctrine Stage 0 says SKIP straight
# to SECTIONS. Re-running them (each with a Codex pass) on a mature file was
# the single largest source of unnecessary phases. Escape hatch for the
# genuinely-new-section case: `run_phase_guard.py relifecycle <reason>` sets a
# one-shot override consumed by the next Stage 1-2 skill invocation.
LIFECYCLE_SKILLS = {"validate-todo-file", "gap-audit-todo", "codex-gap-audit"}


def load_state():
    if not STATE_PATH.exists():
        return {"active": False}
    try:
        with STATE_PATH.open(encoding="utf-8") as fh:
            return json.load(fh)
    except (json.JSONDecodeError, OSError):
        return {"active": False}


def save_state(state):
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    with STATE_PATH.open("w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=1)


def _skill_name(tool_input):
    return tool_input.get("skill") or tool_input.get("name") or ""


def evaluate(tool_name, tool_input, state, armed=False, headless=False,
             stages_done=False):
    """Return (allow: bool, message: str). Pure -- unit-testable.

    The guard governs ONLY the headless unattended run. An interactive operator
    session (headless=False) is never constrained -- it can ask, stop, and run
    --disarm freely. This is what lets the human share the repo's hooks without
    being trapped by the run cursor on disk.

    `stages_done` is the cursor file's `stages_1_2_done` verdict (computed by
    the caller only when the tool is a Stage 1-2 skill; False otherwise).
    """
    if not headless:
        return True, ""

    active = bool(state.get("active"))
    if not active and not armed:
        return True, ""

    # AskUserQuestion is never allowed inside the unattended run.
    if tool_name == "AskUserQuestion":
        return False, (
            "[SEQ-ASK] blocked: decide conservatively + log the assumption, or "
            "defer ([/] + Deferred awaiting-answer + XREF) and advance. "
            "Details: docs/infrastructure/hook-codes.md#seq-ask")

    # The unattended run must never tear itself down. Disarm/clear/stop is a
    # human-only operation, performed from an interactive session (where the
    # OVERNIGHT_SEQUENCER_RUN discriminator is absent and this guard is inert).
    if tool_name == "Bash" and _is_self_teardown(tool_input.get("command", "")):
        return False, (
            "[SEQ-TEARDOWN] blocked: disarm/clear/stop is human-only. Keep "
            "going -- continue the pipeline. "
            "Details: docs/infrastructure/hook-codes.md#seq-teardown")

    # Armed but not yet started: force the redirect onto overnight-sequencer.
    if not active and armed:
        if tool_name == "Skill":
            sk = _skill_name(tool_input)
            if sk == "overnight-sequencer":
                return True, ""
            return False, (
                "[SEQ-REDIRECT] armed run: invoke Skill(overnight-sequencer) "
                "now (no other skill first). "
                "Details: docs/infrastructure/hook-codes.md#seq-redirect")
        return True, ""  # Bash / Read / Grep / Glob allowed for setup

    # Active run: phase enforcement. (AskUserQuestion already handled above.)
    phase = state.get("phase", "PREFLIGHT")

    # Lifecycle routing (Stage 0): a mature file (both preamble stamps) never
    # re-runs Stage 1-2 -- the oracle already said so via stages_1_2_done, and
    # honoring it removes whole VALIDATE/GAP_AUDIT phases (each with a Codex
    # pass). One-shot escape: `relifecycle <reason>` for a genuinely new
    # `## N.` section (doctrine Stage 0 exception).
    if tool_name == "Skill":
        sk = _skill_name(tool_input)
        if (sk in LIFECYCLE_SKILLS and stages_done
                and not state.get("lifecycle_override")):
            return False, (
                f"[SEQ-LIFECYCLE] Skill({sk}) blocked: cursor file is mature "
                "(both preamble stamps). Run `run_phase_guard.py phase "
                "SECTIONS` and proceed; for a genuinely NEW section, "
                "`relifecycle \"<which>\"` first. "
                "Details: docs/infrastructure/hook-codes.md#seq-lifecycle")

    # Sequence-skill ordering: a controlled skill may only fire in a phase
    # that allows it.
    if tool_name == "Skill":
        sk = _skill_name(tool_input)
        if sk in SEQUENCE_SKILLS and sk not in PHASE_ALLOWED_SKILLS.get(phase, set()):
            allowed = sorted(PHASE_ALLOWED_SKILLS.get(phase, set())) or ["(none)"]
            return False, (
                f"[SEQ-PHASE] Skill({sk}) out of sequence in {phase} "
                f"(allowed: {', '.join(allowed)}). Set the correct phase via "
                "`run_phase_guard.py phase <PHASE>` and re-invoke. "
                "Details: docs/infrastructure/hook-codes.md#seq-phase")

    # Everything else (Bash, Edit, Write, Read, Grep, Glob, inner-pipeline
    # skills) passes -- the within-section gates govern it.
    return True, ""


def _cursor_stages_done(state):
    """Live stages_1_2_done verdict for the cursor file, via the triage
    oracle's file_lifecycle. Fail-open (False) on any error -- a broken
    oracle must not block Stage 1-2 work."""
    rel = state.get("file")
    if not rel:
        return False
    try:
        sys.path.insert(0, str(HOOK_DIR))
        import sequencer_triage
        lc = sequencer_triage.file_lifecycle(str(repo_root() / rel))
        return bool(lc.get("validated") and lc.get("gap_audited"))
    except Exception:
        return False


def handle_pretool():
    try:
        d = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    tool_name = d.get("tool_name", "")
    tool_input = d.get("tool_input", {})
    state = load_state()
    stages_done = False
    if (is_headless() and state.get("active") and tool_name == "Skill"
            and _skill_name(tool_input) in LIFECYCLE_SKILLS):
        stages_done = _cursor_stages_done(state)
    allow, msg = evaluate(tool_name, tool_input, state,
                          armed=ARMED_MARKER.exists(),
                          headless=is_headless(), stages_done=stages_done)
    if allow:
        # Consume the one-shot relifecycle override when a Stage 1-2 skill
        # actually fires against a mature file under it.
        if (stages_done and state.get("lifecycle_override")
                and tool_name == "Skill"
                and _skill_name(tool_input) in LIFECYCLE_SKILLS):
            state.pop("lifecycle_override", None)
            save_state(state)
        return 0
    sys.stderr.write(msg)
    return 2


# ---- structural waiting + verified rollover (2026-07-11) -------------------
#
# WAITING_REVIEW: a background Codex review used to cost one Opus turn per
# "Holding..." poll because the Stop hook rejected every voluntary stop. Now
# the session DECLARES the wait (`wait` verb: artifact paths + completion
# patterns), the Stop hook lets it end, and a non-model watcher
# (scripts/overnight/session-watcher.sh) wakes the run exactly once when the
# artifacts complete (watchdog tick as fallback). Zero model turns are spent
# waiting; no review is skipped -- the woken session must still receive it.
#
# ROLLOVER: after a section is shipped+reviewed+pushed+stamped, a fresh
# worker context is cheaper than a long-tail one. `rollover` machine-verifies
# the checkpoint (clean tree, nothing unpushed, graph rebuild OK, content-
# bound receipts, no outstanding jobs) and only then permits ONE clean stop;
# the watcher relaunches immediately. The RUN stays active the whole time --
# only the worker context rotates (doctrine: run-active vs context-rotates).

WAIT_TIMEOUT_MIN_S = 60
WAIT_TIMEOUT_MAX_S = 7200
WAIT_TIMEOUT_DEFAULT_S = 3600
ROLLOVER_PENDING_FRESH_S = 1800  # stale pending flags stop permitting stops
_WAIT_TAIL_BYTES = 1 << 20  # completion markers live at the artifact tail


def _artifact_matches(path: str, pattern: str) -> bool:
    import re as _re
    try:
        with open(path, "rb") as fh:
            fh.seek(0, 2)
            size = fh.tell()
            fh.seek(max(0, size - _WAIT_TAIL_BYTES))
            text = fh.read().decode("utf-8", "replace")
    except OSError:
        return False
    try:
        return _re.search(pattern, text) is not None
    except _re.error:
        return pattern in text


def _wait_satisfied(waiting: dict) -> bool:
    arts = waiting.get("artifacts") or []
    if not arts:
        return True
    return all(_artifact_matches(a.get("path", ""), a.get("pattern", ""))
               for a in arts)


def _wait_expired(waiting: dict) -> bool:
    since = waiting.get("since_epoch") or 0
    timeout = waiting.get("timeout_s") or WAIT_TIMEOUT_DEFAULT_S
    return time.time() - since > timeout


def _spawn_watcher(mode: str) -> int:
    """Spawn scripts/overnight/session-watcher.sh OUTSIDE the calling
    session's systemd cgroup. setsid alone is NOT enough: the headless run
    lives in a systemd service, and when its main process exits (the very
    stop the watcher exists to follow), systemd kills every process left in
    the control group -- watcher included (live incident 2026-07-11, first
    structural wait: watcher + awaited Codex review both died with the
    session). `systemd-run --user` puts the watcher in its own transient
    scope that survives; plain Popen remains the non-systemd fallback.
    Returns the pid (or unit-tagged 1 for systemd-run), 0 on failure."""
    script = repo_root() / "scripts/overnight/session-watcher.sh"
    if not script.exists():
        return 0
    log = repo_root() / ".claude/overnight/watcher.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    unit = f"seq-watcher-{mode}-{os.getpid()}-{int(time.time())}"
    try:
        env_pass = [f"--setenv={k}={v}" for k, v in os.environ.items()
                    if k.startswith("OVERNIGHT_") or k in ("PATH", "HOME")]
        r = subprocess.run(
            ["systemd-run", "--user", "--collect", f"--unit={unit}",
             *env_pass, "/bin/bash", str(script), str(repo_root()), mode],
            capture_output=True, timeout=15)
        if r.returncode == 0:
            with log.open("a") as fh:
                fh.write(f"spawned watcher via systemd-run unit {unit}\n")
            return 1  # alive-in-own-scope; pid tracked by systemd
    except Exception:
        pass
    try:
        with log.open("a") as fh:
            p = subprocess.Popen(
                ["bash", str(script), str(repo_root()), mode],
                stdout=fh, stderr=fh, stdin=subprocess.DEVNULL,
                start_new_session=True, cwd=str(repo_root()))
        return p.pid
    except Exception:
        return 0


def _rollover_failures(root: Path, state: dict) -> list:
    """Machine gates for a verified rollover checkpoint. Every failure is a
    reason the worker context may NOT rotate yet."""
    fails = []
    try:
        out = subprocess.run(["git", "status", "--porcelain", "-uno"],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0 or out.stdout.strip():
            fails.append("tree not clean (tracked changes present)")
    except Exception:
        fails.append("git status unavailable")
    try:
        out = subprocess.run(["git", "rev-list", "--count", "@{u}..HEAD"],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            fails.append("no upstream configured (cannot verify pushed)")
        elif out.stdout.strip() != "0":
            fails.append(f"{out.stdout.strip()} commit(s) not pushed")
    except Exception:
        fails.append("git rev-list unavailable")
    try:
        out = subprocess.run(
            ["bash", "scripts/todo-graph/build-and-validate.sh", "--keep-cache"],
            cwd=str(root), capture_output=True, text=True, timeout=300)
        if out.returncode != 0:
            fails.append("todo-graph build-and-validate failed")
    except Exception:
        fails.append("todo-graph rebuild unavailable")
    # Content-bound build receipt: green build over the CURRENT build inputs.
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "overnight_receipts", str(root / "scripts/overnight/receipts.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        ok, why = mod.check_build(root)
        if not ok:
            fails.append(f"build receipt not content-valid ({why})")
    except Exception as exc:  # noqa: BLE001
        fails.append(f"build receipt check unavailable ({exc})")
    # No outstanding review obligation.
    review_state = root / ".claude/state/last-codex-review.json"
    if review_state.exists():
        try:
            rs = json.loads(review_state.read_text())
            if rs.get("received") is not True:
                fails.append("outstanding Codex review not yet received")
        except (OSError, ValueError):
            fails.append("last-codex-review.json unreadable")
    if state.get("waiting"):
        fails.append("a WAITING_REVIEW wait is still declared")
    return fails


def _live_oracle_status():
    """Best-effort live triage-oracle verdict for the Stop fallback.

    Returns the oracle's `status` string (DONE / BLOCKED / NEEDS_WORK) or None
    on any failure -- callers treat None as "keep blocking" (fail-closed).
    """
    try:
        out = subprocess.run(
            [sys.executable, str(repo_root() / ".claude/hooks/sequencer_triage.py"),
             "--next"],
            capture_output=True, text=True, timeout=60, cwd=str(repo_root()))
        if out.returncode != 0:
            return None
        return json.loads(out.stdout.strip()).get("status")
    except Exception:
        return None


def handle_stop():
    # The guard governs ONLY the headless unattended run -- an interactive
    # operator session is never trapped (it can stop and disarm freely).
    if not is_headless():
        return 0
    # The ONLY graceful self-stop is an oracle-verified FIXPOINT, recorded by an
    # exclusive sentinel that `run_phase_guard.py fixpoint` writes only after the
    # triage oracle confirms zero remaining work. We key on the sentinel (and the
    # immutable env discriminator), NOT on any value the agent can edit: it
    # cannot fake the sentinel (creating it is a blocked self-teardown), cannot
    # disarm itself, and cannot clear the cursor. So no reserved decision, stale
    # blocker, or per-file block can ever talk the run into stopping.
    if FIXPOINT_SENTINEL.exists():
        return 0
    # The human's --disarm removes the master switch (and kills the service); if
    # the marker is already gone, the run has been disarmed -- allow the stop.
    if not ARMED_MARKER.exists():
        return 0
    state_early = load_state()
    # VERIFIED ROLLOVER (2026-07-11): the `rollover` verb machine-verified the
    # checkpoint (clean tree, pushed, graph OK, content-bound receipts, no
    # outstanding jobs) and spawned the relaunch watcher. Permit exactly this
    # one stop; the flag is one-shot (cleared by the next launch's `wake`) and
    # honored only while fresh. The RUN stays active -- the context rotates.
    ro = state_early.get("rollover") or {}
    if (isinstance(ro, dict) and ro.get("pending")
            and time.time() - (ro.get("epoch") or 0) < ROLLOVER_PENDING_FRESH_S):
        sys.stderr.write(
            "[sequencer] stop allowed: verified rollover checkpoint -- the "
            "watcher relaunches a fresh worker context immediately; the run "
            "stays ARMED and the cursor carries the state.\n")
        return 0
    # WAITING_REVIEW (2026-07-11): a declared structural wait lets the session
    # end INSTEAD of burning turns on 'Holding...' polls. The watcher (or the
    # watchdog fallback) wakes a fresh session when the artifacts complete.
    w = state_early.get("waiting") or {}
    if isinstance(w, dict) and w.get("artifacts"):
        if _wait_satisfied(w):
            sys.stderr.write(
                "[SEQ-WAIT-READY] artifacts complete -- read the verdict(s), "
                "receive the review, `run_phase_guard.py wake`, continue. "
                "Details: docs/infrastructure/hook-codes.md#seq-wait-ready\n")
            return 2
        if _wait_expired(w):
            sys.stderr.write(
                "[SEQ-WAIT-EXPIRED] wait timed out -- re-dispatch or defer "
                "with the diagnostic, `run_phase_guard.py wake`, continue. "
                "Details: docs/infrastructure/hook-codes.md#seq-wait-expired\n")
            return 2
        sys.stderr.write(
            "[sequencer] stop allowed: WAITING_REVIEW declared -- the "
            "session-watcher wakes a fresh session when the artifact(s) "
            "complete (watchdog as fallback). No model turns are spent "
            "waiting; the woken session must receive the verdict first.\n")
        return 0
    # Oracle-consulting fallback (runner-kit law 2, adopted 2026-07-03): a
    # session at a TRUE lifecycle end (oracle DONE, or BLOCKED once the 3-state
    # split reports only recoverable deferrals) may end WITHOUT the sentinel --
    # the sentinel is written by the fixpoint CLI, and a session that cannot
    # reach it (lock, confusion, truncation) would otherwise idle until
    # external reap. Ending here does NOT disarm: the marker persists, the
    # watchdog relaunches, and the next session (or the operator) finalizes
    # via `run_phase_guard.py fixpoint`. Fail-closed: any oracle error keeps
    # the hard block below.
    verdict = _live_oracle_status()
    if verdict in ("DONE", "BLOCKED"):
        sys.stderr.write(
            f"[sequencer] stop allowed: live triage oracle reports {verdict} "
            "(no implementable work right now). The run stays ARMED -- "
            "finalize with `python3 .claude/hooks/run_phase_guard.py fixpoint` "
            "(DONE) or let the watchdog retry (BLOCKED).\n")
        return 0
    state = load_state()
    sys.stderr.write(
        f"[SEQ-STOP] do not stop (pass={state.get('pass_no')} "
        f"file={state.get('file')} phase={state.get('phase')}). Defer-and-"
        "advance, or declare a structural wait (`wait` verb) / verified "
        "`rollover`. Re-invoke Skill(overnight-sequencer) and continue. "
        "Details: docs/infrastructure/hook-codes.md#seq-stop")
    return 2


# ---- CLI (the sequencer skill drives transitions) ----

def _emit_anchor(state) -> None:
    """Best-effort one-line situational anchor on stderr; never raises.

    Only fires for an active run so interactive `status` calls stay quiet.
    """
    try:
        if not state.get("active"):
            return
        import runner_status
        sys.stderr.write("[sequencer] " + runner_status.anchor_line(repo_root()) + "\n")
    except Exception:
        pass


def cli(argv):
    cmd = argv[0] if argv else "status"
    state = load_state()
    if cmd == "start":
        state = {
            "active": True, "phase": "PREFLIGHT", "pass_no": 1,
            "domain": None, "file": None, "section_idx": 0,
            "progress_this_pass": False,
            "started_at": argv[1] if len(argv) > 1 else "unknown",
            "updated_at": argv[1] if len(argv) > 1 else "unknown",
        }
        save_state(state)
        # NOTE: the armed marker is intentionally NOT removed here. It is the
        # persistent master switch -- it must outlive every watchdog relaunch so
        # a fresh agent is always redirected back onto overnight-sequencer and
        # can never voluntarily stop. Only `--disarm` or an oracle-verified
        # FIXPOINT removes it. (Removing it on start was the death-thrash bug:
        # once gone, a reset cursor left the guard fully inert.)
        # Clear any stale FIXPOINT sentinel from a prior run: a starting run is by
        # definition not complete, and a leftover sentinel would let Stop succeed.
        try:
            FIXPOINT_SENTINEL.unlink()
        except FileNotFoundError:
            pass
        print("[sequencer] started: pass 1, PREFLIGHT (armed marker kept)",
              file=sys.stderr)
        return 0
    if cmd == "status":
        print(json.dumps(state, indent=1))
        _emit_anchor(state)
        return 0
    if cmd == "phase":
        if len(argv) < 2 or argv[1] not in PHASES:
            print(f"[sequencer] phase needs one of {PHASES}", file=sys.stderr)
            return 1
        state["phase"] = argv[1]
        save_state(state)
        print(f"[sequencer] phase -> {argv[1]}", file=sys.stderr)
        _emit_anchor(state)
        return 0
    if cmd == "cursor":
        # cursor <domain> <file> [section_idx]
        if len(argv) >= 3:
            state["domain"], state["file"] = argv[1], argv[2]
            if len(argv) >= 4:
                state["section_idx"] = int(argv[3])
            save_state(state)
            print(f"[sequencer] cursor -> {argv[2]}", file=sys.stderr)
            return 0
        print("[sequencer] cursor needs <domain> <file> [idx]", file=sys.stderr)
        return 1
    if cmd == "progress":
        state["progress_this_pass"] = True
        save_state(state)
        return 0
    if cmd == "next-pass":
        state["pass_no"] = int(state.get("pass_no", 1)) + 1
        state["progress_this_pass"] = False
        state["phase"] = "TRIAGE"
        save_state(state)
        print(f"[sequencer] -> pass {state['pass_no']}", file=sys.stderr)
        return 0
    if cmd == "fixpoint":
        # FIXPOINT is the ONLY path to a permanent stop (it ends the run and the
        # watchdog auto-disarms). It must NOT be fakeable: machine-verify via the
        # triage oracle (graph-truth, freshly rebuilt) that ZERO work remains.
        # If anything remains, REFUSE -- the run stays active and keeps looping.
        import subprocess
        root = repo_root()
        try:
            subprocess.run(
                ["bash", "scripts/todo-graph/build-and-validate.sh", "--keep-cache"],
                cwd=root, capture_output=True, text=True, timeout=300)
            out = subprocess.run(
                [sys.executable, str(HOOK_DIR / "sequencer_triage.py"), "--next"],
                cwd=root, capture_output=True, text=True, timeout=120)
            nxt = json.loads(out.stdout or "{}")
        except Exception as e:  # noqa: BLE001
            print(f"[sequencer] fixpoint REFUSED: oracle check failed ({e}). "
                  "Fail safe -- keep running, do NOT finish.", file=sys.stderr)
            return 1
        if nxt.get("status") != "DONE":
            print(f"[sequencer] fixpoint REFUSED: the oracle still reports work "
                  f"(status={nxt.get('status')}, file={nxt.get('file')}). The run "
                  "is NOT done -- continue the loop. FIXPOINT is only valid when "
                  "`sequencer_triage.py --next` returns DONE.", file=sys.stderr)
            return 1
        state["phase"] = "FIXPOINT"
        state["active"] = False
        save_state(state)
        # Genuine, oracle-verified completion is the one self-terminating exit:
        # write the exclusive sentinel (this is the ONLY place it is created) so
        # handle_stop will permit the stop, and remove the master switch so the
        # next watchdog tick does not relaunch into a re-entry loop.
        FIXPOINT_SENTINEL.parent.mkdir(parents=True, exist_ok=True)
        FIXPOINT_SENTINEL.write_text("oracle-verified: no remaining work\n",
                                     encoding="utf-8")
        try:
            ARMED_MARKER.unlink()
        except FileNotFoundError:
            pass
        print("[sequencer] FIXPOINT verified by oracle (no remaining work) -- "
              "run complete; sentinel written, armed marker removed",
              file=sys.stderr)
        return 0
    if cmd == "clear":
        save_state({"active": False})
        print(f"[sequencer] cleared: {' '.join(argv[1:]) or 'no reason'}", file=sys.stderr)
        return 0
    if cmd == "relifecycle":
        # One-shot Stage 1-2 override for a mature file that grew a genuinely
        # NEW section. Consumed by the next Stage 1-2 skill invocation.
        reason = " ".join(argv[1:]).strip()
        if len(reason) < 12:
            print("[sequencer] relifecycle needs a reason (>= 12 chars) naming "
                  "the new section", file=sys.stderr)
            return 1
        state["lifecycle_override"] = reason
        save_state(state)
        print(f"[sequencer] lifecycle override recorded: {reason}", file=sys.stderr)
        return 0
    if cmd == "wait":
        # wait <timeout_s> <reason> <path> <pattern> [<path> <pattern>...]
        if len(argv) < 5 or (len(argv) - 3) % 2 != 0:
            print("[sequencer] usage: wait <timeout_s> <reason> <path> "
                  "<pattern> [<path> <pattern>...]", file=sys.stderr)
            return 1
        try:
            timeout_s = max(WAIT_TIMEOUT_MIN_S,
                            min(WAIT_TIMEOUT_MAX_S, int(argv[1])))
        except ValueError:
            print("[sequencer] wait: timeout_s must be an integer", file=sys.stderr)
            return 1
        arts = [{"path": argv[i], "pattern": argv[i + 1]}
                for i in range(3, len(argv), 2)]
        waiting = {"reason": argv[2], "since_epoch": int(time.time()),
                   "timeout_s": timeout_s, "artifacts": arts}
        if _wait_satisfied(waiting):
            print("[sequencer] wait REFUSED: every artifact already matches "
                  "its completion pattern -- nothing to wait for. Read the "
                  "verdict(s) and continue.", file=sys.stderr)
            return 1
        # Replace any prior wait (best-effort kill of its watcher). pids <= 1
        # are sentinels (1 = systemd-run scope, cleaned up by --collect;
        # 0 = spawn failure) -- signalling them would hit init or our own
        # process group.
        prior = state.get("waiting") or {}
        try:
            prior_pid = int(prior.get("watcher_pid") or 0)
        except (TypeError, ValueError):
            prior_pid = 0
        if prior_pid > 1:
            try:
                os.kill(prior_pid, 15)
            except OSError:
                pass
        waiting["watcher_pid"] = _spawn_watcher("wait")
        state["waiting"] = waiting
        state.pop("woke_from_wait", None)
        save_state(state)
        print(f"[sequencer] WAITING_REVIEW declared ({len(arts)} artifact(s), "
              f"timeout {timeout_s}s, watcher pid {waiting['watcher_pid']}). "
              "Final-answer now with a one-line status -- the Stop hook "
              "permits this stop and the watcher wakes a fresh session when "
              "the artifact(s) complete.", file=sys.stderr)
        return 0
    if cmd == "wait-ready":
        # Launcher/watcher query. Exit 0 = proceed with a launch (no wait, or
        # artifacts ready, or wait expired); exit 3 = still waiting.
        w = state.get("waiting") or {}
        if not (isinstance(w, dict) and w.get("artifacts")):
            print(json.dumps({"waiting": False}))
            return 0
        ready = _wait_satisfied(w)
        expired = _wait_expired(w)
        print(json.dumps({"waiting": True, "ready": ready, "expired": expired,
                          "reason": w.get("reason")}))
        return 0 if (ready or expired) else 3
    if cmd == "wake":
        # Called by the launcher right before spinning up Claude: consume the
        # wait and any pending rollover flag, leaving a one-shot note the
        # fresh session reads via `status`.
        woke = {}
        w = state.pop("waiting", None)
        if isinstance(w, dict) and w.get("artifacts"):
            woke = {"reason": w.get("reason"),
                    "artifacts": [a.get("path") for a in w["artifacts"]],
                    "ready": _wait_satisfied(w), "expired": _wait_expired(w),
                    "epoch": int(time.time())}
            state["woke_from_wait"] = woke
        if isinstance(state.get("rollover"), dict):
            state.pop("rollover", None)
        save_state(state)
        print(json.dumps({"woke_from_wait": woke or None}))
        return 0
    if cmd == "rollover":
        # Verified worker-context rotation. Machine gates decide; on success
        # the Stop hook permits exactly one stop and the watcher relaunches.
        if not state.get("active"):
            print("[sequencer] rollover REFUSED: no active run", file=sys.stderr)
            return 1
        fails = _rollover_failures(repo_root(), state)
        if fails:
            print("[sequencer] rollover REFUSED (checkpoint not verified):\n"
                  + "\n".join(f"  - {f}" for f in fails)
                  + "\nFinish/clean these first, or continue in-session.",
                  file=sys.stderr)
            return 1
        state["rollover"] = {"pending": True, "epoch": int(time.time()),
                             "watcher_pid": _spawn_watcher("relaunch")}
        save_state(state)
        print("[sequencer] rollover VERIFIED: clean tree, pushed, graph OK, "
              "receipts content-valid, no outstanding jobs. Final-answer now "
              "with a one-line checkpoint summary -- the watcher relaunches "
              "a fresh worker context; the run stays armed and the cursor "
              "carries the state.", file=sys.stderr)
        return 0
    if cmd == "selftest":
        return selftest()
    print(f"[sequencer] unknown command: {cmd}", file=sys.stderr)
    return 1


def selftest():
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    H = True  # headless: the guard only governs the unattended run
    # Interactive sessions (headless=False) are NEVER constrained -- this is the
    # anti-trap invariant that lets the operator share the repo hooks.
    a, _ = evaluate("AskUserQuestion", {}, {"active": True, "phase": "SECTIONS"}, headless=False)
    check(a, "interactive AskUserQuestion blocked (must never trap the operator)")
    a, _ = evaluate("Skill", {"skill": "implement-todo-section"}, {"active": True, "phase": "VALIDATE"}, headless=False)
    check(a, "interactive Skill phase-blocked (must never trap the operator)")
    a, _ = evaluate("Bash", {"command": "bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm"},
                    {"active": True, "phase": "SECTIONS"}, headless=False)
    check(a, "interactive --disarm blocked (the human MUST be able to disarm)")

    # AskUserQuestion always blocked in the headless run.
    for ph in PHASES:
        a, _ = evaluate("AskUserQuestion", {}, {"active": True, "phase": ph}, headless=H)
        check(not a, f"AskUserQuestion allowed in {ph}")
    # Inactive + unarmed headless allows everything.
    a, _ = evaluate("AskUserQuestion", {}, {"active": False}, headless=H)
    check(a, "AskUserQuestion blocked while headless run inactive+unarmed")
    # Headless self-teardown is blocked across all known shapes; the legit
    # oracle-gated `fixpoint` and ordinary phase transitions are NOT.
    for cmd in ("bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm",
                "bash scripts/overnight-arm.sh todo/x.md --disarm",
                "python3 .claude/hooks/run_phase_guard.py clear oops",
                "python3 .claude/hooks/run_phase_guard.py phase FIXPOINT",
                "rm -f .claude/state/sequencer-armed",
                "rm .claude/state/sequencer-run.json",
                "echo '{}' > .claude/state/sequencer-run.json",
                "python3 -c \"import os; os.remove('.claude/state/sequencer-fixpoint')\"",
                "touch .claude/state/sequencer-fixpoint",
                "cp /tmp/fake .claude/state/sequencer-fixpoint",
                "dd if=/dev/zero of=.claude/state/sequencer-fixpoint bs=1 count=1",
                "systemctl --user stop overnight-impossible-os.service",
                "systemctl --user disable overnight-impossible-os-watchdog.timer",
                "pkill -f 'claude -p'"):
        a, _ = evaluate("Bash", {"command": cmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(not a, f"headless self-teardown allowed: {cmd}")
    for okcmd in ("python3 .claude/hooks/run_phase_guard.py fixpoint",
                  "python3 .claude/hooks/run_phase_guard.py phase SECTIONS",
                  "python3 .claude/hooks/run_phase_guard.py next-pass",
                  "git commit -m 'x' && cat .claude/state/sequencer-run.json"):
        a, _ = evaluate("Bash", {"command": okcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(a, f"legit guard/bash command blocked as self-teardown: {okcmd}")
    # implement-todo-section blocked in VALIDATE, allowed in SECTIONS.
    a, _ = evaluate("Skill", {"skill": "implement-todo-section"}, {"active": True, "phase": "VALIDATE"}, headless=H)
    check(not a, "implement-todo-section allowed in VALIDATE")
    a, _ = evaluate("Skill", {"skill": "implement-todo-section"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(a, "implement-todo-section blocked in SECTIONS")
    # validate-todo-file allowed in VALIDATE, blocked in SECTIONS.
    a, _ = evaluate("Skill", {"name": "validate-todo-file"}, {"active": True, "phase": "VALIDATE"}, headless=H)
    check(a, "validate-todo-file blocked in VALIDATE")
    a, _ = evaluate("Skill", {"name": "validate-todo-file"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(not a, "validate-todo-file allowed in SECTIONS")
    # gap-audit-todo only in GAP_AUDIT.
    a, _ = evaluate("Skill", {"skill": "gap-audit-todo"}, {"active": True, "phase": "GAP_AUDIT"}, headless=H)
    check(a, "gap-audit-todo blocked in GAP_AUDIT")
    a, _ = evaluate("Skill", {"skill": "gap-audit-todo"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(not a, "gap-audit-todo allowed in SECTIONS (should be GAP_AUDIT only)")
    # Non-sequence skill + Bash/Edit pass in SECTIONS.
    a, _ = evaluate("Skill", {"skill": "kernel-code-quality"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(a, "inner-pipeline skill blocked in SECTIONS")
    a, _ = evaluate("Bash", {"command": "git commit"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(a, "Bash blocked in SECTIONS")
    # legacy System A skill blocked everywhere active.
    a, _ = evaluate("Skill", {"skill": "overnight-todo-runner"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(not a, "legacy overnight-todo-runner allowed inside a sequencer run")
    # Armed-but-not-started: only overnight-sequencer skill allowed.
    a, _ = evaluate("Skill", {"skill": "overnight-sequencer"}, {"active": False}, armed=True, headless=H)
    check(a, "overnight-sequencer blocked while armed")
    a, _ = evaluate("Skill", {"name": "overnight-runner:start"}, {"active": False}, armed=True, headless=H)
    check(not a, "generic overnight-runner:start allowed while armed (should redirect)")
    a, _ = evaluate("Bash", {"command": "python3 .claude/hooks/sequencer_triage.py --next"},
                    {"active": False}, armed=True, headless=H)
    check(a, "Bash blocked while armed (setup needs it)")
    a, _ = evaluate("AskUserQuestion", {}, {"active": False}, armed=True, headless=H)
    check(not a, "AskUserQuestion allowed while armed")
    # Armed but cursor reset to inactive (the death-thrash condition): the run is
    # still governed -- the redirect still fires off the persistent marker.
    a, _ = evaluate("Skill", {"name": "overnight-runner:start"}, {"active": False}, armed=True, headless=H)
    check(not a, "armed+inactive run went ungoverned (death-thrash regression)")
    # Not armed, not active (headless): everything passes.
    a, _ = evaluate("Skill", {"skill": "anything"}, {"active": False}, armed=False, headless=H)
    check(a, "skill blocked while neither armed nor active")

    # Lifecycle routing (Stage 0): Stage 1-2 skills blocked on a mature file.
    a, _ = evaluate("Skill", {"skill": "validate-todo-file"},
                    {"active": True, "phase": "VALIDATE"}, headless=H,
                    stages_done=True)
    check(not a, "validate-todo-file allowed on stages_1_2_done file")
    a, _ = evaluate("Skill", {"skill": "gap-audit-todo"},
                    {"active": True, "phase": "GAP_AUDIT"}, headless=H,
                    stages_done=True)
    check(not a, "gap-audit-todo allowed on stages_1_2_done file")
    a, _ = evaluate("Skill", {"skill": "validate-todo-file"},
                    {"active": True, "phase": "VALIDATE",
                     "lifecycle_override": "new section 21 appeared"},
                    headless=H, stages_done=True)
    check(a, "relifecycle override did not unblock validate-todo-file")
    a, _ = evaluate("Skill", {"skill": "validate-todo-file"},
                    {"active": True, "phase": "VALIDATE"}, headless=H,
                    stages_done=False)
    check(a, "validate-todo-file blocked on an immature file")
    a, _ = evaluate("Skill", {"skill": "validate-todo-file"},
                    {"active": True, "phase": "VALIDATE"}, headless=False,
                    stages_done=True)
    check(a, "interactive validate-todo-file blocked (must never trap operator)")

    # Structural-wait helpers: artifact matching + expiry truth table.
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".out", delete=False) as tf:
        tf.write("dispatch running...\nTurn completed after 214s\n")
        wait_art = tf.name
    try:
        check(_artifact_matches(wait_art, "Turn completed"),
              "_artifact_matches missed a present completion marker")
        check(not _artifact_matches(wait_art, "NO_SUCH_MARKER"),
              "_artifact_matches false-positive")
        check(not _artifact_matches("/nonexistent/artifact.out", "x"),
              "_artifact_matches true on missing file")
        w_ok = {"artifacts": [{"path": wait_art, "pattern": "Turn completed"}],
                "since_epoch": int(time.time()), "timeout_s": 3600}
        check(_wait_satisfied(w_ok), "_wait_satisfied false on matched artifact")
        w_pending = {"artifacts": [{"path": wait_art, "pattern": "NOPE"}],
                     "since_epoch": int(time.time()), "timeout_s": 3600}
        check(not _wait_satisfied(w_pending), "_wait_satisfied true on unmatched")
        check(not _wait_expired(w_pending), "fresh wait reported expired")
        w_old = dict(w_pending, since_epoch=int(time.time()) - 7200)
        check(_wait_expired(w_old), "expired wait reported fresh")
    finally:
        os.unlink(wait_art)

    if fails:
        for f in fails:
            print("FAIL:", f, file=sys.stderr)
        print(f"run_phase_guard selftest: {len(fails)} failure(s)", file=sys.stderr)
        return 1
    print("run_phase_guard selftest OK")
    return 0


def main(argv):
    mode = argv[1] if len(argv) > 1 else "pretool"
    if mode == "pretool":
        return handle_pretool()
    if mode == "stop":
        return handle_stop()
    return cli(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
