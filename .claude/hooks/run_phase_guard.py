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
  CLI      start / status / phase / cursor / progress / next-pass / fixpoint /
            clear / selftest / relifecycle / rollover -- the
            overnight-sequencer skill drives phase transitions (and verified
            context rotations) through these.

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


# Codex write/sandbox-escalation markers (mirrors
# codex_review_completed._CODEX_WRITE_FLAGS). A dispatch carrying any of these
# is Codex making CHANGES, not reviewing -- interactive-only per Option A.
_CODEX_WRITE_MARKERS = ("--write", "--full-auto", "--yolo",
                        "--dangerously-bypass-approvals-and-sandbox")
_CODEX_WRITE_SANDBOX = ("workspace-write", "danger-full-access")


def _is_codex_write_dispatch(cmd):
    """True iff cmd is a Codex dispatch AND carries a write marker. Kept
    self-contained (a frozenset/shlex scan) so the guard has no heavy import
    on its hot pretool path."""
    c = cmd or ""
    if not ("codex" in c):  # cheap prefilter; not a codex surface -> not write
        return False
    import shlex
    try:
        toks = shlex.split(c)
    except ValueError:
        toks = c.split()
    # confirm it's actually a codex dispatch surface, not prose mentioning it
    is_codex = any(t == "codex" or t.endswith("codex-companion.mjs")
                   or t.endswith("codex-dispatch.sh")
                   or t.endswith("codex-bg-dispatch.sh")
                   or t.endswith("codex-dispatch-with-files.sh") for t in toks)
    if not is_codex:
        return False
    for i, t in enumerate(toks):
        if t in _CODEX_WRITE_MARKERS:
            return True
        if t in ("--sandbox", "-s"):
            if (toks[i + 1] if i + 1 < len(toks) else "") in _CODEX_WRITE_SANDBOX:
                return True
        if t.startswith("--sandbox=") and t.split("=", 1)[1] in _CODEX_WRITE_SANDBOX:
            return True
    return False


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
    """ATOMIC write (tmp + os.replace). Truncate-and-write let a concurrent
    reader (watcher poll, launcher gate) see partial JSON and misread it as
    'no active wait' -- which bypasses an unfinished review (2026-07-11)."""
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE_PATH.with_suffix(f".{os.getpid()}.tmp")
    with tmp.open("w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=1)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(str(tmp), str(STATE_PATH))


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

    # Codex WRITE is interactive-only (Option A, 2026-07-11). The unattended
    # run dispatches READ-ONLY reviews; a write-capable Codex mutating the
    # tree with no per-step human authorship crosses the autonomous-agent
    # boundary (CLAUDE.md). The headless main session is physically blocked
    # from write dispatches; interactive sessions (guard inert) are not.
    if tool_name == "Bash" and _is_codex_write_dispatch(tool_input.get("command", "")):
        return False, (
            "[SEQ-CODEX-WRITE] blocked: Codex write (`task --write` / sandbox "
            "override) is INTERACTIVE-ONLY (autonomous-agent boundary). The "
            "unattended run uses read-only reviews only. Implement the change "
            "yourself and dispatch a read-only review, or defer for an "
            "operator's interactive rescue. "
            "Details: docs/infrastructure/hook-codes.md#seq-codex-write")

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


# ---- verified rollover (context rotation) ----------------------------------
#
# ROLLOVER: after a section is shipped+reviewed+pushed+stamped, a fresh worker
# context is cheaper than a long-tail one. `rollover` machine-verifies the
# checkpoint (clean tree, pushed, graph rebuild OK, content-bound receipts, no
# outstanding jobs) and only then permits ONE clean session exit; the *:0/10
# watchdog relaunches a fresh session on its next tick. The RUN stays active --
# only the worker context rotates (doctrine: run-active vs context-rotates).
# There is NO custom relaunch watcher: the watchdog is the sole, systemd-owned
# relaunch mechanism (the structural-wait watcher apparatus was removed
# 2026-07-11 after it deadlocked the runner -- a session polling a review
# in-session in a single blocking Bash call costs ~0 tokens, so exit-and-wake
# was solving a non-problem while adding a fatal process-lifecycle bug class).

ROLLOVER_PENDING_FRESH_S = 1800  # stale pending flags stop permitting stops


def _rollover_failures(root: Path, state: dict) -> list:
    """Machine gates for a verified rollover checkpoint. Every failure is a
    reason the worker context may NOT rotate yet."""
    fails = []
    try:
        # UNTRACKED FILES COUNT (-uall): a stranded new file is uncommitted
        # state a fresh worker must not inherit silently. The old `-uno`
        # called a tree with untracked junk "clean" (2026-07-11).
        out = subprocess.run(["git", "status", "--porcelain", "-uall"],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            fails.append("git status unavailable")
        elif out.stdout.strip():
            dirty = [ln for ln in out.stdout.splitlines() if ln.strip()]
            untracked = sum(1 for ln in dirty if ln.startswith("??"))
            fails.append(f"tree not clean ({len(dirty)} change(s), "
                         f"{untracked} untracked)")
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
        # Test + smoke receipts: a rollover asserts a green checkpoint, so the
        # test suite (and, for boot-path work, the smoke test) must be
        # content-valid over the current tree, not just the build.
        try:
            t_ok, t_why = mod.check_suite(root, "all")
            if not t_ok:
                fails.append(f"test receipt not content-valid ({t_why})")
        except Exception:
            fails.append("test receipt missing (record with receipts.py "
                         "record-suite . all after a green test.sh)")
        try:
            s_ok, s_why = mod.check_suite(root, "smoke")
            if not s_ok:
                fails.append(f"smoke receipt not content-valid ({s_why})")
        except Exception:
            fails.append("smoke receipt missing (record with receipts.py "
                         "record-suite . smoke after a green test-smoke.sh)")
    except Exception as exc:  # noqa: BLE001
        fails.append(f"build/test receipt check unavailable ({exc})")
    # No outstanding review obligation. Fail CLOSED on unreadable state.
    review_state = root / ".claude/state/last-codex-review.json"
    if review_state.exists():
        try:
            rs = json.loads(review_state.read_text())
            if rs.get("received") is not True:
                fails.append("outstanding Codex review not yet received")
        except (OSError, ValueError):
            fails.append("last-codex-review.json unreadable (fail-closed)")
    # No outstanding background jobs: a running codex-review or seq-watcher
    # unit means work is in flight the rollover would strand.
    try:
        out = subprocess.run(
            ["systemctl", "--user", "list-units", "--state=active",
             "--no-legend", "codex-rev-*", "seq-watcher-*"],
            capture_output=True, text=True, timeout=10)
        jobs = [ln.split()[0] for ln in out.stdout.splitlines() if ln.strip()]
        if jobs:
            fails.append(f"outstanding background job(s) still active: "
                         f"{', '.join(jobs[:4])}")
    except Exception:
        pass  # systemctl absent -> cannot enumerate; not a hard fail
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
    # VERIFIED ROLLOVER: the `rollover` verb machine-verified the checkpoint
    # (clean tree, pushed, graph OK, content-bound receipts, no outstanding
    # jobs). Permit exactly this one stop; the *:0/10 watchdog relaunches a
    # fresh session on its next tick. The flag is one-shot (cleared on the next
    # launch's `start`) and honored only while fresh. The RUN stays active.
    ro = state_early.get("rollover") or {}
    if (isinstance(ro, dict) and ro.get("pending")
            and time.time() - (ro.get("epoch") or 0) < ROLLOVER_PENDING_FRESH_S):
        sys.stderr.write(
            "[sequencer] stop allowed: verified rollover checkpoint -- the "
            "watchdog relaunches a fresh worker context on its next tick; the "
            "run stays ARMED and the cursor carries the state.\n")
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
    if cmd == "rollover":
        # Verified worker-context rotation. Machine gates decide; on success
        # the Stop hook permits exactly one stop and the *:0/10 watchdog
        # relaunches a fresh session on its next tick.
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
        # Atomic pending flag; the Stop hook permits ONE exit and the *:0/10
        # watchdog relaunches a fresh session. No custom watcher.
        state["rollover"] = {"pending": True, "epoch": int(time.time())}
        save_state(state)
        print("[sequencer] rollover VERIFIED: clean tree, pushed, graph OK, "
              "receipts content-valid, no outstanding jobs. Final-answer now "
              "with a one-line checkpoint summary and END the turn -- the "
              "watchdog relaunches a fresh worker context on its next tick; "
              "the run stays armed and the cursor carries the state.",
              file=sys.stderr)
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

    # Codex WRITE is interactive-only: blocked in the headless active run,
    # never blocked interactively; read-only reviews always pass.
    for wcmd in ("node /x/codex-companion.mjs task --write 'fix it'",
                 "codex exec --sandbox workspace-write 'do it'",
                 "codex --full-auto 'go'",
                 "bash scripts/codex-dispatch.sh 'x' && codex task --write 'y'"):
        a, _ = evaluate("Bash", {"command": wcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(not a, f"headless Codex write allowed: {wcmd[:40]}")
        a, _ = evaluate("Bash", {"command": wcmd}, {"active": True, "phase": "SECTIONS"}, headless=False)
        check(a, f"interactive Codex write blocked (must not): {wcmd[:40]}")
    for rcmd in ("node /x/codex-companion.mjs adversarial-review 'review'",
                 "bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: design] todo/x.md b'",
                 "node /x/codex-companion.mjs task --background 'review-kind design'"):
        a, _ = evaluate("Bash", {"command": rcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(a, f"headless read-only review blocked: {rcmd[:40]}")

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
